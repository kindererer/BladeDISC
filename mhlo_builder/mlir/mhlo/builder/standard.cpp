// Copyright 2021 The BladeDISC Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include "mlir/mhlo/builder/standard.h"

#include "mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/mhlo/builder/mlir_shape_builder.h"
#include "mlir/mhlo/builder/mlir_type_utils.h"
#include "mlir/mhlo/builder/mlir_utils.h"

namespace mlir {
namespace mhlo {
// Build a standard bool constant op from bool value
mlir::Value BuildStdConstForBool(mlir::OpBuilder& builder,
                                 const mlir::Location& loc, bool value) {
  return builder.create<mlir::ConstantOp>(loc, builder.getBoolAttr(value));
}

mlir::Value BuildStdConstForIndex(mlir::OpBuilder& builder,
                                  const mlir::Location& loc, int64_t value) {
  return builder.create<mlir::ConstantOp>(
      loc, builder.getIntegerAttr(builder.getIndexType(), value));
}

mlir::Value BuildStdConstForI32(mlir::OpBuilder& builder,
                                const mlir::Location& loc, int32_t value) {
  return builder.create<mlir::ConstantOp>(loc,
                                          builder.getI32IntegerAttr(value));
}

mlir::Value BuildStdConstForF64(mlir::OpBuilder& builder,
                                const mlir::Location& loc, double value) {
  return builder.create<mlir::ConstantOp>(loc, builder.getF64FloatAttr(value));
}

mlir::Value BuildStdConstForI64(mlir::OpBuilder& builder,
                                const mlir::Location& loc, int64_t value) {
  return builder.create<mlir::ConstantOp>(loc,
                                          builder.getI64IntegerAttr(value));
}

mlir::Value BuildStdConstLike(mlir::OpBuilder& builder,
                              const mlir::Location& loc, int64_t value,
                              mlir::Value other) {
  mlir::Type other_ty = other.getType();
  if (other_ty.isa<mlir::IndexType>()) {
    return BuildStdConstForIndex(builder, loc, value);
  } else if (other_ty.isa<mlir::IntegerType>()) {
    return builder.create<mlir::ConstantIntOp>(
        loc, value, other_ty.cast<mlir::IntegerType>().getWidth());
  } else {
    MHLO_THROW_ERROR("Invalid usage of BuildStdConstLike()");
    return {};
  }
}

mlir::Value BuildStdDimSizeOfTensor(mlir::OpBuilder& builder,
                                    const mlir::Location& loc,
                                    const mlir::Value& tensor,
                                    mlir_dim_t dim_index) {
  auto ranked_type = GetMilrRankedTensorType(tensor);
  auto rank = ranked_type.getRank();
  dim_index = NormalizeDimIndex(dim_index, rank);
  auto dim_size = ranked_type.getDimSize(dim_index);
  if (dim_size == mlir::ShapedType::kDynamicSize) {
    return builder.create<mlir::IndexCastOp>(
        loc, builder.create<tensor::DimOp>(loc, tensor, dim_index),
        builder.getIntegerType(64));
  } else {
    return BuildStdConstForI64(builder, loc, dim_size);
  }
}

SmallValueVec4 BuildStdDimSizeListOfTensor(mlir::OpBuilder& builder,
                                           const mlir::Location& loc,
                                           const mlir::Value& tensor,
                                           const SmallVec4<mlir_dim_t>& dims) {
  auto rank = GetRankOfMlirValue(tensor);
  auto norm_dims =
      dims.size() ? NormalizeDimIndex(dims, rank) : RangeIndices(0, rank);

  SmallValueVec4 shape_values;
  shape_values.reserve(rank);

  for (mlir_dim_t d : norm_dims) {
    shape_values.push_back(BuildStdDimSizeOfTensor(builder, loc, tensor, d));
  }
  return shape_values;
}

mlir::Value BuildStdSelectSigned(mlir::OpBuilder& builder,
                                 const mlir::Location& loc,
                                 const mlir::CmpIPredicate& predc,
                                 const mlir::Value& std_lhs,
                                 const mlir::Value& std_rhs) {
  auto cond = builder.create<mlir::CmpIOp>(loc, predc, std_lhs, std_rhs);
  auto selected = builder.create<mlir::SelectOp>(loc, cond, std_lhs, std_rhs);
  return selected;
}

mlir::Value BuildStdMaximumSigned(mlir::OpBuilder& builder,
                                  const mlir::Location& loc,
                                  const mlir::Value& std_lhs,
                                  const mlir::Value& std_rhs) {
  return BuildStdSelectSigned(builder, loc, mlir::CmpIPredicate::sge, std_lhs,
                              std_rhs);
}

mlir::Value BuildStdMinimumSigned(mlir::OpBuilder& builder,
                                  const mlir::Location& loc,
                                  const mlir::Value& std_lhs,
                                  const mlir::Value& std_rhs) {
  return BuildStdSelectSigned(builder, loc, mlir::CmpIPredicate::sle, std_lhs,
                              std_rhs);
}

mlir::Value BuildStdRemainderSigned(mlir::OpBuilder& builder,
                                    const mlir::Location& loc,
                                    const mlir::Value& std_lhs,
                                    const mlir::Value& std_rhs) {
  return builder.create<mlir::SignedRemIOp>(loc, std_lhs, std_rhs);
}

mlir::Value BuildStdAddSigned(mlir::OpBuilder& builder,
                              const mlir::Location& loc,
                              const mlir::Value& std_lhs,
                              const mlir::Value& std_rhs) {
  return builder.create<mlir::AddIOp>(loc, std_lhs, std_rhs);
}

mlir::Value BuildStdSubSigned(mlir::OpBuilder& builder,
                              const mlir::Location& loc,
                              const mlir::Value& std_lhs,
                              const mlir::Value& std_rhs) {
  return builder.create<mlir::SubIOp>(loc, std_lhs, std_rhs);
}

mlir::Value BuildStdMulSigned(mlir::OpBuilder& builder,
                              const mlir::Location& loc,
                              const mlir::Value& std_lhs,
                              const mlir::Value& std_rhs) {
  return builder.create<mlir::MulIOp>(loc, std_lhs, std_rhs);
}

mlir::Value BuildStdDivSigned(mlir::OpBuilder& builder,
                              const mlir::Location& loc,
                              const mlir::Value& std_lhs,
                              const mlir::Value& std_rhs) {
  return builder.create<mlir::SignedDivIOp>(loc, std_lhs, std_rhs);
}

mlir::Value BuildStdNegtive(mlir::OpBuilder& builder, const mlir::Location& loc,
                            const mlir::Value& std_val) {
  auto zero = BuildStdConstForI64(builder, loc, 0);
  return BuildStdSubSigned(builder, loc, zero, std_val);
}

llvm::Optional<mlir::Value> BuildCastStdConstScalarToHloConstTensor(
    mlir::OpBuilder& builder, const mlir::Location& loc,
    const mlir::Value& std_scalar) {
  auto def = llvm::dyn_cast<mlir::ConstantOp>(std_scalar.getDefiningOp());
  if (!def) {
    return llvm::None;
  }
  const mlir::Attribute& val_attr = def.value();
  auto scalar_ty = mlir::RankedTensorType::get({}, val_attr.getType());
  auto const_attr = mlir::DenseElementsAttr::get(scalar_ty, val_attr);
  auto result = builder.create<mlir::mhlo::ConstOp>(loc, const_attr);
  return result.getResult();
}

mlir::Value BuildStdScalarToHloTensor(mlir::OpBuilder& builder,
                                      const mlir::Location& loc,
                                      const mlir::Value& std_scalar) {
  auto const_val =
      BuildCastStdConstScalarToHloConstTensor(builder, loc, std_scalar);
  if (const_val) {
    return *const_val;
  }
  // Eventually, we would support std::scalar to mhlo::tensor conversion of
  // any type. But, we only support index to dimension tensor conversion
  // currently.
  // TODO: Add IntegerType Check (Only IntegerType Supported)
  auto elem_type = std_scalar.getType();
  mlir::Value dim_size =
      builder
          .create<mlir::IndexCastOp>(
              loc, BuildStdScalarToIndexType(builder, loc, std_scalar),
              elem_type)
          .getResult();
  SmallValueVec4 dim_sizes{dim_size};
  mlir::Value hlo_tensor =
      builder.create<mlir::tensor::FromElementsOp>(loc, elem_type, dim_sizes);
  hlo_tensor = BuildReshapeTensorToScalar(builder, loc, hlo_tensor);
  return hlo_tensor;
}

mlir::Value BuildStdScalarToIndexType(mlir::OpBuilder& builder,
                                      const mlir::Location& loc,
                                      const mlir::Value& dim_size) {
  auto dsize_type = dim_size.getType();
  MHLO_CHECK(dsize_type.isIndex() || dsize_type.isSignlessInteger(),
             "Type must be Integer or Index");
  if (!dsize_type.isIndex()) {
    return builder
        .create<mlir::IndexCastOp>(loc, dim_size, builder.getIndexType())
        .getResult();
  } else {
    return dim_size;
  }
}

SmallValueVec4 BuildStdScalarToHloDimType(mlir::OpBuilder& builder,
                                          const mlir::Location& loc,
                                          const SmallValueVec4& dim_sizes) {
  SmallValueVec4 new_dim_sizes;
  new_dim_sizes.reserve(dim_sizes.size());
  auto mhlo_dim_type = BuildMHloDimType(builder);
  for (auto dsize : dim_sizes) {
    auto dsize_type = dsize.getType();
    MHLO_CHECK(dsize_type.isIndex() || dsize_type.isSignlessInteger(),
               "Type must be Integer or Index");
    if (dsize_type != mhlo_dim_type) {
      if (!dsize_type.isIndex()) {
        dsize =
            builder
                .create<mlir::IndexCastOp>(loc, dsize, builder.getIndexType())
                .getResult();
      }
      dsize = builder.create<mlir::IndexCastOp>(loc, dsize, mhlo_dim_type)
                  .getResult();
    }
    new_dim_sizes.push_back(dsize);
  }
  return new_dim_sizes;
}

mlir::Value BuildStdScalarToHloTensor(mlir::OpBuilder& builder,
                                      const mlir::Location& loc,
                                      const SmallValueVec4& values) {
  // Eventually, we would support std::scalar to mhlo::tensor conversion of
  // any type. But, we only support index to dimension tensor conversion
  // currently.
  // TODO: Add IntegerType Check (Only IntegerType Supported)
  MHLO_CHECK(values.size() > 0, "values must not be empty");
  auto elem_type = values[0].getType();
  for (const auto& val : values) {
    MHLO_CHECK(val.getType() == elem_type, "values type must be the same");
  }
  mlir::Value hlo_tensor =
      builder.create<mlir::tensor::FromElementsOp>(loc, elem_type, values);
  return hlo_tensor;
}
}  // namespace mhlo
}  // namespace mlir
