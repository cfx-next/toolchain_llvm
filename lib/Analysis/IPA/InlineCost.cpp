//===- InlineCost.cpp - Cost analysis for inliner -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements inline cost analysis.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "inline-cost"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

STATISTIC(NumCallsAnalyzed, "Number of call sites analyzed");

namespace {

class CallAnalyzer : public InstVisitor<CallAnalyzer, bool> {
  typedef InstVisitor<CallAnalyzer, bool> Base;
  friend class InstVisitor<CallAnalyzer, bool>;

  // DataLayout if available, or null.
  const DataLayout *const DL;

  /// The TargetTransformInfo available for this compilation.
  const TargetTransformInfo &TTI;

  // The called function.
  Function &F;

  int Threshold;
  int Cost;

  bool IsCallerRecursive;
  bool IsRecursiveCall;
  bool ExposesReturnsTwice;
  bool HasDynamicAlloca;
  bool ContainsNoDuplicateCall;
  bool HasReturn;
  bool HasIndirectBr;

  /// Number of bytes allocated statically by the callee.
  uint64_t AllocatedSize;
  unsigned NumInstructions, NumVectorInstructions;
  int FiftyPercentVectorBonus, TenPercentVectorBonus;
  int VectorBonus;

  // While we walk the potentially-inlined instructions, we build up and
  // maintain a mapping of simplified values specific to this callsite. The
  // idea is to propagate any special information we have about arguments to
  // this call through the inlinable section of the function, and account for
  // likely simplifications post-inlining. The most important aspect we track
  // is CFG altering simplifications -- when we prove a basic block dead, that
  // can cause dramatic shifts in the cost of inlining a function.
  DenseMap<Value *, Constant *> SimplifiedValues;

  // Keep track of the values which map back (through function arguments) to
  // allocas on the caller stack which could be simplified through SROA.
  DenseMap<Value *, Value *> SROAArgValues;

  // The mapping of caller Alloca values to their accumulated cost savings. If
  // we have to disable SROA for one of the allocas, this tells us how much
  // cost must be added.
  DenseMap<Value *, int> SROAArgCosts;

  // Keep track of values which map to a pointer base and constant offset.
  DenseMap<Value *, std::pair<Value *, APInt> > ConstantOffsetPtrs;

  // Custom simplification helper routines.
  bool isAllocaDerivedArg(Value *V);
  bool lookupSROAArgAndCost(Value *V, Value *&Arg,
                            DenseMap<Value *, int>::iterator &CostIt);
  void disableSROA(DenseMap<Value *, int>::iterator CostIt);
  void disableSROA(Value *V);
  void accumulateSROACost(DenseMap<Value *, int>::iterator CostIt,
                          int InstructionCost);
  bool handleSROACandidate(bool IsSROAValid,
                           DenseMap<Value *, int>::iterator CostIt,
                           int InstructionCost);
  bool isGEPOffsetConstant(GetElementPtrInst &GEP);
  bool accumulateGEPOffset(GEPOperator &GEP, APInt &Offset);
  bool simplifyCallSite(Function *F, CallSite CS);
  ConstantInt *stripAndComputeInBoundsConstantOffsets(Value *&V);

  // Custom analysis routines.
  bool analyzeBlock(BasicBlock *BB);

  // Disable several entry points to the visitor so we don't accidentally use
  // them by declaring but not defining them here.
  void visit(Module *);     void visit(Module &);
  void visit(Function *);   void visit(Function &);
  void visit(BasicBlock *); void visit(BasicBlock &);

  // Provide base case for our instruction visit.
  bool visitInstruction(Instruction &I);

  // Our visit overrides.
  bool visitAlloca(AllocaInst &I);
  bool visitPHI(PHINode &I);
  bool visitGetElementPtr(GetElementPtrInst &I);
  bool visitBitCast(BitCastInst &I);
  bool visitPtrToInt(PtrToIntInst &I);
  bool visitIntToPtr(IntToPtrInst &I);
  bool visitCastInst(CastInst &I);
  bool visitUnaryInstruction(UnaryInstruction &I);
  bool visitCmpInst(CmpInst &I);
  bool visitSub(BinaryOperator &I);
  bool visitBinaryOperator(BinaryOperator &I);
  bool visitLoad(LoadInst &I);
  bool visitStore(StoreInst &I);
  bool visitExtractValue(ExtractValueInst &I);
  bool visitInsertValue(InsertValueInst &I);
  bool visitCallSite(CallSite CS);
  bool visitReturnInst(ReturnInst &RI);
  bool visitBranchInst(BranchInst &BI);
  bool visitSwitchInst(SwitchInst &SI);
  bool visitIndirectBrInst(IndirectBrInst &IBI);
  bool visitResumeInst(ResumeInst &RI);
  bool visitUnreachableInst(UnreachableInst &I);

public:
  CallAnalyzer(const DataLayout *DL, const TargetTransformInfo &TTI,
               Function &Callee, int Threshold)
      : DL(DL), TTI(TTI), F(Callee), Threshold(Threshold), Cost(0),
        IsCallerRecursive(false), IsRecursiveCall(false),
        ExposesReturnsTwice(false), HasDynamicAlloca(false),
        ContainsNoDuplicateCall(false), HasReturn(false), HasIndirectBr(false),
        AllocatedSize(0), NumInstructions(0), NumVectorInstructions(0),
        FiftyPercentVectorBonus(0), TenPercentVectorBonus(0), VectorBonus(0),
        NumConstantArgs(0), NumConstantOffsetPtrArgs(0), NumAllocaArgs(0),
        NumConstantPtrCmps(0), NumConstantPtrDiffs(0),
        NumInstructionsSimplified(0), SROACostSavings(0),
        SROACostSavingsLost(0) {}

  bool analyzeCall(CallSite CS);

  int getThreshold() { return Threshold; }
  int getCost() { return Cost; }

  // Keep a bunch of stats about the cost savings found so we can print them
  // out when debugging.
  unsigned NumConstantArgs;
  unsigned NumConstantOffsetPtrArgs;
  unsigned NumAllocaArgs;
  unsigned NumConstantPtrCmps;
  unsigned NumConstantPtrDiffs;
  unsigned NumInstructionsSimplified;
  unsigned SROACostSavings;
  unsigned SROACostSavingsLost;

  void dump();
};

} // namespace

/// \brief Test whether the given value is an Alloca-derived function argument.
bool CallAnalyzer::isAllocaDerivedArg(Value *V) {
  return SROAArgValues.count(V);
}

