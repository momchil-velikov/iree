// Copyright 2025 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Utils/IndexingUtils.h"
#include "mlir/Dialect/Utils/StaticValueUtils.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include "Passes.h"
namespace mlir {
#define GEN_PASS_DEF_DECOMPOSEMEMREFSPASS
#include "iree/compiler/Codegen/Common/Passes.h.inc"
} // namespace mlir

using namespace mlir;

static void setInsertionPointToStart(OpBuilder &builder, Value val) {
  if (auto *parentOp = val.getDefiningOp()) {
    builder.setInsertionPointAfter(parentOp);
  } else {
    builder.setInsertionPointToStart(val.getParentBlock());
  }
}

/// This is copied from static function affine::mlir::computeProduct.
/// TODO: enable this function in AffineOps.h
/// Return the product of `terms`, creating an `affine.apply` if any of them are
/// non-constant values. If any of `terms` is `nullptr`, return `nullptr`.
static OpFoldResult computeProduct(Location loc, OpBuilder &builder,
                                   ArrayRef<OpFoldResult> terms) {
  int64_t nDynamic = 0;
  SmallVector<Value> dynamicPart;
  AffineExpr result = builder.getAffineConstantExpr(1);
  for (OpFoldResult term : terms) {
    if (!term)
      return term;
    std::optional<int64_t> maybeConst = getConstantIntValue(term);
    if (maybeConst) {
      result = result * builder.getAffineConstantExpr(*maybeConst);
    } else {
      dynamicPart.push_back(cast<Value>(term));
      result = result * builder.getAffineSymbolExpr(nDynamic++);
    }
  }
  if (auto constant = dyn_cast<AffineConstantExpr>(result))
    return getAsIndexOpFoldResult(builder.getContext(), constant.getValue());
  return builder.create<affine::AffineApplyOp>(loc, result, dynamicPart)
      .getResult();
}

static std::tuple<Value, OpFoldResult, SmallVector<OpFoldResult>, OpFoldResult,
                  OpFoldResult>
getFlatOffsetAndStrides(OpBuilder &rewriter, Location loc, Value source,
                        ArrayRef<OpFoldResult> subOffsets,
                        ArrayRef<OpFoldResult> subStrides = {}) {
  auto sourceType = cast<MemRefType>(source.getType());
  auto sourceRank = static_cast<unsigned>(sourceType.getRank());

  memref::ExtractStridedMetadataOp newExtractStridedMetadata;
  {
    OpBuilder::InsertionGuard g(rewriter);
    setInsertionPointToStart(rewriter, source);
    newExtractStridedMetadata =
        rewriter.create<memref::ExtractStridedMetadataOp>(loc, source);
  }

  auto &&[sourceStrides, sourceOffset] = sourceType.getStridesAndOffset();

  auto getDim = [&](int64_t dim, Value dimVal) -> OpFoldResult {
    return ShapedType::isDynamic(dim) ? getAsOpFoldResult(dimVal)
                                      : rewriter.getIndexAttr(dim);
  };

  OpFoldResult origOffset =
      getDim(sourceOffset, newExtractStridedMetadata.getOffset());
  ValueRange sourceStridesVals = newExtractStridedMetadata.getStrides();
  OpFoldResult outmostDim =
      getDim(sourceType.getShape().front(),
             newExtractStridedMetadata.getSizes().front());

  SmallVector<OpFoldResult> origStrides;
  origStrides.reserve(sourceRank);

  SmallVector<OpFoldResult> strides;
  strides.reserve(sourceRank);

  AffineExpr s0 = rewriter.getAffineSymbolExpr(0);
  AffineExpr s1 = rewriter.getAffineSymbolExpr(1);
  for (auto i : llvm::seq(0u, sourceRank)) {
    OpFoldResult origStride = getDim(sourceStrides[i], sourceStridesVals[i]);

    if (!subStrides.empty()) {
      strides.push_back(affine::makeComposedFoldedAffineApply(
          rewriter, loc, s0 * s1, {subStrides[i], origStride}));
    }

    origStrides.emplace_back(origStride);
  }

  // Compute linearized index:
  auto &&[expr, values] =
      computeLinearIndex(rewriter.getIndexAttr(0), origStrides, subOffsets);
  OpFoldResult linearizedIndex =
      affine::makeComposedFoldedAffineApply(rewriter, loc, expr, values);

  // Compute collapsed size: (the outmost stride * outmost dimension).
  SmallVector<OpFoldResult> ops{origStrides.front(), outmostDim};
  OpFoldResult collapsedSize = computeProduct(loc, rewriter, ops);

  return {newExtractStridedMetadata.getBaseBuffer(), linearizedIndex,
          origStrides, origOffset, collapsedSize};
}

