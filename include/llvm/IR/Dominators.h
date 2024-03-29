//===- Dominators.h - Dominator Info Calculation ----------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the DominatorTree class, which provides fast and efficient
// dominance queries.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_DOMINATORS_H
#define LLVM_IR_DOMINATORS_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/GenericDomTree.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

namespace llvm {

EXTERN_TEMPLATE_INSTANTIATION(class DomTreeNodeBase<BasicBlock>);
EXTERN_TEMPLATE_INSTANTIATION(class DominatorTreeBase<BasicBlock>);

#define LLVM_COMMA ,
EXTERN_TEMPLATE_INSTANTIATION(void Calculate<Function LLVM_COMMA BasicBlock *>(
    DominatorTreeBase<GraphTraits<BasicBlock *>::NodeType> &DT LLVM_COMMA
        Function &F));
EXTERN_TEMPLATE_INSTANTIATION(
    void Calculate<Function LLVM_COMMA Inverse<BasicBlock *> >(
        DominatorTreeBase<GraphTraits<Inverse<BasicBlock *> >::NodeType> &DT
            LLVM_COMMA Function &F));
#undef LLVM_COMMA

typedef DomTreeNodeBase<BasicBlock> DomTreeNode;

class BasicBlockEdge {
  const BasicBlock *Start;
  const BasicBlock *End;
public:
  BasicBlockEdge(const BasicBlock *Start_, const BasicBlock *End_) :
    Start(Start_), End(End_) { }
  const BasicBlock *getStart() const {
    return Start;
  }
  const BasicBlock *getEnd() const {
    return End;
  }
  bool isSingleEdge() const;
};

/// \brief Concrete subclass of DominatorTreeBase that is used to compute a
/// normal dominator tree.
class DominatorTree : public DominatorTreeBase<BasicBlock> {
public:
  typedef DominatorTreeBase<BasicBlock> Base;

  DominatorTree() : DominatorTreeBase<BasicBlock>(false) {}

  // FIXME: This is no longer needed and should be removed when its uses are
  // cleaned up.
  Base& getBase() { return *this; }

  /// \brief Returns *false* if the other dominator tree matches this dominator
  /// tree.
  inline bool compare(const DominatorTree &Other) const {
    const DomTreeNode *R = getRootNode();
    const DomTreeNode *OtherR = Other.getRootNode();

    if (!R || !OtherR || R->getBlock() != OtherR->getBlock())
      return true;

    if (Base::compare(Other))
      return true;

    return false;
  }

  // Ensure base-class overloads are visible.
  using Base::dominates;

  /// \brief Return true if Def dominates a use in User.
  ///
  /// This performs the special checks necessary if Def and User are in the same
  /// basic block. Note that Def doesn't dominate a use in Def itself!
  bool dominates(const Instruction *Def, const Use &U) const;
  bool dominates(const Instruction *Def, const Instruction *User) const;
  bool dominates(const Instruction *Def, const BasicBlock *BB) const;
  bool dominates(const BasicBlockEdge &BBE, const Use &U) const;
  bool dominates(const BasicBlockEdge &BBE, const BasicBlock *BB) const;

  inline DomTreeNode *operator[](BasicBlock *BB) const {
    return getNode(BB);
  }

  // Ensure base class overloads are visible.
  using Base::isReachableFromEntry;

  /// \brief Provide an overload for a Use.
  bool isReachableFromEntry(const Use &U) const;

  /// \brief Verify the correctness of the domtree by re-computing it.
  ///
  /// This should only be used for debugging as it aborts the program if the
  /// verification fails.
  void verifyDomTree() const;
};

//===-------------------------------------
// DominatorTree GraphTraits specializations so the DominatorTree can be
// iterable by generic graph iterators.

template <> struct GraphTraits<DomTreeNode*> {
  typedef DomTreeNode NodeType;
  typedef NodeType::iterator  ChildIteratorType;

  static NodeType *getEntryNode(NodeType *N) {
    return N;
  }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->end();
  }

  typedef df_iterator<DomTreeNode*> nodes_iterator;

  static nodes_iterator nodes_begin(DomTreeNode *N) {
    return df_begin(getEntryNode(N));
  }

  static nodes_iterator nodes_end(DomTreeNode *N) {
    return df_end(getEntryNode(N));
  }
};

template <> struct GraphTraits<DominatorTree*>
  : public GraphTraits<DomTreeNode*> {
  static NodeType *getEntryNode(DominatorTree *DT) {
    return DT->getRootNode();
  }

  static nodes_iterator nodes_begin(DominatorTree *N) {
    return df_begin(getEntryNode(N));
  }

  static nodes_iterator nodes_end(DominatorTree *N) {
    return df_end(getEntryNode(N));
  }
};

/// \brief Analysis pass which computes a \c DominatorTree.
class DominatorTreeWrapperPass : public FunctionPass {
  DominatorTree DT;

public:
  static char ID;

  DominatorTreeWrapperPass() : FunctionPass(ID) {
    initializeDominatorTreeWrapperPassPass(*PassRegistry::getPassRegistry());
  }

  DominatorTree &getDomTree() { return DT; }
  const DominatorTree &getDomTree() const { return DT; }

  bool runOnFunction(Function &F) override;

  void verifyAnalysis() const override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }

  void releaseMemory() override { DT.releaseMemory(); }

  void print(raw_ostream &OS, const Module *M = 0) const override;
};

} // End llvm namespace

#endif