/// \brief Lookup the SROA-candidate argument and cost iterator which V maps to.
/// Returns false if V does not map to a SROA-candidate.
bool CallAnalyzer::lookupSROAArgAndCost(
    Value *V, Value *&Arg, DenseMap<Value *, int>::iterator &CostIt) {
  if (SROAArgValues.empty() || SROAArgCosts.empty())
    return false;

  DenseMap<Value *, Value *>::iterator ArgIt = SROAArgValues.find(V);
  if (ArgIt == SROAArgValues.end())
    return false;

  Arg = ArgIt->second;
  CostIt = SROAArgCosts.find(Arg);
  return CostIt != SROAArgCosts.end();
}

/// \brief Disable SROA for the candidate marked by this cost iterator.
///
/// This marks the candidate as no longer viable for SROA, and adds the cost
/// savings associated with it back into the inline cost measurement.
void CallAnalyzer::disableSROA(DenseMap<Value *, int>::iterator CostIt) {
  // If we're no longer able to perform SROA we need to undo its cost savings
  // and prevent subsequent analysis.
  Cost += CostIt->second;
  SROACostSavings -= CostIt->second;
  SROACostSavingsLost += CostIt->second;
  SROAArgCosts.erase(CostIt);
}

/// \brief If 'V' maps to a SROA candidate, disable SROA for it.
void CallAnalyzer::disableSROA(Value *V) {
  Value *SROAArg;
  DenseMap<Value *, int>::iterator CostIt;
  if (lookupSROAArgAndCost(V, SROAArg, CostIt))
    disableSROA(CostIt);
}

/// \brief Accumulate the given cost for a particular SROA candidate.
void CallAnalyzer::accumulateSROACost(DenseMap<Value *, int>::iterator CostIt,
                                      int InstructionCost) {
  CostIt->second += InstructionCost;
  SROACostSavings += InstructionCost;
}

/// \brief Helper for the common pattern of handling a SROA candidate.
/// Either accumulates the cost savings if the SROA remains valid, or disables
/// SROA for the candidate.
bool CallAnalyzer::handleSROACandidate(bool IsSROAValid,
                                       DenseMap<Value *, int>::iterator CostIt,
                                       int InstructionCost) {
  if (IsSROAValid) {
    accumulateSROACost(CostIt, InstructionCost);
    return true;
  }

  disableSROA(CostIt);
  return false;
}

/// \brief Check whether a GEP's indices are all constant.
///
/// Respects any simplified values known during the analysis of this callsite.
bool CallAnalyzer::isGEPOffsetConstant(GetElementPtrInst &GEP) {
  for (User::op_iterator I = GEP.idx_begin(), E = GEP.idx_end(); I != E; ++I)
    if (!isa<Constant>(*I) && !SimplifiedValues.lookup(*I))
      return false;

  return true;
}

/// \brief Accumulate a constant GEP offset into an APInt if possible.
///
/// Returns false if unable to compute the offset for any reason. Respects any
/// simplified values known during the analysis of this callsite.
bool CallAnalyzer::accumulateGEPOffset(GEPOperator &GEP, APInt &Offset) {
  if (!DL)
    return false;

  unsigned IntPtrWidth = DL->getPointerSizeInBits();
  assert(IntPtrWidth == Offset.getBitWidth());

  for (gep_type_iterator GTI = gep_type_begin(GEP), GTE = gep_type_end(GEP);
       GTI != GTE; ++GTI) {
    ConstantInt *OpC = dyn_cast<ConstantInt>(GTI.getOperand());
    if (!OpC)
      if (Constant *SimpleOp = SimplifiedValues.lookup(GTI.getOperand()))
        OpC = dyn_cast<ConstantInt>(SimpleOp);
    if (!OpC)
      return false;
    if (OpC->isZero()) continue;

    // Handle a struct index, which adds its field offset to the pointer.
    if (StructType *STy = dyn_cast<StructType>(*GTI)) {
      unsigned ElementIdx = OpC->getZExtValue();
      const StructLayout *SL = DL->getStructLayout(STy);
      Offset += APInt(IntPtrWidth, SL->getElementOffset(ElementIdx));
      continue;
    }

    APInt TypeSize(IntPtrWidth, DL->getTypeAllocSize(GTI.getIndexedType()));
    Offset += OpC->getValue().sextOrTrunc(IntPtrWidth) * TypeSize;
  }
  return true;
}

bool CallAnalyzer::visitAlloca(AllocaInst &I) {
  // FIXME: Check whether inlining will turn a dynamic alloca into a static
  // alloca, and handle that case.

  // Accumulate the allocated size.
  if (I.isStaticAlloca()) {
    Type *Ty = I.getAllocatedType();
    AllocatedSize += (DL ? DL->getTypeAllocSize(Ty) :
                      Ty->getPrimitiveSizeInBits());
  }

  // We will happily inline static alloca instructions.
  if (I.isStaticAlloca())
    return Base::visitAlloca(I);

  // FIXME: This is overly conservative. Dynamic allocas are inefficient for
  // a variety of reasons, and so we would like to not inline them into
  // functions which don't currently have a dynamic alloca. This simply
  // disables inlining altogether in the presence of a dynamic alloca.
  HasDynamicAlloca = true;
  return false;
}

bool CallAnalyzer::visitPHI(PHINode &I) {
  // FIXME: We should potentially be tracking values through phi nodes,
  // especially when they collapse to a single value due to deleted CFG edges
  // during inlining.

  // FIXME: We need to propagate SROA *disabling* through phi nodes, even
  // though we don't want to propagate it's bonuses. The idea is to disable
  // SROA if it *might* be used in an inappropriate manner.

  // Phi nodes are always zero-cost.
  return true;
}

bool CallAnalyzer::visitGetElementPtr(GetElementPtrInst &I) {
  Value *SROAArg;
  DenseMap<Value *, int>::iterator CostIt;
  bool SROACandidate = lookupSROAArgAndCost(I.getPointerOperand(),
                                            SROAArg, CostIt);

  // Try to fold GEPs of constant-offset call site argument pointers. This
  // requires target data and inbounds GEPs.
  if (DL && I.isInBounds()) {
    // Check if we have a base + offset for the pointer.
    Value *Ptr = I.getPointerOperand();
    std::pair<Value *, APInt> BaseAndOffset = ConstantOffsetPtrs.lookup(Ptr);
    if (BaseAndOffset.first) {
      // Check if the offset of this GEP is constant, and if so accumulate it
      // into Offset.
      if (!accumulateGEPOffset(cast<GEPOperator>(I), BaseAndOffset.second)) {
        // Non-constant GEPs aren't folded, and disable SROA.
        if (SROACandidate)
          disableSROA(CostIt);
        return false;
      }

      // Add the result as a new mapping to Base + Offset.
      ConstantOffsetPtrs[&I] = BaseAndOffset;

      // Also handle SROA candidates here, we already know that the GEP is
      // all-constant indexed.
      if (SROACandidate)
        SROAArgValues[&I] = SROAArg;

      return true;
    }
  }

  if (isGEPOffsetConstant(I)) {
    if (SROACandidate)
      SROAArgValues[&I] = SROAArg;

    // Constant GEPs are modeled as free.
    return true;
  }

  // Variable GEPs will require math and will disable SROA.
  if (SROACandidate)
    disableSROA(CostIt);
  return false;
}

