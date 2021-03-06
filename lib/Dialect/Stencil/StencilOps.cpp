#include "Dialect/Stencil/StencilOps.h"
#include "Dialect/Stencil/StencilDialect.h"
#include "Dialect/Stencil/StencilTypes.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Region.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/UseDefLists.h"
#include "mlir/IR/Value.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <tuple>

using namespace mlir;

//===----------------------------------------------------------------------===//
// stencil.apply
//===----------------------------------------------------------------------===//

static ParseResult parseApplyOp(OpAsmParser &parser, OperationState &state) {
  SmallVector<OpAsmParser::OperandType, 8> operands;
  SmallVector<OpAsmParser::OperandType, 8> arguments;
  SmallVector<Type, 8> operandTypes;

  // Parse the assignment list
  if (succeeded(parser.parseOptionalLParen())) {
    do {
      OpAsmParser::OperandType currentArgument, currentOperand;
      Type currentType;

      if (parser.parseRegionArgument(currentArgument) || parser.parseEqual() ||
          parser.parseOperand(currentOperand) ||
          parser.parseColonType(currentType))
        return failure();

      arguments.push_back(currentArgument);
      operands.push_back(currentOperand);
      operandTypes.push_back(currentType);
    } while (succeeded(parser.parseOptionalComma()));
    if (parser.parseRParen())
      return failure();
  }

  // Parse the result types and the optional attributes
  SmallVector<Type, 8> resultTypes;
  if (parser.parseArrowTypeList(resultTypes) ||
      parser.parseOptionalAttrDictWithKeyword(state.attributes))
    return failure();

  // Resolve the operand types
  auto loc = parser.getCurrentLocation();
  if (parser.resolveOperands(operands, operandTypes, loc, state.operands) ||
      parser.addTypesToList(resultTypes, state.types))
    return failure();

  // Parse the body region.
  Region *body = state.addRegion();
  if (parser.parseRegion(*body, arguments, operandTypes))
    return failure();

  // Parse the optional bounds
  ArrayAttr lbAttr, ubAttr;
  if (succeeded(parser.parseOptionalKeyword("to"))) {
    // Parse the optional bounds
    if (parser.parseLParen() ||
        parser.parseAttribute(lbAttr, stencil::ApplyOp::getLBAttrName(),
                              state.attributes) ||
        parser.parseColon() ||
        parser.parseAttribute(ubAttr, stencil::ApplyOp::getUBAttrName(),
                              state.attributes) ||
        parser.parseRParen())
      return failure();
  }

  return success();
}

static void print(stencil::ApplyOp applyOp, OpAsmPrinter &printer) {
  printer << stencil::ApplyOp::getOperationName() << ' ';
  // Print the region arguments
  SmallVector<Value, 10> operands = applyOp.getOperands();
  if (!applyOp.region().empty() && !operands.empty()) {
    Block *body = applyOp.getBody();
    printer << "(";
    llvm::interleaveComma(
        llvm::seq<int>(0, operands.size()), printer, [&](int i) {
          printer << body->getArgument(i) << " = " << operands[i] << " : "
                  << operands[i].getType();
        });
    printer << ") ";
  }

  // Print the result types
  printer << "-> ";
  if (applyOp.res().size() > 1)
    printer << "(";
  llvm::interleaveComma(applyOp.res().getTypes(), printer);
  if (applyOp.res().size() > 1)
    printer << ")";

  // Print optional attributes
  printer.printOptionalAttrDictWithKeyword(
      applyOp.getAttrs(), /*elidedAttrs=*/{stencil::ApplyOp::getLBAttrName(),
                                           stencil::ApplyOp::getUBAttrName()});

  // Print region, bounds, and return type
  printer.printRegion(applyOp.region(),
                      /*printEntryBlockArgs=*/false);
  if (applyOp.lb().hasValue() && applyOp.ub().hasValue()) {
    printer << " to (";
    printer.printAttribute(applyOp.lb().getValue());
    printer << " : ";
    printer.printAttribute(applyOp.ub().getValue());
    printer << ")";
  }
}