static Value getValueFromOpFoldResult(OpBuilder &rewriter, Location loc,
                                      OpFoldResult in) {
  if (Attribute offsetAttr = dyn_cast<Attribute>(in)) {
    return rewriter.create<arith::ConstantIndexOp>(
        loc, cast<IntegerAttr>(offsetAttr).getInt());
  }
  return cast<Value>(in);
}

/// Returns a collapsed memref and the linearized index to access the element
/// at the specified indices.
static std::pair<Value, Value> getFlattenMemrefAndOffset(OpBuilder &rewriter,
                                                         Location loc,
                                                         Value source,
                                                         ValueRange indices) {
  auto &&[base, index, strides, offset, collapsedShape] =
      getFlatOffsetAndStrides(rewriter, loc, source,
                              getAsOpFoldResult(indices));

  return std::make_pair(
      rewriter.create<memref::ReinterpretCastOp>(
          loc, source,
          /* offset = */ offset,
          /* shapes = */ ArrayRef<OpFoldResult>{collapsedShape},
          /* strides = */ ArrayRef<OpFoldResult>{strides.back()}),
      getValueFromOpFoldResult(rewriter, loc, index));
}

static bool needFlattenning(Value val) {
  auto type = cast<MemRefType>(val.getType());
  return type.getRank() > 1;
}

static bool checkLayout(Value val) {
  auto type = cast<MemRefType>(val.getType());
  return type.getLayout().isIdentity() ||
         isa<StridedLayoutAttr>(type.getLayout());
}