bool CallAnalyzer::visitBitCast(BitCastInst &I) {
  // Propagate constants through bitcasts.
  Constant *COp = dyn_cast<Constant>(I.getOperand(0));
  if (!COp)
    COp = SimplifiedValues.lookup(I.getOperand(0));
  if (COp)
    if (Constant *C = ConstantExpr::getBitCast(COp, I.getType())) {
      SimplifiedValues[&I] = C;
      return true;
    }

  // Track base/offsets through casts
  std::pair<Value *, APInt> BaseAndOffset
    = ConstantOffsetPtrs.lookup(I.getOperand(0));
  // Casts don't change the offset, just wrap it up.
  if (BaseAndOffset.first)
    ConstantOffsetPtrs[&I] = BaseAndOffset;

  // Also look for SROA candidates here.
  Value *SROAArg;
  DenseMap<Value *, int>::iterator CostIt;
  if (lookupSROAArgAndCost(I.getOperand(0), SROAArg, CostIt))
    SROAArgValues[&I] = SROAArg;

  // Bitcasts are always zero cost.
  return true;
}

bool CallAnalyzer::visitPtrToInt(PtrToIntInst &I) {
  const DataLayout *DL = I.getDataLayout();
  // Propagate constants through ptrtoint.
  Constant *COp = dyn_cast<Constant>(I.getOperand(0));
  if (!COp)
    COp = SimplifiedValues.lookup(I.getOperand(0));
  if (COp)
    if (Constant *C = ConstantExpr::getPtrToInt(COp, I.getType())) {
      SimplifiedValues[&I] = C;
      return true;
    }

  // Track base/offset pairs when converted to a plain integer provided the
  // integer is large enough to represent the pointer.
  unsigned IntegerSize = I.getType()->getScalarSizeInBits();
  if (DL && IntegerSize >= DL->getPointerSizeInBits()) {
    std::pair<Value *, APInt> BaseAndOffset
      = ConstantOffsetPtrs.lookup(I.getOperand(0));
    if (BaseAndOffset.first)
      ConstantOffsetPtrs[&I] = BaseAndOffset;
  }

  // This is really weird. Technically, ptrtoint will disable SROA. However,
  // unless that ptrtoint is *used* somewhere in the live basic blocks after
  // inlining, it will be nuked, and SROA should proceed. All of the uses which
  // would block SROA would also block SROA if applied directly to a pointer,
  // and so we can just add the integer in here. The only places where SROA is
  // preserved either cannot fire on an integer, or won't in-and-of themselves
  // disable SROA (ext) w/o some later use that we would see and disable.
  Value *SROAArg;
  DenseMap<Value *, int>::iterator CostIt;
  if (lookupSROAArgAndCost(I.getOperand(0), SROAArg, CostIt))
    SROAArgValues[&I] = SROAArg;

  return TargetTransformInfo::TCC_Free == TTI.getUserCost(&I);
}

bool CallAnalyzer::visitIntToPtr(IntToPtrInst &I) {
  const DataLayout *DL = I.getDataLayout();
  // Propagate constants through ptrtoint.
  Constant *COp = dyn_cast<Constant>(I.getOperand(0));
  if (!COp)
    COp = SimplifiedValues.lookup(I.getOperand(0));
  if (COp)
    if (Constant *C = ConstantExpr::getIntToPtr(COp, I.getType())) {
      SimplifiedValues[&I] = C;
      return true;
    }

  // Track base/offset pairs when round-tripped through a pointer without
  // modifications provided the integer is not too large.
  Value *Op = I.getOperand(0);
  unsigned IntegerSize = Op->getType()->getScalarSizeInBits();
  if (DL && IntegerSize <= DL->getPointerSizeInBits()) {
    std::pair<Value *, APInt> BaseAndOffset = ConstantOffsetPtrs.lookup(Op);
    if (BaseAndOffset.first)
      ConstantOffsetPtrs[&I] = BaseAndOffset;
  }

  // "Propagate" SROA here in the same manner as we do for ptrtoint above.
  Value *SROAArg;
  DenseMap<Value *, int>::iterator CostIt;
  if (lookupSROAArgAndCost(Op, SROAArg, CostIt))
    SROAArgValues[&I] = SROAArg;

  return TargetTransformInfo::TCC_Free == TTI.getUserCost(&I);
}

bool CallAnalyzer::visitCastInst(CastInst &I) {
  // Propagate constants through ptrtoint.
  Constant *COp = dyn_cast<Constant>(I.getOperand(0));
  if (!COp)
    COp = SimplifiedValues.lookup(I.getOperand(0));
  if (COp)
    if (Constant *C = ConstantExpr::getCast(I.getOpcode(), COp, I.getType())) {
      SimplifiedValues[&I] = C;
      return true;
    }

  // Disable SROA in the face of arbitrary casts we don't whitelist elsewhere.
  disableSROA(I.getOperand(0));

  return TargetTransformInfo::TCC_Free == TTI.getUserCost(&I);
}

bool CallAnalyzer::visitUnaryInstruction(UnaryInstruction &I) {
  Value *Operand = I.getOperand(0);
  Constant *COp = dyn_cast<Constant>(Operand);
  if (!COp)
    COp = SimplifiedValues.lookup(Operand);
  if (COp)
    if (Constant *C = ConstantFoldInstOperands(I.getOpcode(), I.getType(),
                                               COp, DL)) {
      SimplifiedValues[&I] = C;
      return true;
    }

  // Disable any SROA on the argument to arbitrary unary operators.
  disableSROA(Operand);

  return false;
}

