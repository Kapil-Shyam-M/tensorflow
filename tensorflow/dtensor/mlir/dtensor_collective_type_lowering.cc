/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <memory>
#include <optional>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FormatVariadic.h"
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/Types.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Support/DebugStringHelper.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/transforms/collection_ops_util.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/dtensor/cc/constants.h"
#include "tensorflow/dtensor/cc/dtensor_utils.h"
#include "tensorflow/dtensor/cc/tensor_layout.h"
#include "tensorflow/dtensor/mlir/collectives_common.h"
#include "tensorflow/dtensor/mlir/device_utils.h"
#include "tensorflow/dtensor/mlir/dtensor_dialect/ir/dialect.h"
#include "tensorflow/dtensor/mlir/dtensor_dialect/ir/dtensor_attributes.h"
#include "tensorflow/dtensor/mlir/dtensor_location.h"
#include "tensorflow/dtensor/mlir/group_assignment.h"
#include "tensorflow/dtensor/mlir/ir/tf_dtensor.h"
#include "tensorflow/dtensor/mlir/layout_parsing.h"
#include "tensorflow/dtensor/mlir/spmd_expander_common.h"
#include "tensorflow/dtensor/mlir/value_utils.h"

namespace tensorflow {
namespace dtensor {
namespace {

#define GEN_PASS_DEF_DTENSORCOLLECTIVETYPELOWERINGPASS
#include "tensorflow/dtensor/mlir/dtensor_passes.h.inc"

mlir::LogicalResult WrapOpWithCasts(const mlir::RankedTensorType& input_type,
                                    const mlir::RankedTensorType& output_type,
                                    mlir::Operation* reduce_op) {
  mlir::OpBuilder builder(reduce_op);
  auto intermediate_type = mlir::RankedTensorType::get(
      output_type.getShape(), input_type.getElementType());

  const mlir::Location loc = reduce_op->getLoc();
  mlir::TF::CastOp cast_to_long = builder.create<mlir::TF::CastOp>(
      loc, input_type, reduce_op->getOperand(0));
  reduce_op->setOperand(0, cast_to_long.getY());
  reduce_op->getResult(0).setType(intermediate_type);

  mlir::Value result = reduce_op->getResult(0);
  builder.setInsertionPointAfter(reduce_op);
  mlir::TF::CastOp cast_to_original =
      builder.create<mlir::TF::CastOp>(loc, output_type, result);
  StatusOr<Layout> result_layout =
      ExtractRequiredSingleLayoutFromOp(result.getDefiningOp());

  if (!result_layout.ok()) {
    return reduce_op->emitOpError(result_layout.status().message());
  }
  SetSingleLayoutOnOp(cast_to_original, *result_layout);
  reduce_op->getResult(0).replaceAllUsesExcept(cast_to_original.getY(),
                                               cast_to_original);
  return mlir::success();
}

template <class ReduceOpType>
mlir::LogicalResult ConvertShortIntReduce(ReduceOpType reduce_op) {
  mlir::OpBuilder builder(reduce_op);
  StatusOr<Layout> output_layout = ExtractRequiredSingleLayoutFromOp(reduce_op);
  if (!output_layout.ok()) {
    return reduce_op.emitOpError(output_layout.status().message());
  }
  const mlir::Type output_type = reduce_op.getResult().getType();
  const mlir::Type input_type = reduce_op.getOperand(0).getType();

  // Handle bools by first casting to int32 and swapping All/Any for Min/Max.
  const mlir::TensorType& tensor_input_type =
      input_type.dyn_cast<mlir::TensorType>();
  const mlir::TensorType& tensor_output_type =
      output_type.dyn_cast<mlir::TensorType>();
  if (!tensor_input_type) return mlir::success();
  if (!tensor_output_type) return mlir::success();

  if (tensor_input_type.getElementType().isInteger(1)) {
    if (reduce_op.getReduceOpAttr().getValue().str() == kReduceOpAll)
      reduce_op.setReduceOpAttr(
          builder.getStringAttr(std::string(kReduceOpMin)));
    else if (reduce_op.getReduceOpAttr().getValue().str() == kReduceOpAny)
      reduce_op.setReduceOpAttr(
          builder.getStringAttr(std::string(kReduceOpMax)));
    else if (reduce_op.getReduceOpAttr().getValue().str() != kReduceOpMax &&
             reduce_op.getReduceOpAttr().getValue().str() != kReduceOpMin)
      return reduce_op.emitOpError()
             << "reduce for boolean only supports 'All'/'Min' or 'Any'/'Max' "
                "reduction. "
             << "Received '" << reduce_op.getReduceOpAttr().getValue().str()
             << "'";
  }
  if (mlir::isa<mlir::IntegerType>(tensor_input_type.getElementType())) {
    int32_t min_width = 64;
    if (output_layout->mesh().is_tpu_mesh()) {
      min_width = 32;
    }

    if (tensor_input_type.getElementType().getIntOrFloatBitWidth() >=
        min_width) {
      return mlir::success();
    }
    auto input_type = mlir::RankedTensorType::get(
        tensor_input_type.getShape(), builder.getIntegerType(min_width));

    auto output_type = mlir::RankedTensorType::get(
        tensor_output_type.getShape(), tensor_input_type.getElementType());
    return WrapOpWithCasts(input_type, output_type, reduce_op);
  }
  if (mlir::isa<mlir::BFloat16Type>(tensor_input_type.getElementType())) {
    if (output_layout->mesh().is_tpu_mesh()) {
      return mlir::success();
    }
    auto input_type = mlir::RankedTensorType::get(tensor_input_type.getShape(),
                                                  builder.getF32Type());

    auto output_type = mlir::RankedTensorType::get(
        tensor_output_type.getShape(), tensor_input_type.getElementType());

    return WrapOpWithCasts(input_type, output_type, reduce_op);
  }
  return mlir::success();
}

// A Walk that allows mutatatoin inside parent.
template <typename FuncT, typename OpT = mlir::detail::first_argument<FuncT>>
mlir::LogicalResult MutatingWalk(mlir::Operation* parent, FuncT func) {
  llvm::SmallVector<OpT, 4> ops;
  parent->walk([&](OpT op) { ops.push_back(op); });
  for (auto op : ops) {
    if (mlir::failed(func(op))) {
      return mlir::LogicalResult::failure();
    }
  }
  return mlir::LogicalResult::success();
}

class DTensorCollectiveTypeLoweringPass
    : public impl::DTensorCollectiveTypeLoweringPassBase<
          DTensorCollectiveTypeLoweringPass> {
 public:
  void runOnOperation() override {
    mlir::func::FuncOp func = getOperation();


    if (mlir::failed(
            MutatingWalk(func, [&](mlir::TF::DTensorAllReduceOp all_reduce) {
              // Lower integer type all reduce
              return ConvertShortIntReduce(all_reduce);
            }))) {
      signalPassFailure();
    }

    if (mlir::failed(MutatingWalk(
            func, [&](mlir::TF::DTensorReduceScatterOp reduce_scatter) {
              // Lower integer type all reduce
              return ConvertShortIntReduce(reduce_scatter);
            }))) {
      signalPassFailure();
    }
  }
};

}  // namespace

std::unique_ptr<mlir::OperationPass<mlir::func::FuncOp>>
CreateDTensorCollectiveTypeLoweringPass() {
  return std::make_unique<DTensorCollectiveTypeLoweringPass>();
}

}  // namespace dtensor
}  // namespace tensorflow