namespace {
template <typename T>
static Value getTargetMemref(T op) {
  if constexpr (std::is_same_v<T, memref::LoadOp>) {
    return op.getMemref();
  } else if constexpr (std::is_same_v<T, vector::LoadOp>) {
    return op.getBase();
  } else if constexpr (std::is_same_v<T, memref::StoreOp>) {
    return op.getMemref();
  } else if constexpr (std::is_same_v<T, vector::StoreOp>) {
    return op.getBase();
  } else if constexpr (std::is_same_v<T, vector::MaskedLoadOp>) {
    return op.getBase();
  } else if constexpr (std::is_same_v<T, vector::MaskedStoreOp>) {
    return op.getBase();
  } else if constexpr (std::is_same_v<T, vector::TransferReadOp>) {
    return op.getBase();
  } else if constexpr (std::is_same_v<T, vector::TransferWriteOp>) {
    return op.getBase();
  }
  return {};
}

template <typename T>
static void replaceOp(T op, PatternRewriter &rewriter, Value flatMemref,
                      Value offset) {
  if constexpr (std::is_same_v<T, memref::LoadOp>) {
    auto newLoad = rewriter.create<memref::LoadOp>(
        op->getLoc(), op->getResultTypes(), flatMemref, ValueRange{offset});
    newLoad->setAttrs(op->getAttrs());
    rewriter.replaceOp(op, newLoad.getResult());
  } else if constexpr (std::is_same_v<T, vector::LoadOp>) {
    auto newLoad = rewriter.create<vector::LoadOp>(
        op->getLoc(), op->getResultTypes(), flatMemref, ValueRange{offset});
    newLoad->setAttrs(op->getAttrs());
    rewriter.replaceOp(op, newLoad.getResult());
  } else if constexpr (std::is_same_v<T, memref::StoreOp>) {
    auto newStore = rewriter.create<memref::StoreOp>(
        op->getLoc(), op->getOperands().front(), flatMemref,
        ValueRange{offset});
    newStore->setAttrs(op->getAttrs());
    rewriter.replaceOp(op, newStore);
  } else if constexpr (std::is_same_v<T, vector::StoreOp>) {
    auto newStore = rewriter.create<vector::StoreOp>(
        op->getLoc(), op->getOperands().front(), flatMemref,
        ValueRange{offset});
    newStore->setAttrs(op->getAttrs());
    rewriter.replaceOp(op, newStore);
  } else if constexpr (std::is_same_v<T, vector::TransferReadOp>) {
    auto newTransferRead = rewriter.create<vector::TransferReadOp>(
        op->getLoc(), op.getType(), flatMemref, ValueRange{offset},
        op.getPadding());
    rewriter.replaceOp(op, newTransferRead.getResult());
  } else if constexpr (std::is_same_v<T, vector::TransferWriteOp>) {
    auto newTransferWrite = rewriter.create<vector::TransferWriteOp>(
        op->getLoc(), op.getVector(), flatMemref, ValueRange{offset});
    rewriter.replaceOp(op, newTransferWrite);
  } else if constexpr (std::is_same_v<T, vector::MaskedLoadOp>) {
    auto newMaskedLoad = rewriter.create<vector::MaskedLoadOp>(
        op->getLoc(), op.getType(), flatMemref, ValueRange{offset},
        op.getMask(), op.getPassThru());
    newMaskedLoad->setAttrs(op->getAttrs());
    rewriter.replaceOp(op, newMaskedLoad.getResult());
  } else if constexpr (std::is_same_v<T, vector::MaskedStoreOp>) {
    auto newMaskedStore = rewriter.create<vector::MaskedStoreOp>(
        op->getLoc(), flatMemref, ValueRange{offset}, op.getMask(),
        op.getValueToStore());
    newMaskedStore->setAttrs(op->getAttrs());
    rewriter.replaceOp(op, newMaskedStore);
  } else {
    op.emitOpError("unimplemented: do not know how to replace op.");
  }
}

template <typename T>
struct MemRefRewritePatternBase : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;
  LogicalResult matchAndRewrite(T op,
                                PatternRewriter &rewriter) const override {
    Value memref = getTargetMemref<T>(op);
    if (!needFlattenning(memref) || !checkLayout(memref))
      return rewriter.notifyMatchFailure(op,
                                         "nothing to do or unsupported layout");
    auto &&[flatMemref, offset] = getFlattenMemrefAndOffset(
        rewriter, op->getLoc(), memref, op.getIndices());
    replaceOp<T>(op, rewriter, flatMemref, offset);
    return success();
  }
};

struct FlattenMemrefLoad : public MemRefRewritePatternBase<memref::LoadOp> {
  using MemRefRewritePatternBase<memref::LoadOp>::MemRefRewritePatternBase;
};

struct FlattenVectorLoad : public MemRefRewritePatternBase<vector::LoadOp> {
  using MemRefRewritePatternBase<vector::LoadOp>::MemRefRewritePatternBase;
};

struct FlattenMemrefStore : public MemRefRewritePatternBase<memref::StoreOp> {
  using MemRefRewritePatternBase<memref::StoreOp>::MemRefRewritePatternBase;
};

struct FlattenVectorStore : public MemRefRewritePatternBase<vector::StoreOp> {
  using MemRefRewritePatternBase<vector::StoreOp>::MemRefRewritePatternBase;
};