bool CallAnalyzer::visitCmpInst(CmpInst &I) {
  Value *LHS = I.getOperand(0), *RHS = I.getOperand(1);
  // First try to handle simplified comparisons.
  if (!isa<Constant>(LHS))
    if (Constant *SimpleLHS = SimplifiedValues.lookup(LHS))
      LHS = SimpleLHS;
  if (!isa<Constant>(RHS))
    if (Constant *SimpleRHS = SimplifiedValues.lookup(RHS))
      RHS = SimpleRHS;
  if (Constant *CLHS = dyn_cast<Constant>(LHS)) {
    if (Constant *CRHS = dyn_cast<Constant>(RHS))
      if (Constant *C = ConstantExpr::getCompare(I.getPredicate(), CLHS, CRHS)) {
        SimplifiedValues[&I] = C;
        return true;
      }
  }

  if (I.getOpcode() == Instruction::FCmp)
    return false;

  // Otherwise look for a comparison between constant offset pointers with
  // a common base.
  Value *LHSBase, *RHSBase;
  APInt LHSOffset, RHSOffset;
  std::tie(LHSBase, LHSOffset) = ConstantOffsetPtrs.lookup(LHS);
  if (LHSBase) {
    std::tie(RHSBase, RHSOffset) = ConstantOffsetPtrs.lookup(RHS);
    if (RHSBase && LHSBase == RHSBase) {
      // We have common bases, fold the icmp to a constant based on the
      // offsets.
      Constant *CLHS = ConstantInt::get(LHS->getContext(), LHSOffset);
      Constant *CRHS = ConstantInt::get(RHS->getContext(), RHSOffset);
      if (Constant *C = ConstantExpr::getICmp(I.getPredicate(), CLHS, CRHS)) {
        SimplifiedValues[&I] = C;
        ++NumConstantPtrCmps;
        return true;
      }
    }
  }

  // If the comparison is an equality comparison with null, we can simplify it
  // for any alloca-derived argument.
  if (I.isEquality() && isa<ConstantPointerNull>(I.getOperand(1)))
    if (isAllocaDerivedArg(I.getOperand(0))) {
      // We can actually predict the result of comparisons between an
      // alloca-derived value and null. Note that this fires regardless of
      // SROA firing.
      bool IsNotEqual = I.getPredicate() == CmpInst::ICMP_NE;
      SimplifiedValues[&I] = IsNotEqual ? ConstantInt::getTrue(I.getType())
                                        : ConstantInt::getFalse(I.getType());
      return true;
    }

  // Finally check for SROA candidates in comparisons.
  Value *SROAArg;
  DenseMap<Value *, int>::iterator CostIt;
  if (lookupSROAArgAndCost(I.getOperand(0), SROAArg, CostIt)) {
    if (isa<ConstantPointerNull>(I.getOperand(1))) {
      accumulateSROACost(CostIt, InlineConstants::InstrCost);
      return true;
    }

    disableSROA(CostIt);
  }

  return false;
}

bool CallAnalyzer::visitSub(BinaryOperator &I) {
  // Try to handle a special case: we can fold computing the difference of two
  // constant-related pointers.
  Value *LHS = I.getOperand(0), *RHS = I.getOperand(1);
  Value *LHSBase, *RHSBase;
  APInt LHSOffset, RHSOffset;
  std::tie(LHSBase, LHSOffset) = ConstantOffsetPtrs.lookup(LHS);
  if (LHSBase) {
    std::tie(RHSBase, RHSOffset) = ConstantOffsetPtrs.lookup(RHS);
    if (RHSBase && LHSBase == RHSBase) {
      // We have common bases, fold the subtract to a constant based on the
      // offsets.
      Constant *CLHS = ConstantInt::get(LHS->getContext(), LHSOffset);
      Constant *CRHS = ConstantInt::get(RHS->getContext(), RHSOffset);
      if (Constant *C = ConstantExpr::getSub(CLHS, CRHS)) {
        SimplifiedValues[&I] = C;
        ++NumConstantPtrDiffs;
        return true;
      }
    }
  }

  // Otherwise, fall back to the generic logic for simplifying and handling
  // instructions.
  return Base::visitSub(I);
}

bool CallAnalyzer::visitBinaryOperator(BinaryOperator &I) {
  Value *LHS = I.getOperand(0), *RHS = I.getOperand(1);
  if (!isa<Constant>(LHS))
    if (Constant *SimpleLHS = SimplifiedValues.lookup(LHS))
      LHS = SimpleLHS;
  if (!isa<Constant>(RHS))
    if (Constant *SimpleRHS = SimplifiedValues.lookup(RHS))
      RHS = SimpleRHS;
  Value *SimpleV = SimplifyBinOp(I.getOpcode(), LHS, RHS, DL);
  if (Constant *C = dyn_cast_or_null<Constant>(SimpleV)) {
    SimplifiedValues[&I] = C;
    return true;
  }

  // Disable any SROA on arguments to arbitrary, unsimplified binary operators.
  disableSROA(LHS);
  disableSROA(RHS);

  return false;
}

bool CallAnalyzer::visitLoad(LoadInst &I) {
  Value *SROAArg;
  DenseMap<Value *, int>::iterator CostIt;
  if (lookupSROAArgAndCost(I.getOperand(0), SROAArg, CostIt)) {
    if (I.isSimple()) {
      accumulateSROACost(CostIt, InlineConstants::InstrCost);
      return true;
    }

    disableSROA(CostIt);
  }

  return false;
}

bool CallAnalyzer::visitStore(StoreInst &I) {
  Value *SROAArg;
  DenseMap<Value *, int>::iterator CostIt;
  if (lookupSROAArgAndCost(I.getOperand(0), SROAArg, CostIt)) {
    if (I.isSimple()) {
      accumulateSROACost(CostIt, InlineConstants::InstrCost);
      return true;
    }

    disableSROA(CostIt);
  }

  return false;
}

bool CallAnalyzer::visitExtractValue(ExtractValueInst &I) {
  // Constant folding for extract value is trivial.
  Constant *C = dyn_cast<Constant>(I.getAggregateOperand());
  if (!C)
    C = SimplifiedValues.lookup(I.getAggregateOperand());
  if (C) {
    SimplifiedValues[&I] = ConstantExpr::getExtractValue(C, I.getIndices());
    return true;
  }

  // SROA can look through these but give them a cost.
  return false;
}