void stencil::ApplyOp::setOperandShape(Value operand, TempType newType) {
  auto it = llvm::find(getOperands(), operand);
  assert(it != getOperands().end() && "failed to find operand");
  auto arg = getBody()->getArgument(std::distance(getOperands().begin(), it));
  auto oldType = arg.getType().cast<TempType>();
  assert(oldType.getElementType() == newType.getElementType() &&
         "expected the types to have the same element type");
  assert(oldType.getAllocation() == newType.getAllocation() &&
         "expected the types to have the same allocation");
  arg.setType(newType);
}

//===----------------------------------------------------------------------===//
// stencil.dyn_access
//===----------------------------------------------------------------------===//

void stencil::DynAccessOp::shiftByOffset(ArrayRef<int64_t> offset) {
  SmallVector<Attribute, kIndexSize> lowerBound, upperBound;
  for (auto elem : llvm::zip(offset, lb(), ub())) {
    lowerBound.push_back(IntegerAttr::get(IntegerType::get(64, getContext()),
                                          std::get<1>(elem)
                                                  .template cast<IntegerAttr>()
                                                  .getValue()
                                                  .getSExtValue() +
                                              std::get<0>(elem)));
    upperBound.push_back(IntegerAttr::get(IntegerType::get(64, getContext()),
                                          std::get<2>(elem)
                                                  .template cast<IntegerAttr>()
                                                  .getValue()
                                                  .getSExtValue() +
                                              std::get<0>(elem)));
  }
  lbAttr(ArrayAttr::get(lowerBound, getContext()));
  ubAttr(ArrayAttr::get(upperBound, getContext()));
}

std::tuple<stencil::Index, stencil::Index>
stencil::DynAccessOp::getAccessExtent() {
  Index lowerBound, upperBound;
  for (auto it : llvm::zip(lb(), ub())) {
    lowerBound.push_back(
        std::get<0>(it).cast<IntegerAttr>().getValue().getSExtValue());
    upperBound.push_back(
        std::get<1>(it).cast<IntegerAttr>().getValue().getSExtValue());
  }
  return std::make_tuple(lowerBound, upperBound);
}

//===----------------------------------------------------------------------===//
// stencil.make_result
//===----------------------------------------------------------------------===//