struct FlattenVectorMaskedLoad
    : public MemRefRewritePatternBase<vector::MaskedLoadOp> {
  using MemRefRewritePatternBase<
      vector::MaskedLoadOp>::MemRefRewritePatternBase;
};

struct FlattenVectorMaskedStore
    : public MemRefRewritePatternBase<vector::MaskedStoreOp> {
  using MemRefRewritePatternBase<
      vector::MaskedStoreOp>::MemRefRewritePatternBase;
};

struct FlattenVectorTransferRead
    : public MemRefRewritePatternBase<vector::TransferReadOp> {
  using MemRefRewritePatternBase<
      vector::TransferReadOp>::MemRefRewritePatternBase;
};

struct FlattenVectorTransferWrite
    : public MemRefRewritePatternBase<vector::TransferWriteOp> {
  using MemRefRewritePatternBase<
      vector::TransferWriteOp>::MemRefRewritePatternBase;
};

struct FlattenSubview : public OpRewritePattern<memref::SubViewOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(memref::SubViewOp op,
                                PatternRewriter &rewriter) const override {
    Value memref = op.getSource();
    if (!needFlattenning(memref))
      return rewriter.notifyMatchFailure(op, "nothing to do");

    if (!checkLayout(memref))
      return rewriter.notifyMatchFailure(op, "unsupported layout");

    Location loc = op.getLoc();
    SmallVector<OpFoldResult> subOffsets = op.getMixedOffsets();
    SmallVector<OpFoldResult> subSizes = op.getMixedSizes();
    SmallVector<OpFoldResult> subStrides = op.getMixedStrides();
    auto &&[base, finalOffset, strides, _, __] =
        getFlatOffsetAndStrides(rewriter, loc, memref, subOffsets, subStrides);

    auto srcType = cast<MemRefType>(memref.getType());
    auto resultType = cast<MemRefType>(op.getType());
    unsigned subRank = static_cast<unsigned>(resultType.getRank());

    llvm::SmallBitVector droppedDims = op.getDroppedDims();

    SmallVector<OpFoldResult> finalSizes;
    finalSizes.reserve(subRank);

    SmallVector<OpFoldResult> finalStrides;
    finalStrides.reserve(subRank);

    for (auto i : llvm::seq(0u, static_cast<unsigned>(srcType.getRank()))) {
      if (droppedDims.test(i))
        continue;

      finalSizes.push_back(subSizes[i]);
      finalStrides.push_back(strides[i]);
    }

    rewriter.replaceOpWithNewOp<memref::ReinterpretCastOp>(
        op, resultType, base, finalOffset, finalSizes, finalStrides);
    return success();
  }
};

struct DecomposeMemrefsPass
    : public impl::DecomposeMemrefsPassBase<DecomposeMemrefsPass> {
  using impl::DecomposeMemrefsPassBase<
      DecomposeMemrefsPass>::DecomposeMemrefsPassBase;

  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<affine::AffineDialect, arith::ArithDialect,
                    memref::MemRefDialect, vector::VectorDialect>();
  }

  void runOnOperation() override {
    RewritePatternSet patterns(&getContext());

    mlir::iree_compiler::populateDecomposeMemrefsPatterns(patterns);

    if (failed(applyPatternsGreedily(getOperation(), std::move(patterns))))
      return signalPassFailure();
  }
};

} // namespace

namespace mlir::iree_compiler {
void populateDecomposeMemrefsPatterns(RewritePatternSet &patterns) {
  patterns.insert<FlattenMemrefLoad, FlattenMemrefStore, FlattenSubview,
                  FlattenVectorMaskedLoad, FlattenVectorMaskedStore,
                  FlattenVectorLoad, FlattenVectorStore,
                  FlattenVectorTransferRead, FlattenVectorTransferWrite>(
      patterns.getContext());
}

std::unique_ptr<Pass> createDecomposeMemrefsPass() {
  return std::make_unique<DecomposeMemrefsPass>();
}
} // namespace mlir::iree_compiler