bool CallAnalyzer::visitInsertValue(InsertValueInst &I) {
  // Constant folding for insert value is trivial.
  Constant *AggC = dyn_cast<Constant>(I.getAggregateOperand());
  if (!AggC)
    AggC = SimplifiedValues.lookup(I.getAggregateOperand());
  Constant *InsertedC = dyn_cast<Constant>(I.getInsertedValueOperand());
  if (!InsertedC)
    InsertedC = SimplifiedValues.lookup(I.getInsertedValueOperand());
  if (AggC && InsertedC) {
    SimplifiedValues[&I] = ConstantExpr::getInsertValue(AggC, InsertedC,
                                                        I.getIndices());
    return true;
  }

  // SROA can look through these but give them a cost.
  return false;
}

/// \brief Try to simplify a call site.
///
/// Takes a concrete function and callsite and tries to actually simplify it by
/// analyzing the arguments and call itself with instsimplify. Returns true if
/// it has simplified the callsite to some other entity (a constant), making it
/// free.
bool CallAnalyzer::simplifyCallSite(Function *F, CallSite CS) {
  // FIXME: Using the instsimplify logic directly for this is inefficient
  // because we have to continually rebuild the argument list even when no
  // simplifications can be performed. Until that is fixed with remapping
  // inside of instsimplify, directly constant fold calls here.
  if (!canConstantFoldCallTo(F))
    return false;

  // Try to re-map the arguments to constants.
  SmallVector<Constant *, 4> ConstantArgs;
  ConstantArgs.reserve(CS.arg_size());
  for (CallSite::arg_iterator I = CS.arg_begin(), E = CS.arg_end();
       I != E; ++I) {
    Constant *C = dyn_cast<Constant>(*I);
    if (!C)
      C = dyn_cast_or_null<Constant>(SimplifiedValues.lookup(*I));
    if (!C)
      return false; // This argument doesn't map to a constant.

    ConstantArgs.push_back(C);
  }
  if (Constant *C = ConstantFoldCall(F, ConstantArgs)) {
    SimplifiedValues[CS.getInstruction()] = C;
    return true;
  }

  return false;
}

bool CallAnalyzer::visitCallSite(CallSite CS) {
  if (CS.hasFnAttr(Attribute::ReturnsTwice) &&
      !F.getAttributes().hasAttribute(AttributeSet::FunctionIndex,
                                      Attribute::ReturnsTwice)) {
    // This aborts the entire analysis.
    ExposesReturnsTwice = true;
    return false;
  }
  if (CS.isCall() &&
      cast<CallInst>(CS.getInstruction())->hasFnAttr(Attribute::NoDuplicate))
    ContainsNoDuplicateCall = true;

  if (Function *F = CS.getCalledFunction()) {
    // When we have a concrete function, first try to simplify it directly.
    if (simplifyCallSite(F, CS))
      return true;

    // Next check if it is an intrinsic we know about.
    // FIXME: Lift this into part of the InstVisitor.
    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(CS.getInstruction())) {
      switch (II->getIntrinsicID()) {
      default:
        return Base::visitCallSite(CS);

      case Intrinsic::memset:
      case Intrinsic::memcpy:
      case Intrinsic::memmove:
        // SROA can usually chew through these intrinsics, but they aren't free.
        return false;
      }
    }

    if (F == CS.getInstruction()->getParent()->getParent()) {
      // This flag will fully abort the analysis, so don't bother with anything
      // else.
      IsRecursiveCall = true;
      return false;
    }

    if (TTI.isLoweredToCall(F)) {
      // We account for the average 1 instruction per call argument setup
      // here.
      Cost += CS.arg_size() * InlineConstants::InstrCost;

      // Everything other than inline ASM will also have a significant cost
      // merely from making the call.
      if (!isa<InlineAsm>(CS.getCalledValue()))
        Cost += InlineConstants::CallPenalty;
    }

    return Base::visitCallSite(CS);
  }

  // Otherwise we're in a very special case -- an indirect function call. See
  // if we can be particularly clever about this.
  Value *Callee = CS.getCalledValue();

  // First, pay the price of the argument setup. We account for the average
  // 1 instruction per call argument setup here.
  Cost += CS.arg_size() * InlineConstants::InstrCost;

  // Next, check if this happens to be an indirect function call to a known
  // function in this inline context. If not, we've done all we can.
  Function *F = dyn_cast_or_null<Function>(SimplifiedValues.lookup(Callee));
  if (!F)
    return Base::visitCallSite(CS);

  // If we have a constant that we are calling as a function, we can peer
  // through it and see the function target. This happens not infrequently
  // during devirtualization and so we want to give it a hefty bonus for
  // inlining, but cap that bonus in the event that inlining wouldn't pan
  // out. Pretend to inline the function, with a custom threshold.
  CallAnalyzer CA(DL, TTI, *F, InlineConstants::IndirectCallThreshold);
  if (CA.analyzeCall(CS)) {
    // We were able to inline the indirect call! Subtract the cost from the
    // bonus we want to apply, but don't go below zero.
    Cost -= std::max(0, InlineConstants::IndirectCallThreshold - CA.getCost());
  }

  return Base::visitCallSite(CS);
}

bool CallAnalyzer::visitReturnInst(ReturnInst &RI) {
  // At least one return instruction will be free after inlining.
  bool Free = !HasReturn;
  HasReturn = true;
  return Free;
}

bool CallAnalyzer::visitBranchInst(BranchInst &BI) {
  // We model unconditional branches as essentially free -- they really
  // shouldn't exist at all, but handling them makes the behavior of the
  // inliner more regular and predictable. Interestingly, conditional branches
  // which will fold away are also free.
  return BI.isUnconditional() || isa<ConstantInt>(BI.getCondition()) ||
         dyn_cast_or_null<ConstantInt>(
             SimplifiedValues.lookup(BI.getCondition()));
}

bool CallAnalyzer::visitSwitchInst(SwitchInst &SI) {
  // We model unconditional switches as free, see the comments on handling
  // branches.
  return isa<ConstantInt>(SI.getCondition()) ||
         dyn_cast_or_null<ConstantInt>(
             SimplifiedValues.lookup(SI.getCondition()));
}

bool CallAnalyzer::visitIndirectBrInst(IndirectBrInst &IBI) {
  // We never want to inline functions that contain an indirectbr.  This is
  // incorrect because all the blockaddress's (in static global initializers
  // for example) would be referring to the original function, and this
  // indirect jump would jump from the inlined copy of the function into the
  // original function which is extremely undefined behavior.
  // FIXME: This logic isn't really right; we can safely inline functions with
  // indirectbr's as long as no other function or global references the
  // blockaddress of a block within the current function.  And as a QOI issue,
  // if someone is using a blockaddress without an indirectbr, and that
  // reference somehow ends up in another function or global, we probably don't
  // want to inline this function.
  HasIndirectBr = true;
  return false;
}