OpOperand *stencil::StoreResultOp::getReturnOpOperand() {
  auto current = res();
  while (current.hasOneUse()) {
    // Return the operand if we found the return op
    OpOperand *operand = current.getUses().begin().getOperand();
    if (isa<stencil::ReturnOp>(operand->getOwner()))
      return operand;
    // Otherwise we expect a yield op
    auto yieldOp = dyn_cast<scf::YieldOp>(operand->getOwner());
    if (!yieldOp)
      return nullptr;
    if (isa<scf::ForOp>(yieldOp.getParentOp()) &&
        yieldOp.getParentOfType<stencil::ApplyOp>())
      nullptr;
    // Continue the search in the parent region
    current = yieldOp.getParentOp()->getResult(operand->getOperandNumber());
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Canonicalization
//===----------------------------------------------------------------------===//

stencil::ApplyOpPattern::ApplyOpPattern(MLIRContext *context,
                                        PatternBenefit benefit)
    : OpRewritePattern<stencil::ApplyOp>(context, benefit) {}

stencil::ApplyOp
stencil::ApplyOpPattern::cleanupOpArguments(stencil::ApplyOp applyOp,
                                            PatternRewriter &rewriter) const {
  // Compute the new operand list and index mapping
  llvm::DenseMap<Value, unsigned int> newIndex;
  SmallVector<Value, 10> newOperands;
  for (auto &en : llvm::enumerate(applyOp.getOperands())) {
    if (newIndex.count(en.value()) == 0) {
      if (!applyOp.getBody()->getArgument(en.index()).getUses().empty()) {
        newIndex[en.value()] = newOperands.size();
        newOperands.push_back(en.value());
      } else {
        // Unused arguments are mapped to the first index
        newIndex[en.value()] = 0;
      }
    }
  }

  // Create a new operation with shorther argument list
  if (newOperands.size() < applyOp.getNumOperands()) {
    auto loc = applyOp.getLoc();
    auto newOp = rewriter.create<stencil::ApplyOp>(loc, newOperands,
                                                   applyOp.getResultTypes());

    // Compute the argument mapping and move the block
    SmallVector<Value, 10> newArgs(applyOp.getNumOperands());
    llvm::transform(applyOp.getOperands(), newArgs.begin(), [&](Value value) {
      return newOperands.empty()
                 ? value // pass default value if the new apply has no params
                 : newOp.getBody()->getArgument(newIndex[value]);
    });
    rewriter.mergeBlocks(applyOp.getBody(), newOp.getBody(), newArgs);
    return newOp;
  }
  return nullptr;
}

namespace {

/// This is a pattern to remove duplicate and unused arguments
struct ApplyOpArgumentCleaner : public stencil::ApplyOpPattern {
  using ApplyOpPattern::ApplyOpPattern;

  LogicalResult matchAndRewrite(stencil::ApplyOp applyOp,
                                PatternRewriter &rewriter) const override {
    if (auto newOp = cleanupOpArguments(applyOp, rewriter)) {
      rewriter.replaceOp(applyOp, newOp.getResults());
      return success();
    }
    return failure();
  }
};

// Helper methods to hoist operations
LogicalResult hoistBackward(Operation *op, PatternRewriter &rewriter,
                            std::function<bool(Operation *)> condition) {
  // Skip compute operations
  auto curr = op;
  while (curr->getPrevNode() && condition(curr->getPrevNode()) &&
         !llvm::is_contained(curr->getPrevNode()->getUsers(), op))
    curr = curr->getPrevNode();

  // Move the operation
  if (curr != op) {
    rewriter.setInsertionPoint(curr);
    rewriter.replaceOp(op, rewriter.clone(*op)->getResults());
    return success();
  }
  return failure();
}
LogicalResult hoistForward(Operation *op, PatternRewriter &rewriter,
                           std::function<bool(Operation *)> condition) {
  // Skip compute operations
  auto curr = op;
  while (curr->getNextNode() && condition(curr->getNextNode()) &&
         !curr->getNextNode()->isKnownTerminator())
    curr = curr->getNextNode();

  // Move the operation
  if (curr != op) {
    rewriter.setInsertionPointAfter(curr);
    rewriter.replaceOp(op, rewriter.clone(*op)->getResults());
    return success();
  }
  return failure();
} // namespace

/// This is a pattern to hoist assert ops out of the computation
struct CastOpHoisting : public OpRewritePattern<stencil::CastOp> {
  using OpRewritePattern<stencil::CastOp>::OpRewritePattern;

  // Remove duplicates if needed
  LogicalResult matchAndRewrite(stencil::CastOp castOp,
                                PatternRewriter &rewriter) const override {
    // Skip all operations except for other casts
    auto condition = [](Operation *op) { return !isa<stencil::CastOp>(op); };
    return hoistBackward(castOp.getOperation(), rewriter, condition);
  }
};

/// This is a pattern to hoist load ops out of the computation
struct LoadOpHoisting : public OpRewritePattern<stencil::LoadOp> {
  using OpRewritePattern<stencil::LoadOp>::OpRewritePattern;

  // Remove duplicates if needed
  LogicalResult matchAndRewrite(stencil::LoadOp loadOp,
                                PatternRewriter &rewriter) const override {
    // Skip all operations except for casts and other loads
    auto condition = [](Operation *op) {
      return !isa<stencil::LoadOp>(op) && !isa<stencil::CastOp>(op);
    };
    return hoistBackward(loadOp.getOperation(), rewriter, condition);
  }
};

/// This is a pattern to hoist store ops out of the computation
struct StoreOpHoisting : public OpRewritePattern<stencil::StoreOp> {
  using OpRewritePattern<stencil::StoreOp>::OpRewritePattern;

  // Remove duplicates if needed
  LogicalResult matchAndRewrite(stencil::StoreOp storeOp,
                                PatternRewriter &rewriter) const override {
    // Skip all operations except for stores
    auto condition = [](Operation *op) { return !isa<stencil::StoreOp>(op); };
    return hoistForward(storeOp.getOperation(), rewriter, condition);
  }
};

} // end anonymous namespace

// Register canonicalization patterns
void stencil::ApplyOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<ApplyOpArgumentCleaner>(context);
}

void stencil::CastOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<CastOpHoisting>(context);
}
void stencil::LoadOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<LoadOpHoisting>(context);
}
void stencil::StoreOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<StoreOpHoisting>(context);
}

namespace mlir {
namespace stencil {

#include "Dialect/Stencil/StencilOpsInterfaces.cpp.inc"

#define GET_OP_CLASSES
#include "Dialect/Stencil/StencilOps.cpp.inc"

} // namespace stencil
} // namespace mlir