bool CallAnalyzer::visitResumeInst(ResumeInst &RI) {
  // FIXME: It's not clear that a single instruction is an accurate model for
  // the inline cost of a resume instruction.
  return false;
}

bool CallAnalyzer::visitUnreachableInst(UnreachableInst &I) {
  // FIXME: It might be reasonably to discount the cost of instructions leading
  // to unreachable as they have the lowest possible impact on both runtime and
  // code size.
  return true; // No actual code is needed for unreachable.
}

bool CallAnalyzer::visitInstruction(Instruction &I) {
  // Some instructions are free. All of the free intrinsics can also be
  // handled by SROA, etc.
  if (TargetTransformInfo::TCC_Free == TTI.getUserCost(&I))
    return true;

  // We found something we don't understand or can't handle. Mark any SROA-able
  // values in the operand list as no longer viable.
  for (User::op_iterator OI = I.op_begin(), OE = I.op_end(); OI != OE; ++OI)
    disableSROA(*OI);

  return false;
}


/// \brief Analyze a basic block for its contribution to the inline cost.
///
/// This method walks the analyzer over every instruction in the given basic
/// block and accounts for their cost during inlining at this callsite. It
/// aborts early if the threshold has been exceeded or an impossible to inline
/// construct has been detected. It returns false if inlining is no longer
/// viable, and true if inlining remains viable.
bool CallAnalyzer::analyzeBlock(BasicBlock *BB) {
  for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
    // FIXME: Currently, the number of instructions in a function regardless of
    // our ability to simplify them during inline to constants or dead code,
    // are actually used by the vector bonus heuristic. As long as that's true,
    // we have to special case debug intrinsics here to prevent differences in
    // inlining due to debug symbols. Eventually, the number of unsimplified
    // instructions shouldn't factor into the cost computation, but until then,
    // hack around it here.
    if (isa<DbgInfoIntrinsic>(I))
      continue;

    ++NumInstructions;
    if (isa<ExtractElementInst>(I) || I->getType()->isVectorTy())
      ++NumVectorInstructions;

    // If the instruction simplified to a constant, there is no cost to this
    // instruction. Visit the instructions using our InstVisitor to account for
    // all of the per-instruction logic. The visit tree returns true if we
    // consumed the instruction in any way, and false if the instruction's base
    // cost should count against inlining.
    if (Base::visit(I))
      ++NumInstructionsSimplified;
    else
      Cost += InlineConstants::InstrCost;

    // If the visit this instruction detected an uninlinable pattern, abort.
    if (IsRecursiveCall || ExposesReturnsTwice || HasDynamicAlloca ||
        HasIndirectBr)
      return false;

    // If the caller is a recursive function then we don't want to inline
    // functions which allocate a lot of stack space because it would increase
    // the caller stack usage dramatically.
    if (IsCallerRecursive &&
        AllocatedSize > InlineConstants::TotalAllocaSizeRecursiveCaller)
      return false;

    if (NumVectorInstructions > NumInstructions/2)
      VectorBonus = FiftyPercentVectorBonus;
    else if (NumVectorInstructions > NumInstructions/10)
      VectorBonus = TenPercentVectorBonus;
    else
      VectorBonus = 0;

    // Check if we've past the threshold so we don't spin in huge basic
    // blocks that will never inline.
    if (Cost > (Threshold + VectorBonus))
      return false;
  }

  return true;
}

/// \brief Compute the base pointer and cumulative constant offsets for V.
///
/// This strips all constant offsets off of V, leaving it the base pointer, and
/// accumulates the total constant offset applied in the returned constant. It
/// returns 0 if V is not a pointer, and returns the constant '0' if there are
/// no constant offsets applied.
ConstantInt *CallAnalyzer::stripAndComputeInBoundsConstantOffsets(Value *&V) {
  if (!DL || !V->getType()->isPointerTy())
    return 0;

  unsigned IntPtrWidth = DL->getPointerSizeInBits();
  APInt Offset = APInt::getNullValue(IntPtrWidth);

  // Even though we don't look through PHI nodes, we could be called on an
  // instruction in an unreachable block, which may be on a cycle.
  SmallPtrSet<Value *, 4> Visited;
  Visited.insert(V);
  do {
    if (GEPOperator *GEP = dyn_cast<GEPOperator>(V)) {
      if (!GEP->isInBounds() || !accumulateGEPOffset(*GEP, Offset))
        return 0;
      V = GEP->getPointerOperand();
    } else if (Operator::getOpcode(V) == Instruction::BitCast) {
      V = cast<Operator>(V)->getOperand(0);
    } else if (GlobalAlias *GA = dyn_cast<GlobalAlias>(V)) {
      if (GA->mayBeOverridden())
        break;
      V = GA->getAliasee();
    } else {
      break;
    }
    assert(V->getType()->isPointerTy() && "Unexpected operand type!");
  } while (Visited.insert(V));

  Type *IntPtrTy = DL->getIntPtrType(V->getContext());
  return cast<ConstantInt>(ConstantInt::get(IntPtrTy, Offset));
}

/// \brief Analyze a call site for potential inlining.
///
/// Returns true if inlining this call is viable, and false if it is not
/// viable. It computes the cost and adjusts the threshold based on numerous
/// factors and heuristics. If this method returns false but the computed cost
/// is below the computed threshold, then inlining was forcibly disabled by
/// some artifact of the routine.
bool CallAnalyzer::analyzeCall(CallSite CS) {
  ++NumCallsAnalyzed;

  // Track whether the post-inlining function would have more than one basic
  // block. A single basic block is often intended for inlining. Balloon the
  // threshold by 50% until we pass the single-BB phase.
  bool SingleBB = true;
  int SingleBBBonus = Threshold / 2;
  Threshold += SingleBBBonus;

  // Perform some tweaks to the cost and threshold based on the direct
  // callsite information.

  // We want to more aggressively inline vector-dense kernels, so up the
  // threshold, and we'll lower it if the % of vector instructions gets too
  // low.
  assert(NumInstructions == 0);
  assert(NumVectorInstructions == 0);
  FiftyPercentVectorBonus = Threshold;
  TenPercentVectorBonus = Threshold / 2;

  // Give out bonuses per argument, as the instructions setting them up will
  // be gone after inlining.
  for (unsigned I = 0, E = CS.arg_size(); I != E; ++I) {
    if (DL && CS.isByValArgument(I)) {
      // We approximate the number of loads and stores needed by dividing the
      // size of the byval type by the target's pointer size.
      PointerType *PTy = cast<PointerType>(CS.getArgument(I)->getType());
      unsigned TypeSize = DL->getTypeSizeInBits(PTy->getElementType());
      unsigned PointerSize = DL->getPointerSizeInBits();
      // Ceiling division.
      unsigned NumStores = (TypeSize + PointerSize - 1) / PointerSize;

      // If it generates more than 8 stores it is likely to be expanded as an
      // inline memcpy so we take that as an upper bound. Otherwise we assume
      // one load and one store per word copied.
      // FIXME: The maxStoresPerMemcpy setting from the target should be used
      // here instead of a magic number of 8, but it's not available via
      // DataLayout.
      NumStores = std::min(NumStores, 8U);

      Cost -= 2 * NumStores * InlineConstants::InstrCost;
    } else {
      // For non-byval arguments subtract off one instruction per call
      // argument.
      Cost -= InlineConstants::InstrCost;
    }
  }

  // If there is only one call of the function, and it has internal linkage,
  // the cost of inlining it drops dramatically.
  bool OnlyOneCallAndLocalLinkage = F.hasLocalLinkage() && F.hasOneUse() &&
    &F == CS.getCalledFunction();
  if (OnlyOneCallAndLocalLinkage)
    Cost += InlineConstants::LastCallToStaticBonus;

  // If the instruction after the call, or if the normal destination of the
  // invoke is an unreachable instruction, the function is noreturn. As such,
  // there is little point in inlining this unless there is literally zero
  // cost.
  Instruction *Instr = CS.getInstruction();
  if (InvokeInst *II = dyn_cast<InvokeInst>(Instr)) {
    if (isa<UnreachableInst>(II->getNormalDest()->begin()))
      Threshold = 1;
  } else if (isa<UnreachableInst>(++BasicBlock::iterator(Instr)))
    Threshold = 1;

  // If this function uses the coldcc calling convention, prefer not to inline
  // it.
  if (F.getCallingConv() == CallingConv::Cold)
    Cost += InlineConstants::ColdccPenalty;

  // Check if we're done. This can happen due to bonuses and penalties.
  if (Cost > Threshold)
    return false;

  if (F.empty())
    return true;

  Function *Caller = CS.getInstruction()->getParent()->getParent();
  // Check if the caller function is recursive itself.
  for (Value::use_iterator U = Caller->use_begin(), E = Caller->use_end();
       U != E; ++U) {
    CallSite Site(cast<Value>(*U));
    if (!Site)
      continue;
    Instruction *I = Site.getInstruction();
    if (I->getParent()->getParent() == Caller) {
      IsCallerRecursive = true;
      break;
    }
  }

  // Populate our simplified values by mapping from function arguments to call
  // arguments with known important simplifications.
  CallSite::arg_iterator CAI = CS.arg_begin();
  for (Function::arg_iterator FAI = F.arg_begin(), FAE = F.arg_end();
       FAI != FAE; ++FAI, ++CAI) {
    assert(CAI != CS.arg_end());
    if (Constant *C = dyn_cast<Constant>(CAI))
      SimplifiedValues[FAI] = C;

    Value *PtrArg = *CAI;
    if (ConstantInt *C = stripAndComputeInBoundsConstantOffsets(PtrArg)) {
      ConstantOffsetPtrs[FAI] = std::make_pair(PtrArg, C->getValue());

      // We can SROA any pointer arguments derived from alloca instructions.
      if (isa<AllocaInst>(PtrArg)) {
        SROAArgValues[FAI] = PtrArg;
        SROAArgCosts[PtrArg] = 0;
      }
    }
  }
  NumConstantArgs = SimplifiedValues.size();
  NumConstantOffsetPtrArgs = ConstantOffsetPtrs.size();
  NumAllocaArgs = SROAArgValues.size();

  // The worklist of live basic blocks in the callee *after* inlining. We avoid
  // adding basic blocks of the callee which can be proven to be dead for this
  // particular call site in order to get more accurate cost estimates. This
  // requires a somewhat heavyweight iteration pattern: we need to walk the
  // basic blocks in a breadth-first order as we insert live successors. To
  // accomplish this, prioritizing for small iterations because we exit after
  // crossing our threshold, we use a small-size optimized SetVector.
  typedef SetVector<BasicBlock *, SmallVector<BasicBlock *, 16>,
                                  SmallPtrSet<BasicBlock *, 16> > BBSetVector;
  BBSetVector BBWorklist;
  BBWorklist.insert(&F.getEntryBlock());
  // Note that we *must not* cache the size, this loop grows the worklist.
  for (unsigned Idx = 0; Idx != BBWorklist.size(); ++Idx) {
    // Bail out the moment we cross the threshold. This means we'll under-count
    // the cost, but only when undercounting doesn't matter.
    if (Cost > (Threshold + VectorBonus))
      break;

    BasicBlock *BB = BBWorklist[Idx];
    if (BB->empty())
      continue;

    // Analyze the cost of this block. If we blow through the threshold, this
    // returns false, and we can bail on out.
    if (!analyzeBlock(BB)) {
      if (IsRecursiveCall || ExposesReturnsTwice || HasDynamicAlloca ||
          HasIndirectBr)
        return false;

      // If the caller is a recursive function then we don't want to inline
      // functions which allocate a lot of stack space because it would increase
      // the caller stack usage dramatically.
      if (IsCallerRecursive &&
          AllocatedSize > InlineConstants::TotalAllocaSizeRecursiveCaller)
        return false;

      break;
    }

    TerminatorInst *TI = BB->getTerminator();

    // Add in the live successors by first checking whether we have terminator
    // that may be simplified based on the values simplified by this call.
    if (BranchInst *BI = dyn_cast<BranchInst>(TI)) {
      if (BI->isConditional()) {
        Value *Cond = BI->getCondition();
        if (ConstantInt *SimpleCond
              = dyn_cast_or_null<ConstantInt>(SimplifiedValues.lookup(Cond))) {
          BBWorklist.insert(BI->getSuccessor(SimpleCond->isZero() ? 1 : 0));
          continue;
        }
      }
    } else if (SwitchInst *SI = dyn_cast<SwitchInst>(TI)) {
      Value *Cond = SI->getCondition();
      if (ConstantInt *SimpleCond
            = dyn_cast_or_null<ConstantInt>(SimplifiedValues.lookup(Cond))) {
        BBWorklist.insert(SI->findCaseValue(SimpleCond).getCaseSuccessor());
        continue;
      }
    }

    // If we're unable to select a particular successor, just count all of
    // them.
    for (unsigned TIdx = 0, TSize = TI->getNumSuccessors(); TIdx != TSize;
         ++TIdx)
      BBWorklist.insert(TI->getSuccessor(TIdx));

    // If we had any successors at this point, than post-inlining is likely to
    // have them as well. Note that we assume any basic blocks which existed
    // due to branches or switches which folded above will also fold after
    // inlining.
    if (SingleBB && TI->getNumSuccessors() > 1) {
      // Take off the bonus we applied to the threshold.
      Threshold -= SingleBBBonus;
      SingleBB = false;
    }
  }

  // If this is a noduplicate call, we can still inline as long as
  // inlining this would cause the removal of the caller (so the instruction
  // is not actually duplicated, just moved).
  if (!OnlyOneCallAndLocalLinkage && ContainsNoDuplicateCall)
    return false;

  Threshold += VectorBonus;

  return Cost < Threshold;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
/// \brief Dump stats about this call's analysis.
void CallAnalyzer::dump() {
#define DEBUG_PRINT_STAT(x) dbgs() << "      " #x ": " << x << "\n"
  DEBUG_PRINT_STAT(NumConstantArgs);
  DEBUG_PRINT_STAT(NumConstantOffsetPtrArgs);
  DEBUG_PRINT_STAT(NumAllocaArgs);
  DEBUG_PRINT_STAT(NumConstantPtrCmps);
  DEBUG_PRINT_STAT(NumConstantPtrDiffs);
  DEBUG_PRINT_STAT(NumInstructionsSimplified);
  DEBUG_PRINT_STAT(SROACostSavings);
  DEBUG_PRINT_STAT(SROACostSavingsLost);
  DEBUG_PRINT_STAT(ContainsNoDuplicateCall);
  DEBUG_PRINT_STAT(Cost);
  DEBUG_PRINT_STAT(Threshold);
  DEBUG_PRINT_STAT(VectorBonus);
#undef DEBUG_PRINT_STAT
}
#endif

INITIALIZE_PASS_BEGIN(InlineCostAnalysis, "inline-cost", "Inline Cost Analysis",
                      true, true)
INITIALIZE_AG_DEPENDENCY(TargetTransformInfo)
INITIALIZE_PASS_END(InlineCostAnalysis, "inline-cost", "Inline Cost Analysis",
                    true, true)

char InlineCostAnalysis::ID = 0;

InlineCostAnalysis::InlineCostAnalysis() : CallGraphSCCPass(ID) {}

InlineCostAnalysis::~InlineCostAnalysis() {}

void InlineCostAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<TargetTransformInfo>();
  CallGraphSCCPass::getAnalysisUsage(AU);
}

bool InlineCostAnalysis::runOnSCC(CallGraphSCC &SCC) {
  TTI = &getAnalysis<TargetTransformInfo>();
  return false;
}

InlineCost InlineCostAnalysis::getInlineCost(CallSite CS, int Threshold) {
  return getInlineCost(CS, CS.getCalledFunction(), Threshold);
}

/// \brief Test that two functions either have or have not the given attribute
///        at the same time.
static bool attributeMatches(Function *F1, Function *F2,
                             Attribute::AttrKind Attr) {
  return F1->hasFnAttribute(Attr) == F2->hasFnAttribute(Attr);
}

/// \brief Test that there are no attribute conflicts between Caller and Callee
///        that prevent inlining.
static bool functionsHaveCompatibleAttributes(Function *Caller,
                                              Function *Callee) {
  return attributeMatches(Caller, Callee, Attribute::SanitizeAddress) &&
         attributeMatches(Caller, Callee, Attribute::SanitizeMemory) &&
         attributeMatches(Caller, Callee, Attribute::SanitizeThread);
}

InlineCost InlineCostAnalysis::getInlineCost(CallSite CS, Function *Callee,
                                             int Threshold) {
  // Cannot inline indirect calls.
  if (!Callee)
    return llvm::InlineCost::getNever();

  // Calls to functions with always-inline attributes should be inlined
  // whenever possible.
  if (Callee->hasFnAttribute(Attribute::AlwaysInline)) {
    if (isInlineViable(*Callee))
      return llvm::InlineCost::getAlways();
    return llvm::InlineCost::getNever();
  }

  // Never inline functions with conflicting attributes (unless callee has
  // always-inline attribute).
  if (!functionsHaveCompatibleAttributes(CS.getCaller(), Callee))
    return llvm::InlineCost::getNever();

  // Don't inline this call if the caller has the optnone attribute.
  if (CS.getCaller()->hasFnAttribute(Attribute::OptimizeNone))
    return llvm::InlineCost::getNever();

  // Don't inline functions which can be redefined at link-time to mean
  // something else.  Don't inline functions marked noinline or call sites
  // marked noinline.
  if (Callee->mayBeOverridden() ||
      Callee->hasFnAttribute(Attribute::NoInline) || CS.isNoInline())
    return llvm::InlineCost::getNever();

  DEBUG(llvm::dbgs() << "      Analyzing call of " << Callee->getName()
        << "...\n");

  CallAnalyzer CA(Callee->getDataLayout(), *TTI, *Callee, Threshold);
  bool ShouldInline = CA.analyzeCall(CS);

  DEBUG(CA.dump());

  // Check if there was a reason to force inlining or no inlining.
  if (!ShouldInline && CA.getCost() < CA.getThreshold())
    return InlineCost::getNever();
  if (ShouldInline && CA.getCost() >= CA.getThreshold())
    return InlineCost::getAlways();

  return llvm::InlineCost::get(CA.getCost(), CA.getThreshold());
}

bool InlineCostAnalysis::isInlineViable(Function &F) {
  bool ReturnsTwice =
    F.getAttributes().hasAttribute(AttributeSet::FunctionIndex,
                                   Attribute::ReturnsTwice);
  for (Function::iterator BI = F.begin(), BE = F.end(); BI != BE; ++BI) {
    // Disallow inlining of functions which contain an indirect branch.
    if (isa<IndirectBrInst>(BI->getTerminator()))
      return false;

    for (BasicBlock::iterator II = BI->begin(), IE = BI->end(); II != IE;
         ++II) {
      CallSite CS(II);
      if (!CS)
        continue;

      // Disallow recursive calls.
      if (&F == CS.getCalledFunction())
        return false;

      // Disallow calls which expose returns-twice to a function not previously
      // attributed as such.
      if (!ReturnsTwice && CS.isCall() &&
          cast<CallInst>(CS.getInstruction())->canReturnTwice())
        return false;
    }
  }

  return true;
}
