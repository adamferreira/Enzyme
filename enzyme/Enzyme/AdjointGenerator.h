//===- AdjointGenerator.h - Implementation of Adjoint's of instructions --===//
//
//                             Enzyme Project
//
// Part of the Enzyme Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// If using this code in an academic setting, please cite the following:
// @incollection{enzymeNeurips,
// title = {Instead of Rewriting Foreign Code for Machine Learning,
//          Automatically Synthesize Fast Gradients},
// author = {Moses, William S. and Churavy, Valentin},
// booktitle = {Advances in Neural Information Processing Systems 33},
// year = {2020},
// note = {To appear in},
// }
//
//===----------------------------------------------------------------------===//
//
// This file contains an instruction visitor AdjointGenerator that generates
// the corresponding augmented forward pass code, and adjoints for all
// LLVM instructions.
//
//===----------------------------------------------------------------------===//
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Value.h"

#include "DifferentialUseAnalysis.h"
#include "EnzymeLogic.h"
#include "GradientUtils.h"
#include "LibraryFuncs.h"

#define DEBUG_TYPE "enzyme"
using namespace llvm;

// Helper instruction visitor that generates adjoints
template <class AugmentedReturnType = AugmentedReturn *>
class AdjointGenerator
    : public llvm::InstVisitor<AdjointGenerator<AugmentedReturnType>> {
private:
  // Type of code being generated (forward, reverse, or both)
  const DerivativeMode Mode;

  GradientUtils *const gutils;
  const std::vector<DIFFE_TYPE> &constant_args;
  TypeResults &TR;
  std::function<unsigned(Instruction *, CacheType)> getIndex;
  const std::map<CallInst *, const std::map<Argument *, bool>>
      uncacheable_args_map;
  const SmallPtrSetImpl<Instruction *> *returnuses;
  AugmentedReturnType augmentedReturn;
  const std::map<ReturnInst *, StoreInst *> *replacedReturns;

  const SmallPtrSetImpl<const Value *> &unnecessaryValues;
  const SmallPtrSetImpl<const Instruction *> &unnecessaryInstructions;
  const SmallPtrSetImpl<const Instruction *> &unnecessaryStores;

  AllocaInst *dretAlloca;

public:
  AdjointGenerator(
      DerivativeMode Mode, GradientUtils *gutils,
      const std::vector<DIFFE_TYPE> &constant_args, TypeResults &TR,
      std::function<unsigned(Instruction *, CacheType)> getIndex,
      const std::map<CallInst *, const std::map<Argument *, bool>>
          uncacheable_args_map,
      const SmallPtrSetImpl<Instruction *> *returnuses,
      AugmentedReturnType augmentedReturn,
      const std::map<ReturnInst *, StoreInst *> *replacedReturns,
      const SmallPtrSetImpl<const Value *> &unnecessaryValues,
      const SmallPtrSetImpl<const Instruction *> &unnecessaryInstructions,
      const SmallPtrSetImpl<const Instruction *> &unnecessaryStores,
      AllocaInst *dretAlloca)
      : Mode(Mode), gutils(gutils), constant_args(constant_args), TR(TR),
        getIndex(getIndex), uncacheable_args_map(uncacheable_args_map),
        returnuses(returnuses), augmentedReturn(augmentedReturn),
        replacedReturns(replacedReturns), unnecessaryValues(unnecessaryValues),
        unnecessaryInstructions(unnecessaryInstructions),
        unnecessaryStores(unnecessaryStores), dretAlloca(dretAlloca) {

    assert(TR.info.Function == gutils->oldFunc);
    for (auto &pair :
         TR.analysis.analyzedFunctions.find(TR.info)->second.analysis) {
      if (auto in = dyn_cast<Instruction>(pair.first)) {
        if (in->getParent()->getParent() != gutils->oldFunc) {
          llvm::errs() << "inf: " << *in->getParent()->getParent() << "\n";
          llvm::errs() << "gutils->oldFunc: " << *gutils->oldFunc << "\n";
          llvm::errs() << "in: " << *in << "\n";
        }
        assert(in->getParent()->getParent() == gutils->oldFunc);
      }
    }
  }

  SmallPtrSet<Instruction *, 4> erased;

  void eraseIfUnused(llvm::Instruction &I, bool erase = true,
                     bool check = true) {
    bool used =
        unnecessaryInstructions.find(&I) == unnecessaryInstructions.end();

    auto iload = gutils->getNewFromOriginal(&I);

    if (used && check)
      return;

    PHINode *pn = nullptr;
    if (!I.getType()->isVoidTy()) {
      IRBuilder<> BuilderZ(iload);
      pn = BuilderZ.CreatePHI(I.getType(), 1,
                              (I.getName() + "_replacementA").str());
      gutils->fictiousPHIs.push_back(pn);

      for (auto inst_orig : unnecessaryInstructions) {
        if (isa<ReturnInst>(inst_orig))
          continue;
        if (erased.count(inst_orig))
          continue;
        auto inst = gutils->getNewFromOriginal(inst_orig);
        for (unsigned i = 0; i < inst->getNumOperands(); ++i) {
          if (inst->getOperand(i) == iload) {
            inst->setOperand(i, pn);
          }
        }
      }
    }

    erased.insert(&I);
    if (erase) {
      if (pn)
        gutils->replaceAWithB(iload, pn);
      gutils->erase(iload);
    }
  }

  void visitInstruction(llvm::Instruction &inst) {
    // TODO explicitly handle all instructions rather than using the catch all
    // below
    if (Mode == DerivativeMode::Forward)
      return;

#if LLVM_VERSION_MAJOR >= 10
    if (auto *FPMO = dyn_cast<FPMathOperator>(&inst)) {
      if (FPMO->getOpcode() == Instruction::FNeg) {
        eraseIfUnused(inst);
        if (gutils->isConstantInstruction(&inst))
          return;
        if (Mode != DerivativeMode::Reverse && Mode != DerivativeMode::Both)
          return;

        Value *orig_op1 = FPMO->getOperand(0);
        bool constantval1 = gutils->isConstantValue(orig_op1);

        IRBuilder<> Builder2(inst.getParent());
        getReverseBuilder(Builder2);

        Value *idiff = diffe(FPMO, Builder2);

        if (!constantval1) {
          Value *dif1 = Builder2.CreateFNeg(idiff);
          setDiffe(FPMO, Constant::getNullValue(FPMO->getType()), Builder2);
          addToDiffe(orig_op1, dif1, Builder2,
                     dif1->getType()->getScalarType());
        }
        return;
      }
    }
#endif

    llvm::errs() << *gutils->oldFunc << "\n";
    llvm::errs() << *gutils->newFunc << "\n";
    llvm::errs() << "in Mode: " << to_string(Mode) << "\n";
    llvm::errs() << "cannot handle unknown instruction\n" << inst;
    report_fatal_error("unknown value");
  }

  void visitAllocaInst(llvm::AllocaInst &I) { eraseIfUnused(I); }
  void visitICmpInst(llvm::ICmpInst &I) { eraseIfUnused(I); }

  void visitFCmpInst(llvm::FCmpInst &I) { eraseIfUnused(I); }

  void visitLoadInst(llvm::LoadInst &LI) {
    // If a load of an omp init argument, don't cache for reverse
    // and don't do any adjoint propagation (assumed integral)
    for (auto U : LI.getPointerOperand()->users()) {
      if (auto CI = dyn_cast<CallInst>(U)) {
        if (auto F = CI->getCalledFunction()) {
          if (F->getName() == "__kmpc_for_static_init_4") {
            eraseIfUnused(LI);
            return;
          }
        }
      }
    }

    bool constantval = gutils->isConstantValue(&LI);
    // even if this is an active value if it has no active users
    // (e.g. potential but unused active pointer), it does not
    // need an adjoint here
    if (!constantval) {
      constantval |= gutils->ATA->isValueInactiveFromUsers(TR, &LI);
    }
#if LLVM_VERSION_MAJOR >= 10
    auto alignment = LI.getAlign();
#else
    auto alignment = LI.getAlignment();
#endif

    BasicBlock *parent = LI.getParent();
    Type *type = LI.getType();

    LoadInst *newi = dyn_cast<LoadInst>(gutils->getNewFromOriginal(&LI));

    //! Store inverted pointer loads that need to be cached for use in reverse
    //! pass
    if (!type->isEmptyTy() && !type->isFPOrFPVectorTy() &&
        TR.query(&LI).Inner0().isPossiblePointer()) {
      PHINode *placeholder = cast<PHINode>(gutils->invertedPointers[&LI]);
      assert(placeholder->getType() == type);
      gutils->invertedPointers.erase(&LI);

      if (!constantval) {
        IRBuilder<> BuilderZ(placeholder);
        Value *newip = nullptr;

        bool needShadow = is_value_needed_in_reverse<Shadow>(
            TR, gutils, &LI, /*toplevel*/ Mode == DerivativeMode::Both);

        switch (Mode) {

        case DerivativeMode::Forward:
        case DerivativeMode::Both: {
          newip = gutils->invertPointerM(&LI, BuilderZ);
          assert(newip->getType() == type);

          if (Mode == DerivativeMode::Forward &&
              gutils->can_modref_map->find(&LI)->second && needShadow) {
            gutils->cacheForReverse(BuilderZ, newip,
                                    getIndex(&LI, CacheType::Shadow));
          }
          placeholder->replaceAllUsesWith(newip);
          gutils->erase(placeholder);
          gutils->invertedPointers[&LI] = newip;
          break;
        }

        case DerivativeMode::Reverse: {
          // only make shadow where caching needed
          if (gutils->can_modref_map->find(&LI)->second && needShadow) {
            newip = gutils->cacheForReverse(BuilderZ, placeholder,
                                            getIndex(&LI, CacheType::Shadow));
            assert(newip->getType() == type);
            gutils->invertedPointers[&LI] = newip;
          } else {
            newip = gutils->invertPointerM(&LI, BuilderZ);
            assert(newip->getType() == type);
            placeholder->replaceAllUsesWith(newip);
            gutils->erase(placeholder);
            gutils->invertedPointers[&LI] = newip;
          }
          break;
        }
        }

      } else {
        gutils->erase(placeholder);
      }
    }

    eraseIfUnused(LI);

    // Allow forcing cache reads to be on or off using flags.
    assert(!(cache_reads_always && cache_reads_never) &&
           "Both cache_reads_always and cache_reads_never are true. This "
           "doesn't make sense.");

    Value *inst = newi;

    //! Store loads that need to be cached for use in reverse pass
    if (cache_reads_always ||
        (!cache_reads_never && gutils->can_modref_map->find(&LI)->second &&
         is_value_needed_in_reverse<Primal>(
             TR, gutils, &LI, /*toplevel*/ Mode == DerivativeMode::Both))) {
      IRBuilder<> BuilderZ(gutils->getNewFromOriginal(&LI)->getNextNode());
      // auto tbaa = inst->getMetadata(LLVMContext::MD_tbaa);

      inst = gutils->cacheForReverse(BuilderZ, newi,
                                     getIndex(&LI, CacheType::Self));
      assert(inst->getType() == type);

      if (Mode == DerivativeMode::Reverse) {
        assert(inst != newi);
      } else {
        assert(inst == newi);
      }
    }

    if (Mode == DerivativeMode::Forward)
      return;

    if (constantval)
      return;

    if (nonmarkedglobals_inactiveloads) {
      // Assume that non enzyme_shadow globals are inactive
      //  If we ever store to a global variable, we will error if it doesn't
      //  have a shadow This allows functions who only read global memory to
      //  have their derivative computed Note that this is too aggressive for
      //  general programs as if the global aliases with an argument something
      //  that is written to, then we will have a logical error
      if (auto arg = dyn_cast<GlobalVariable>(LI.getPointerOperand())) {
        if (!hasMetadata(arg, "enzyme_shadow")) {
          return;
        }
      }
    }

    bool isfloat = type->isFPOrFPVectorTy();
    if (!isfloat && type->isIntOrIntVectorTy()) {
      auto storeSize =
          gutils->newFunc->getParent()->getDataLayout().getTypeSizeInBits(
              type) /
          8;
      auto vd =
          TR.firstPointer(storeSize, LI.getPointerOperand(),
                          /*errifnotfound*/ false, /*pointerIntSame*/ true);
      if (vd.isKnown())
        isfloat = vd.isFloat();
      else
        isfloat =
            TR.intType(storeSize, &LI, /*errIfNotFound*/ !looseTypeAnalysis)
                .isFloat();
    }

    if (isfloat) {
      IRBuilder<> Builder2(parent);
      getReverseBuilder(Builder2);
      auto prediff = diffe(&LI, Builder2);
      setDiffe(&LI, Constant::getNullValue(type), Builder2);
      // llvm::errs() << "  + doing load propagation: orig:" << *oorig << "
      // inst:" << *inst << " prediff: " << *prediff << " inverted_operand: " <<
      // *inverted_operand << "\n";

      if (!gutils->isConstantValue(LI.getPointerOperand())) {
        Value *inverted_operand =
            gutils->invertPointerM(LI.getPointerOperand(), Builder2);
        assert(inverted_operand);
        ((DiffeGradientUtils *)gutils)
            ->addToInvertedPtrDiffe(inverted_operand, prediff, Builder2,
                                    alignment);
      }
    }
  }

  void visitStoreInst(llvm::StoreInst &SI) {
    Value *orig_ptr = SI.getPointerOperand();
    Value *orig_val = SI.getValueOperand();
    Value *val = gutils->getNewFromOriginal(orig_val);
    Type *valType = orig_val->getType();

    // If a store of an omp init argument, don't delete in reverse
    // and don't do any adjoint propagation (assumed integral)
    for (auto U : orig_ptr->users()) {
      if (auto CI = dyn_cast<CallInst>(U)) {
        if (auto F = CI->getCalledFunction()) {
          if (F->getName() == "__kmpc_for_static_init_4") {
            return;
          }
        }
      }
    }

    if (unnecessaryStores.count(&SI)) {
      eraseIfUnused(SI);
      return;
    }

    if (gutils->isConstantValue(orig_ptr)) {
      eraseIfUnused(SI);
      return;
    }

    // TODO allow recognition of other types that could contain pointers [e.g.
    // {void*, void*} or <2 x i64> ]
    StoreInst *ts = nullptr;

    auto storeSize =
        gutils->newFunc->getParent()->getDataLayout().getTypeSizeInBits(
            valType) /
        8;

    //! Storing a floating point value
    Type *FT = nullptr;
    if (valType->isFPOrFPVectorTy()) {
      FT = valType->getScalarType();
    } else if (!valType->isPointerTy()) {
      if (looseTypeAnalysis) {
        auto fp = TR.firstPointer(storeSize, orig_ptr, /*errifnotfound*/ false,
                                  /*pointerIntSame*/ true);
        if (fp.isKnown()) {
          FT = fp.isFloat();
        } else if (isa<ConstantInt>(orig_val) ||
                   valType->isIntOrIntVectorTy()) {
          llvm::errs() << "assuming type as integral for store: " << SI << "\n";
          FT = nullptr;
        } else {
          TR.firstPointer(storeSize, orig_ptr, /*errifnotfound*/ true,
                          /*pointerIntSame*/ true);
          llvm::errs() << "cannot deduce type of store " << SI << "\n";
          assert(0 && "cannot deduce");
        }
      } else
        FT = TR.firstPointer(storeSize, orig_ptr, /*errifnotfound*/ true,
                             /*pointerIntSame*/ true)
                 .isFloat();
    }

    if (FT) {
      //! Only need to update the reverse function
      if (Mode == DerivativeMode::Reverse || Mode == DerivativeMode::Both) {
        IRBuilder<> Builder2(SI.getParent());
        getReverseBuilder(Builder2);

        if (gutils->isConstantValue(orig_val)) {
          ts = setPtrDiffe(orig_ptr, Constant::getNullValue(valType), Builder2);
        } else {
          auto dif1 =
              Builder2.CreateLoad(gutils->invertPointerM(orig_ptr, Builder2));
#if LLVM_VERSION_MAJOR >= 10
          dif1->setAlignment(SI.getAlign());
#else
          dif1->setAlignment(SI.getAlignment());
#endif
          ts = setPtrDiffe(orig_ptr, Constant::getNullValue(valType), Builder2);
          addToDiffe(orig_val, dif1, Builder2, FT);
        }
      }

      //! Storing an integer or pointer
    } else {
      //! Only need to update the forward function
      if (Mode == DerivativeMode::Forward || Mode == DerivativeMode::Both) {
        IRBuilder<> storeBuilder(gutils->getNewFromOriginal(&SI));

        Value *valueop = nullptr;

        // Fallback mechanism, TODO check
        if (gutils->isConstantValue(orig_val)) {
          valueop =
              val; // Constant::getNullValue(op->getValueOperand()->getType());
        } else {
          valueop = gutils->invertPointerM(orig_val, storeBuilder);
        }
        ts = setPtrDiffe(orig_ptr, valueop, storeBuilder);
      }
    }

    if (ts) {
#if LLVM_VERSION_MAJOR >= 10
      ts->setAlignment(SI.getAlign());
#else
      ts->setAlignment(SI.getAlignment());
#endif
      ts->setVolatile(SI.isVolatile());
      ts->setOrdering(SI.getOrdering());
      ts->setSyncScopeID(SI.getSyncScopeID());
    }
    eraseIfUnused(SI);
  }

  void visitGetElementPtrInst(llvm::GetElementPtrInst &gep) {
    eraseIfUnused(gep);
  }

  void visitPHINode(llvm::PHINode &phi) { eraseIfUnused(phi); }

  void visitCastInst(llvm::CastInst &I) {
    eraseIfUnused(I);
    if (gutils->isConstantInstruction(&I))
      return;

    if (I.getType()->isPointerTy() ||
        I.getOpcode() == CastInst::CastOps::PtrToInt)
      return;

    if (Mode == DerivativeMode::Forward)
      return;

    Value *orig_op0 = I.getOperand(0);
    Value *op0 = gutils->getNewFromOriginal(orig_op0);

    IRBuilder<> Builder2(I.getParent());
    getReverseBuilder(Builder2);

    if (!gutils->isConstantValue(orig_op0)) {
      Value *dif = diffe(&I, Builder2);

      size_t size = 1;
      if (orig_op0->getType()->isSized())
        size = (gutils->newFunc->getParent()->getDataLayout().getTypeSizeInBits(
                    orig_op0->getType()) +
                7) /
               8;
      if (I.getOpcode() == CastInst::CastOps::FPTrunc ||
          I.getOpcode() == CastInst::CastOps::FPExt) {
        addToDiffe(orig_op0, Builder2.CreateFPCast(dif, op0->getType()),
                   Builder2, TR.intType(size, orig_op0, false).isFloat());
      } else if (I.getOpcode() == CastInst::CastOps::BitCast) {
        addToDiffe(orig_op0, Builder2.CreateBitCast(dif, op0->getType()),
                   Builder2, TR.intType(size, orig_op0, false).isFloat());
      } else if (I.getOpcode() == CastInst::CastOps::Trunc) {
        // TODO CHECK THIS
        auto trunced = Builder2.CreateZExt(dif, op0->getType());
        addToDiffe(orig_op0, trunced, Builder2,
                   TR.intType(size, orig_op0, false).isFloat());
      } else {
        llvm::errs() << *I.getParent()->getParent() << "\n"
                     << *I.getParent() << "\n";
        llvm::errs() << "cannot handle above cast " << I << "\n";
        report_fatal_error("unknown instruction");
      }
    }
    setDiffe(&I, Constant::getNullValue(I.getType()), Builder2);
  }

  void visitSelectInst(llvm::SelectInst &SI) {
    eraseIfUnused(SI);
    if (gutils->isConstantInstruction(&SI))
      return;
    if (SI.getType()->isPointerTy())
      return;

    if (Mode == DerivativeMode::Forward)
      return;

    Value *op0 = gutils->getNewFromOriginal(SI.getOperand(0));
    Value *orig_op1 = SI.getOperand(1);
    Value *op1 = gutils->getNewFromOriginal(orig_op1);
    Value *orig_op2 = SI.getOperand(2);
    Value *op2 = gutils->getNewFromOriginal(orig_op2);

    // TODO fix all the reverse builders
    IRBuilder<> Builder2(SI.getParent());
    getReverseBuilder(Builder2);

    Value *dif1 = nullptr;
    Value *dif2 = nullptr;

    if (!gutils->isConstantValue(orig_op1))
      dif1 = Builder2.CreateSelect(lookup(op0, Builder2), diffe(&SI, Builder2),
                                   Constant::getNullValue(op1->getType()),
                                   "diffe" + op1->getName());
    if (!gutils->isConstantValue(orig_op2))
      dif2 = Builder2.CreateSelect(
          lookup(op0, Builder2), Constant::getNullValue(op2->getType()),
          diffe(&SI, Builder2), "diffe" + op2->getName());

    size_t size = 1;
    if (orig_op1->getType()->isSized())
      size = (gutils->newFunc->getParent()->getDataLayout().getTypeSizeInBits(
                  orig_op1->getType()) +
              7) /
             8;

    setDiffe(&SI, Constant::getNullValue(SI.getType()), Builder2);
    if (dif1)
      addToDiffe(orig_op1, dif1, Builder2,
                 TR.intType(size, orig_op1, false).isFloat());
    if (dif2)
      addToDiffe(orig_op2, dif2, Builder2,
                 TR.intType(size, orig_op2, false).isFloat());
  }

  void visitExtractElementInst(llvm::ExtractElementInst &EEI) {
    eraseIfUnused(EEI);
    if (gutils->isConstantInstruction(&EEI))
      return;
    if (Mode == DerivativeMode::Forward)
      return;

    IRBuilder<> Builder2(EEI.getParent());
    getReverseBuilder(Builder2);

    Value *orig_vec = EEI.getVectorOperand();

    if (!gutils->isConstantValue(orig_vec)) {
      SmallVector<Value *, 4> sv;
      sv.push_back(gutils->getNewFromOriginal(EEI.getIndexOperand()));
      ((DiffeGradientUtils *)gutils)
          ->addToDiffeIndexed(orig_vec, diffe(&EEI, Builder2), sv, Builder2);
    }
    setDiffe(&EEI, Constant::getNullValue(EEI.getType()), Builder2);
  }

  void visitInsertElementInst(llvm::InsertElementInst &IEI) {
    eraseIfUnused(IEI);
    if (gutils->isConstantInstruction(&IEI))
      return;
    if (Mode == DerivativeMode::Forward)
      return;

    IRBuilder<> Builder2(IEI.getParent());
    getReverseBuilder(Builder2);

    Value *dif1 = diffe(&IEI, Builder2);

    Value *orig_op0 = IEI.getOperand(0);
    Value *orig_op1 = IEI.getOperand(1);
    Value *op1 = gutils->getNewFromOriginal(orig_op1);
    Value *op2 = gutils->getNewFromOriginal(IEI.getOperand(2));

    size_t size0 = 1;
    if (orig_op0->getType()->isSized())
      size0 = (gutils->newFunc->getParent()->getDataLayout().getTypeSizeInBits(
                   orig_op0->getType()) +
               7) /
              8;
    size_t size1 = 1;
    if (orig_op1->getType()->isSized())
      size1 = (gutils->newFunc->getParent()->getDataLayout().getTypeSizeInBits(
                   orig_op1->getType()) +
               7) /
              8;

    if (!gutils->isConstantValue(orig_op0))
      addToDiffe(orig_op0,
                 Builder2.CreateInsertElement(
                     dif1, Constant::getNullValue(op1->getType()),
                     lookup(op2, Builder2)),
                 Builder2, TR.intType(size0, orig_op0, false).isFloat());

    if (!gutils->isConstantValue(orig_op1))
      addToDiffe(orig_op1,
                 Builder2.CreateExtractElement(dif1, lookup(op2, Builder2)),
                 Builder2, TR.intType(size1, orig_op1, false).isFloat());

    setDiffe(&IEI, Constant::getNullValue(IEI.getType()), Builder2);
  }

  void visitShuffleVectorInst(llvm::ShuffleVectorInst &SVI) {
    eraseIfUnused(SVI);
    if (gutils->isConstantInstruction(&SVI))
      return;
    if (Mode == DerivativeMode::Forward)
      return;

    IRBuilder<> Builder2(SVI.getParent());
    getReverseBuilder(Builder2);

    auto loaded = diffe(&SVI, Builder2);
    size_t l1 =
        cast<VectorType>(SVI.getOperand(0)->getType())->getNumElements();
    uint64_t instidx = 0;

    for (size_t idx : SVI.getShuffleMask()) {
      auto opnum = (idx < l1) ? 0 : 1;
      auto opidx = (idx < l1) ? idx : (idx - l1);
      SmallVector<Value *, 4> sv;
      sv.push_back(ConstantInt::get(Type::getInt32Ty(SVI.getContext()), opidx));
      if (!gutils->isConstantValue(SVI.getOperand(opnum)))
        ((DiffeGradientUtils *)gutils)
            ->addToDiffeIndexed(SVI.getOperand(opnum),
                                Builder2.CreateExtractElement(loaded, instidx),
                                sv, Builder2);
      ++instidx;
    }
    setDiffe(&SVI, Constant::getNullValue(SVI.getType()), Builder2);
  }

  void visitExtractValueInst(llvm::ExtractValueInst &EVI) {
    eraseIfUnused(EVI);
    if (gutils->isConstantInstruction(&EVI))
      return;
    if (EVI.getType()->isPointerTy())
      return;

    if (Mode == DerivativeMode::Forward)
      return;

    Value *orig_op0 = EVI.getOperand(0);

    IRBuilder<> Builder2(EVI.getParent());
    getReverseBuilder(Builder2);

    auto prediff = diffe(&EVI, Builder2);

    // todo const
    if (!gutils->isConstantValue(orig_op0)) {
      SmallVector<Value *, 4> sv;
      for (auto i : EVI.getIndices())
        sv.push_back(ConstantInt::get(Type::getInt32Ty(EVI.getContext()), i));
      ((DiffeGradientUtils *)gutils)
          ->addToDiffeIndexed(orig_op0, prediff, sv, Builder2);
    }

    setDiffe(&EVI, Constant::getNullValue(EVI.getType()), Builder2);
  }

  void visitInsertValueInst(llvm::InsertValueInst &IVI) {
    eraseIfUnused(IVI);
    if (gutils->isConstantValue(&IVI))
      return;

    if (Mode == DerivativeMode::Forward)
      return;

    auto st = cast<StructType>(IVI.getType());
    bool hasNonPointer = false;
    for (unsigned i = 0; i < st->getNumElements(); ++i) {
      if (!st->getElementType(i)->isPointerTy()) {
        hasNonPointer = true;
      }
    }
    if (!hasNonPointer)
      return;

    bool floatingInsertion = false;
    for (InsertValueInst *iv = &IVI;;) {
      size_t size0 = 1;
      if (iv->getInsertedValueOperand()->getType()->isSized() &&
          (iv->getInsertedValueOperand()->getType()->isIntOrIntVectorTy() ||
           iv->getInsertedValueOperand()->getType()->isFPOrFPVectorTy()))
        size0 =
            (gutils->newFunc->getParent()->getDataLayout().getTypeSizeInBits(
                 iv->getInsertedValueOperand()->getType()) +
             7) /
            8;
      auto it = TR.intType(size0, iv->getInsertedValueOperand(), false);
      if (it.isFloat() || !it.isKnown()) {
        floatingInsertion = true;
        break;
      }
      Value *val = iv->getAggregateOperand();
      if (gutils->isConstantValue(val))
        break;
      if (auto dc = dyn_cast<InsertValueInst>(val)) {
        iv = dc;
      } else {
        // unsure where this came from, conservatively assume contains float
        floatingInsertion = true;
        break;
      }
    }

    if (!floatingInsertion)
      return;

    // TODO handle pointers
    // TODO type analysis handle structs

    IRBuilder<> Builder2(IVI.getParent());
    getReverseBuilder(Builder2);

    Value *orig_inserted = IVI.getInsertedValueOperand();
    Value *orig_agg = IVI.getAggregateOperand();

    size_t size0 = 1;
    if (orig_inserted->getType()->isSized())
      size0 = (gutils->newFunc->getParent()->getDataLayout().getTypeSizeInBits(
                   orig_inserted->getType()) +
               7) /
              8;

    Type *flt = nullptr;
    if (!gutils->isConstantValue(orig_inserted) &&
        (flt = TR.intType(size0, orig_inserted).isFloat())) {
      auto prediff = diffe(&IVI, Builder2);
      auto dindex = Builder2.CreateExtractValue(prediff, IVI.getIndices());
      addToDiffe(orig_inserted, dindex, Builder2, flt);
    }

    size_t size1 = 1;
    if (orig_agg->getType()->isSized() &&
        (orig_agg->getType()->isIntOrIntVectorTy() ||
         orig_agg->getType()->isFPOrFPVectorTy()))
      size1 = (gutils->newFunc->getParent()->getDataLayout().getTypeSizeInBits(
                   orig_agg->getType()) +
               7) /
              8;

    if (!gutils->isConstantValue(orig_agg)) {
      auto prediff = diffe(&IVI, Builder2);
      auto dindex = Builder2.CreateInsertValue(
          prediff, Constant::getNullValue(orig_inserted->getType()),
          IVI.getIndices());
      llvm::errs() << "orig:" << IVI
                   << " query(orig_agg):" << TR.query(orig_agg).str() << "\n";
      addToDiffe(orig_agg, dindex, Builder2,
                 TR.intType(size1, orig_agg, false).isFloat());
    }

    setDiffe(&IVI, Constant::getNullValue(IVI.getType()), Builder2);
  }

  inline void getReverseBuilder(IRBuilder<> &Builder2) {
    BasicBlock *BB =
        cast<BasicBlock>(gutils->getNewFromOriginal(Builder2.GetInsertBlock()));
    BasicBlock *BB2 = gutils->reverseBlocks[BB];
    if (!BB2) {
      llvm::errs() << "oldFunc: " << *gutils->oldFunc << "\n";
      llvm::errs() << "newFunc: " << *gutils->newFunc << "\n";
      llvm::errs() << "could not invert " << *BB;
    }
    assert(BB2);

    Builder2.SetInsertPoint(BB2);
    Builder2.SetCurrentDebugLocation(
        gutils->getNewFromOriginal(Builder2.getCurrentDebugLocation()));
    Builder2.setFastMathFlags(getFast());
  }

  Value *diffe(Value *val, IRBuilder<> &Builder) {
    assert(Mode == DerivativeMode::Reverse || Mode == DerivativeMode::Both);
    return ((DiffeGradientUtils *)gutils)->diffe(val, Builder);
  }

  void setDiffe(Value *val, Value *dif, IRBuilder<> &Builder) {
    assert(Mode == DerivativeMode::Reverse || Mode == DerivativeMode::Both);
    ((DiffeGradientUtils *)gutils)->setDiffe(val, dif, Builder);
  }

  StoreInst *setPtrDiffe(Value *val, Value *dif, IRBuilder<> &Builder) {
    return gutils->setPtrDiffe(val, dif, Builder);
  }

  std::vector<SelectInst *> addToDiffe(Value *val, Value *dif,
                                       IRBuilder<> &Builder, Type *T) {
    assert(Mode == DerivativeMode::Reverse || Mode == DerivativeMode::Both);
    return ((DiffeGradientUtils *)gutils)->addToDiffe(val, dif, Builder, T);
  }

  Value *lookup(Value *val, IRBuilder<> &Builder) {
    return gutils->lookupM(val, Builder);
  }

  void visitBinaryOperator(llvm::BinaryOperator &BO) {
    eraseIfUnused(BO);
    if (gutils->isConstantInstruction(&BO))
      return;
    if (Mode != DerivativeMode::Reverse && Mode != DerivativeMode::Both)
      return;

    Value *orig_op0 = BO.getOperand(0);
    Value *orig_op1 = BO.getOperand(1);
    bool constantval0 = gutils->isConstantValue(orig_op0);
    bool constantval1 = gutils->isConstantValue(orig_op1);

    size_t size = 1;
    if (BO.getType()->isSized())
      size = (gutils->newFunc->getParent()->getDataLayout().getTypeSizeInBits(
                  BO.getType()) +
              7) /
             8;

    if (BO.getType()->isIntOrIntVectorTy() &&
        TR.intType(size, &BO, /*errifnotfound*/ false) == BaseType::Pointer) {
      return;
    }

    IRBuilder<> Builder2(BO.getParent());
    getReverseBuilder(Builder2);

    Value *dif0 = nullptr;
    Value *dif1 = nullptr;
    Value *idiff = diffe(&BO, Builder2);

    Type *addingType = BO.getType();

    switch (BO.getOpcode()) {
    case Instruction::FMul: {
      if (!constantval0)
        dif0 = Builder2.CreateFMul(
            idiff, lookup(gutils->getNewFromOriginal(orig_op1), Builder2),
            "m0diffe" + orig_op0->getName());
      if (!constantval1)
        dif1 = Builder2.CreateFMul(
            idiff, lookup(gutils->getNewFromOriginal(orig_op0), Builder2),
            "m1diffe" + orig_op1->getName());
      break;
    }
    case Instruction::FAdd: {
      if (!constantval0)
        dif0 = idiff;
      if (!constantval1)
        dif1 = idiff;
      break;
    }
    case Instruction::FSub: {
      if (!constantval0)
        dif0 = idiff;
      if (!constantval1)
        dif1 = Builder2.CreateFNeg(idiff);
      break;
    }
    case Instruction::FDiv: {
      if (!constantval0)
        dif0 = Builder2.CreateFDiv(
            idiff, lookup(gutils->getNewFromOriginal(orig_op1), Builder2),
            "d0diffe" + orig_op0->getName());
      if (!constantval1) {
        Value *lop0 = lookup(gutils->getNewFromOriginal(orig_op0), Builder2);
        Value *lop1 = lookup(gutils->getNewFromOriginal(orig_op1), Builder2);
        Value *lastdiv = Builder2.CreateFDiv(lop0, lop1);
        if (auto newi = dyn_cast<Instruction>(lastdiv))
          newi->copyIRFlags(&BO);

        dif1 = Builder2.CreateFNeg(
            Builder2.CreateFMul(lastdiv, Builder2.CreateFDiv(idiff, lop1)));
      }
      break;
    }
    case Instruction::LShr: {
      if (!constantval0) {
        if (auto ci = dyn_cast<ConstantInt>(orig_op1)) {
          size_t size = 1;
          if (orig_op0->getType()->isSized())
            size = (gutils->newFunc->getParent()
                        ->getDataLayout()
                        .getTypeSizeInBits(orig_op0->getType()) +
                    7) /
                   8;

          if (Type *flt =
                  TR.intType(size, orig_op0, /*necessary*/ false).isFloat()) {
            auto bits = gutils->newFunc->getParent()
                            ->getDataLayout()
                            .getTypeAllocSizeInBits(flt);
            if (ci->getSExtValue() >= (int64_t)bits &&
                ci->getSExtValue() % bits == 0) {
              dif0 = Builder2.CreateShl(idiff, ci);
              addingType = flt;
              goto done;
            }
          }
        }
      }
      goto def;
    }
    case Instruction::Add: {
      if (looseTypeAnalysis) {
        // if loose type analysis, assume this integer add is constant
        return;
      }
      goto def;
    }
    default:
    def:;
      llvm::errs() << *gutils->oldFunc << "\n";
      for (auto &pair : gutils->internal_isConstantInstruction) {
        llvm::errs()
            << " constantinst[" << *pair.first << "] = " << pair.second
            << " val:"
            << gutils->internal_isConstantValue[const_cast<Instruction *>(
                   pair.first)]
            << " type: "
            << TR.query(const_cast<Instruction *>(pair.first)).str() << "\n";
      }
      llvm::errs() << "cannot handle unknown binary operator: " << BO << "\n";
      report_fatal_error("unknown binary operator");
    }

  done:;
    if (dif0 || dif1)
      setDiffe(&BO, Constant::getNullValue(BO.getType()), Builder2);
    if (dif0)
      addToDiffe(orig_op0, dif0, Builder2, addingType);
    if (dif1)
      addToDiffe(orig_op1, dif1, Builder2, addingType);
  }

  void visitMemSetInst(llvm::MemSetInst &MS) {
    // Don't duplicate set in reverse pass
    if (Mode == DerivativeMode::Reverse) {
      erased.insert(&MS);
      gutils->erase(gutils->getNewFromOriginal(&MS));
    }

    if (gutils->isConstantInstruction(&MS))
      return;

    Value *orig_op0 = MS.getOperand(0);
    Value *orig_op1 = MS.getOperand(1);
    Value *op1 = gutils->getNewFromOriginal(orig_op1);
    Value *op2 = gutils->getNewFromOriginal(MS.getOperand(2));
    Value *op3 = gutils->getNewFromOriginal(MS.getOperand(3));

    // TODO this should 1) assert that the value being meset is constant
    //                 2) duplicate the memset for the inverted pointer

    if (!gutils->isConstantValue(orig_op1)) {
      llvm::errs() << "couldn't handle non constant inst in memset to "
                      "propagate differential to\n"
                   << MS;
      report_fatal_error("non constant in memset");
    }

    if (Mode == DerivativeMode::Forward || Mode == DerivativeMode::Both) {
      IRBuilder<> BuilderZ(gutils->getNewFromOriginal(&MS));

      SmallVector<Value *, 4> args;
      if (!gutils->isConstantValue(orig_op0)) {
        args.push_back(gutils->invertPointerM(orig_op0, BuilderZ));
      } else {
        // If constant destination then no operation needs doing
        return;
        // args.push_back(gutils->lookupM(MS.getOperand(0), BuilderZ));
      }

      args.push_back(gutils->lookupM(op1, BuilderZ));
      args.push_back(gutils->lookupM(op2, BuilderZ));
      args.push_back(gutils->lookupM(op3, BuilderZ));

      Type *tys[] = {args[0]->getType(), args[2]->getType()};
      auto cal = BuilderZ.CreateCall(
          Intrinsic::getDeclaration(MS.getParent()->getParent()->getParent(),
                                    Intrinsic::memset, tys),
          args);
      cal->setAttributes(MS.getAttributes());
      cal->setCallingConv(MS.getCallingConv());
      cal->setTailCallKind(MS.getTailCallKind());
    }

    if (Mode == DerivativeMode::Reverse || Mode == DerivativeMode::Both) {
      // TODO consider what reverse pass memset should be
    }
  }

  void subTransferHelper(Type *secretty, BasicBlock *parent,
                         Intrinsic::ID intrinsic, unsigned dstalign,
                         unsigned srcalign, unsigned offset, Value *orig_dst,
                         Value *orig_src, Value *length, Value *isVolatile,
                         llvm::MemTransferInst &MTI) {
    // TODO offset
    if (secretty) {
      // no change to forward pass if represents floats
      if (Mode == DerivativeMode::Reverse || Mode == DerivativeMode::Both) {
        IRBuilder<> Builder2(parent);
        getReverseBuilder(Builder2);

        // If the src is context simply zero d_dst and don't propagate to d_src
        // (which thus == src and may be illegal)
        if (gutils->isConstantValue(orig_src)) {
          SmallVector<Value *, 4> args;
          args.push_back(gutils->invertPointerM(orig_dst, Builder2));
          args.push_back(
              ConstantInt::get(Type::getInt8Ty(parent->getContext()), 0));
          args.push_back(lookup(length, Builder2));
#if LLVM_VERSION_MAJOR <= 6
          args.push_back(ConstantInt::get(
              Type::getInt32Ty(parent->getContext()), max(1U, dstalign)));
#endif
          args.push_back(ConstantInt::getFalse(parent->getContext()));

          Type *tys[] = {args[0]->getType(), args[2]->getType()};
          auto memsetIntr = Intrinsic::getDeclaration(
              parent->getParent()->getParent(), Intrinsic::memset, tys);
          auto cal = Builder2.CreateCall(memsetIntr, args);
          cal->setCallingConv(memsetIntr->getCallingConv());
          if (dstalign != 0) {
#if LLVM_VERSION_MAJOR >= 10
            cal->addParamAttr(0, Attribute::getWithAlignment(
                                     parent->getContext(), Align(dstalign)));
#else
            cal->addParamAttr(
                0, Attribute::getWithAlignment(parent->getContext(), dstalign));
#endif
          }

        } else {
          SmallVector<Value *, 4> args;
          auto secretpt = PointerType::getUnqual(secretty);
          auto dsto = gutils->invertPointerM(orig_dst, Builder2);
          if (offset != 0)
            dsto = Builder2.CreateConstInBoundsGEP1_64(dsto, offset);
          args.push_back(Builder2.CreatePointerCast(dsto, secretpt));
          auto srco = gutils->invertPointerM(orig_src, Builder2);
          if (offset != 0)
            srco = Builder2.CreateConstInBoundsGEP1_64(srco, offset);
          args.push_back(Builder2.CreatePointerCast(srco, secretpt));
          args.push_back(Builder2.CreateUDiv(
              lookup(length, Builder2),

              ConstantInt::get(length->getType(),
                               Builder2.GetInsertBlock()
                                       ->getParent()
                                       ->getParent()
                                       ->getDataLayout()
                                       .getTypeAllocSizeInBits(secretty) /
                                   8)));

          auto dmemcpy = ((intrinsic == Intrinsic::memcpy)
                              ? getOrInsertDifferentialFloatMemcpy
                              : getOrInsertDifferentialFloatMemmove)(
              *parent->getParent()->getParent(), secretpt, dstalign, srcalign);
          Builder2.CreateCall(dmemcpy, args);
        }
      }
    } else {

      // if represents pointer or integer type then only need to modify forward
      // pass with the copy
      if (Mode == DerivativeMode::Forward || Mode == DerivativeMode::Both) {

        // It is questionable how the following case would even occur, but if
        // the dst is constant, we shouldn't do anything extra
        if (gutils->isConstantValue(orig_dst)) {
          return;
        }

        SmallVector<Value *, 4> args;
        IRBuilder<> BuilderZ(gutils->getNewFromOriginal(&MTI));

        // If src is inactive, then we should copy from the regular pointer
        // (i.e. suppose we are copying constant memory representing dimensions
        // into a tensor)
        //  to ensure that the differential tensor is well formed for use
        //  OUTSIDE the derivative generation (as enzyme doesn't need this), we
        //  should also perform the copy onto the differential. Future
        //  Optimization (not implemented): If dst can never escape Enzyme code,
        //  we may omit this copy.
        // no need to update pointers, even if dst is active
        auto dsto = gutils->invertPointerM(orig_dst, BuilderZ);
        if (offset != 0)
          dsto = BuilderZ.CreateConstInBoundsGEP1_64(dsto, offset);
        args.push_back(dsto);
        auto srco = gutils->invertPointerM(orig_src, BuilderZ);
        if (offset != 0)
          srco = BuilderZ.CreateConstInBoundsGEP1_64(srco, offset);
        args.push_back(srco);

        args.push_back(length);
#if LLVM_VERSION_MAJOR <= 6
        args.push_back(ConstantInt::get(Type::getInt32Ty(parent->getContext()),
                                        max(1U, min(srcalign, dstalign))));
#endif
        args.push_back(isVolatile);

        //#if LLVM_VERSION_MAJOR >= 7
        Type *tys[] = {args[0]->getType(), args[1]->getType(),
                       args[2]->getType()};
        //#else
        // Type *tys[] = {args[0]->getType(), args[1]->getType(),
        // args[2]->getType(), args[3]->getType()}; #endif

        auto memtransIntr = Intrinsic::getDeclaration(
            gutils->newFunc->getParent(), intrinsic, tys);
        auto cal = BuilderZ.CreateCall(memtransIntr, args);
        cal->setAttributes(MTI.getAttributes());
        cal->setCallingConv(memtransIntr->getCallingConv());
        cal->setTailCallKind(MTI.getTailCallKind());

        if (dstalign != 0) {
#if LLVM_VERSION_MAJOR >= 10
          cal->addParamAttr(0, Attribute::getWithAlignment(parent->getContext(),
                                                           Align(dstalign)));
#else
          cal->addParamAttr(
              0, Attribute::getWithAlignment(parent->getContext(), dstalign));
#endif
        }
        if (srcalign != 0) {
#if LLVM_VERSION_MAJOR >= 10
          cal->addParamAttr(1, Attribute::getWithAlignment(parent->getContext(),
                                                           Align(srcalign)));
#else
          cal->addParamAttr(
              1, Attribute::getWithAlignment(parent->getContext(), srcalign));
#endif
        }
      }
    }
  }

  void visitMemTransferInst(llvm::MemTransferInst &MTI) {
    if (gutils->isConstantValue(MTI.getOperand(0))) {
      eraseIfUnused(MTI);
      return;
    }

    if (unnecessaryStores.count(&MTI)) {
      eraseIfUnused(MTI);
      return;
    }

    Value *orig_op0 = MTI.getOperand(0);
    Value *orig_op1 = MTI.getOperand(1);
    Value *op2 = gutils->getNewFromOriginal(MTI.getOperand(2));
#if LLVM_VERSION_MAJOR >= 7
    Value *isVolatile = gutils->getNewFromOriginal(MTI.getOperand(3));
#else
    Value *isVolatile = gutils->getNewFromOriginal(MTI.getOperand(4));
#endif

    // copying into nullptr is invalid (not sure why it exists here), but we
    // shouldn't do it in reverse pass or shadow
    if (isa<ConstantPointerNull>(orig_op0) ||
        TR.query(orig_op0).Inner0() == BaseType::Anything) {
      eraseIfUnused(MTI);
      return;
    }

    size_t size = 1;
    if (auto ci = dyn_cast<ConstantInt>(op2)) {
      size = ci->getLimitedValue();
    }

    // TODO note that we only handle memcpy/etc of ONE type (aka memcpy of {int,
    // double} not allowed)

    // llvm::errs() << *gutils->oldFunc << "\n";
    // TR.dump();

    auto vd = TR.query(orig_op0).Data0().AtMost(size);
    vd |= TR.query(orig_op1).Data0().AtMost(size);

    // llvm::errs() << "MIT: " << MTI << "|size: " << size << " vd: " <<
    // vd.str() << "\n";

    if (!vd.isKnownPastPointer()) {
      if (looseTypeAnalysis) {
        if (isa<CastInst>(orig_op0) &&
            cast<CastInst>(orig_op0)->getSrcTy()->isPointerTy() &&
            cast<PointerType>(cast<CastInst>(orig_op0)->getSrcTy())
                ->getElementType()
                ->isFPOrFPVectorTy()) {
          vd = TypeTree(ConcreteType(cast<PointerType>(
                                         cast<CastInst>(orig_op0)->getSrcTy())
                                         ->getElementType()
                                         ->getScalarType()))
                   .Only(0);
          goto known;
        }
      }
      EmitFailure("CannotDeduceType", MTI.getDebugLoc(), &MTI,
                  "failed to deduce type of copy ", MTI);

      TR.firstPointer(size, orig_op0, /*errifnotfound*/ true,
                      /*pointerIntSame*/ true);
      llvm_unreachable("bad mti");
    }
  known:;

    unsigned dstalign = 0;
    if (MTI.paramHasAttr(0, Attribute::Alignment)) {
      dstalign = MTI.getParamAttr(0, Attribute::Alignment).getValueAsInt();
    }
    unsigned srcalign = 0;
    if (MTI.paramHasAttr(1, Attribute::Alignment)) {
      srcalign = MTI.getParamAttr(1, Attribute::Alignment).getValueAsInt();
    }

    unsigned start = 0;

    IRBuilder<> BuilderZ(gutils->getNewFromOriginal(&MTI));

    while (1) {
      unsigned nextStart = size;

      auto dt = vd[{-1}];
      for (size_t i = start; i < size; ++i) {
        bool Legal = true;
        dt.checkedOrIn(vd[{(int)i}], /*PointerIntSame*/ true, Legal);
        if (!Legal) {
          nextStart = i;
          break;
        }
      }
      if (!dt.isKnown()) {
        TR.dump();
        llvm::errs() << " vd:" << vd.str() << " start:" << start
                     << " size: " << size << " dt:" << dt.str() << "\n";
      }
      assert(dt.isKnown());

      Value *length = op2;
      if (nextStart != size) {
        length = ConstantInt::get(op2->getType(), nextStart);
      }
      if (start != 0)
        length =
            BuilderZ.CreateSub(length, ConstantInt::get(op2->getType(), start));

      unsigned subdstalign = dstalign;
      // todo make better alignment calculation
      if (dstalign != 0) {
        if (start % dstalign != 0) {
          dstalign = 1;
        }
      }
      unsigned subsrcalign = srcalign;
      // todo make better alignment calculation
      if (srcalign != 0) {
        if (start % srcalign != 0) {
          srcalign = 1;
        }
      }
      subTransferHelper(dt.isFloat(), MTI.getParent(), MTI.getIntrinsicID(),
                        subdstalign, subsrcalign, /*offset*/ start, orig_op0,
                        orig_op1, /*length*/ length, /*volatile*/ isVolatile,
                        MTI);

      if (nextStart == size)
        break;
      start = nextStart;
    }

    eraseIfUnused(MTI);
  }

  void visitIntrinsicInst(llvm::IntrinsicInst &II) {
    if (II.getIntrinsicID() == Intrinsic::stacksave) {
      eraseIfUnused(II, /*erase*/ true, /*check*/ false);
      return;
    }
    if (II.getIntrinsicID() == Intrinsic::stackrestore ||
        II.getIntrinsicID() == Intrinsic::lifetime_end) {
      eraseIfUnused(II, /*erase*/ true, /*check*/ false);
      return;
    }

    eraseIfUnused(II);
    Value *orig_ops[II.getNumOperands()];

    for (unsigned i = 0; i < II.getNumOperands(); ++i) {
      orig_ops[i] = II.getOperand(i);
    }

    if (Mode == DerivativeMode::Forward) {
      switch (II.getIntrinsicID()) {
      case Intrinsic::prefetch:
      case Intrinsic::dbg_declare:
      case Intrinsic::dbg_value:
#if LLVM_VERSION_MAJOR > 6
      case Intrinsic::dbg_label:
#endif
      case Intrinsic::dbg_addr:
      case Intrinsic::lifetime_start:
      case Intrinsic::assume:
      case Intrinsic::fabs:
#if LLVM_VERSION_MAJOR < 10
      case Intrinsic::x86_sse_max_ss:
      case Intrinsic::x86_sse_max_ps:
      case Intrinsic::x86_sse_min_ss:
      case Intrinsic::x86_sse_min_ps:
#endif
      case Intrinsic::maxnum:
      case Intrinsic::minnum:
      case Intrinsic::log:
      case Intrinsic::log2:
      case Intrinsic::log10:
      case Intrinsic::exp:
      case Intrinsic::exp2:
      case Intrinsic::pow:
      case Intrinsic::powi:
#if LLVM_VERSION_MAJOR >= 9
      case Intrinsic::experimental_vector_reduce_v2_fadd:
#endif
      case Intrinsic::sin:
      case Intrinsic::cos:
      case Intrinsic::floor:
      case Intrinsic::ceil:
      case Intrinsic::trunc:
      case Intrinsic::rint:
      case Intrinsic::nearbyint:
      case Intrinsic::round:
      case Intrinsic::sqrt:
      case Intrinsic::fma:
        return;
      default:
        if (gutils->isConstantInstruction(&II))
          return;
        llvm::errs() << *gutils->oldFunc << "\n";
        llvm::errs() << *gutils->newFunc << "\n";
        llvm::errs() << "cannot handle (augmented) unknown intrinsic\n" << II;
        report_fatal_error("(augmented) unknown intrinsic");
      }
    }

    if (Mode == DerivativeMode::Both || Mode == DerivativeMode::Reverse) {

      IRBuilder<> Builder2(II.getParent());
      getReverseBuilder(Builder2);
      Module *M = II.getParent()->getParent()->getParent();

      Value *vdiff = nullptr;
      if (!gutils->isConstantValue(&II)) {
        vdiff = diffe(&II, Builder2);
        setDiffe(&II, Constant::getNullValue(II.getType()), Builder2);
      }

      switch (II.getIntrinsicID()) {
      case Intrinsic::assume:
      case Intrinsic::prefetch:
      case Intrinsic::dbg_declare:
      case Intrinsic::dbg_value:
#if LLVM_VERSION_MAJOR > 6
      case Intrinsic::dbg_label:
#endif
      case Intrinsic::dbg_addr:
      case Intrinsic::floor:
      case Intrinsic::ceil:
      case Intrinsic::trunc:
      case Intrinsic::rint:
      case Intrinsic::nearbyint:
      case Intrinsic::round:
        // Derivative of these is zero and requires no modification
        return;

#if LLVM_VERSION_MAJOR >= 9
      case Intrinsic::experimental_vector_reduce_v2_fadd: {
        if (gutils->isConstantInstruction(&II))
          return;

        if (!gutils->isConstantValue(orig_ops[0])) {
          addToDiffe(orig_ops[0], vdiff, Builder2, orig_ops[0]->getType());
        }
        if (!gutils->isConstantValue(orig_ops[1])) {
          auto und = UndefValue::get(orig_ops[1]->getType());
          auto mask = ConstantAggregateZero::get(
              VectorType::get(Type::getInt32Ty(und->getContext()),
                              cast<VectorType>(und->getType())->getNumElements()
#if LLVM_VERSION_MAJOR >= 11
                                  ,
                              false));
#else
                                  ));
#endif
          auto vec = Builder2.CreateShuffleVector(
              Builder2.CreateInsertElement(und, vdiff, (uint64_t)0), und, mask);
          addToDiffe(orig_ops[1], vec, Builder2, orig_ops[0]->getType());
        }
        return;
      }
#endif

      case Intrinsic::lifetime_start: {
        if (gutils->isConstantInstruction(&II))
          return;
        SmallVector<Value *, 2> args = {
            lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2),
            lookup(gutils->getNewFromOriginal(orig_ops[1]), Builder2)};
        Type *tys[] = {args[1]->getType()};
        auto cal = Builder2.CreateCall(
            Intrinsic::getDeclaration(M, Intrinsic::lifetime_end, tys), args);
        cal->setAttributes(II.getAttributes());
        cal->setCallingConv(II.getCallingConv());
        cal->setTailCallKind(II.getTailCallKind());
        return;
      }

      case Intrinsic::sqrt: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {
          SmallVector<Value *, 2> args = {
              lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2)};
          Type *tys[] = {orig_ops[0]->getType()};
          auto cal = cast<CallInst>(Builder2.CreateCall(
              Intrinsic::getDeclaration(M, II.getIntrinsicID(), tys), args));
          cal->copyIRFlags(&II);
          cal->setAttributes(II.getAttributes());
          cal->setCallingConv(II.getCallingConv());
          cal->setTailCallKind(II.getTailCallKind());
          cal->setDebugLoc(gutils->getNewFromOriginal(II.getDebugLoc()));

          Value *dif0 = Builder2.CreateBinOp(
              Instruction::FDiv,
              Builder2.CreateFMul(ConstantFP::get(II.getType(), 0.5), vdiff),
              cal);

          Value *cmp = Builder2.CreateFCmpOEQ(
              args[0], ConstantFP::get(orig_ops[0]->getType(), 0));
          dif0 = Builder2.CreateSelect(
              cmp, ConstantFP::get(orig_ops[0]->getType(), 0), dif0);

          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }
        return;
      }

      case Intrinsic::fabs: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {
          Value *cmp = Builder2.CreateFCmpOLT(
              lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2),
              ConstantFP::get(orig_ops[0]->getType(), 0));
          Value *dif0 = Builder2.CreateFMul(
              Builder2.CreateSelect(cmp,
                                    ConstantFP::get(orig_ops[0]->getType(), -1),
                                    ConstantFP::get(orig_ops[0]->getType(), 1)),
              vdiff);
          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }
        return;
      }

#if LLVM_VERSION_MAJOR < 10
      case Intrinsic::x86_sse_max_ss:
      case Intrinsic::x86_sse_max_ps:
#endif
      case Intrinsic::maxnum: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {
          Value *cmp = Builder2.CreateFCmpOLT(
              lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2),
              lookup(gutils->getNewFromOriginal(orig_ops[1]), Builder2));
          Value *dif0 = Builder2.CreateSelect(
              cmp, ConstantFP::get(orig_ops[0]->getType(), 0), vdiff);
          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }
        if (vdiff && !gutils->isConstantValue(orig_ops[1])) {
          Value *cmp = Builder2.CreateFCmpOLT(
              lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2),
              lookup(gutils->getNewFromOriginal(orig_ops[1]), Builder2));
          Value *dif1 = Builder2.CreateSelect(
              cmp, vdiff, ConstantFP::get(orig_ops[0]->getType(), 0));
          addToDiffe(orig_ops[1], dif1, Builder2, II.getType());
        }
        return;
      }

#if LLVM_VERSION_MAJOR < 10
      case Intrinsic::x86_sse_min_ss:
      case Intrinsic::x86_sse_min_ps:
#endif
      case Intrinsic::minnum: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {
          Value *cmp = Builder2.CreateFCmpOLT(
              lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2),
              lookup(gutils->getNewFromOriginal(orig_ops[1]), Builder2));
          Value *dif0 = Builder2.CreateSelect(
              cmp, vdiff, ConstantFP::get(orig_ops[0]->getType(), 0));
          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }
        if (vdiff && !gutils->isConstantValue(orig_ops[1])) {
          Value *cmp = Builder2.CreateFCmpOLT(
              lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2),
              lookup(gutils->getNewFromOriginal(orig_ops[1]), Builder2));
          Value *dif1 = Builder2.CreateSelect(
              cmp, ConstantFP::get(orig_ops[0]->getType(), 0), vdiff);
          addToDiffe(orig_ops[1], dif1, Builder2, II.getType());
        }
        return;
      }

      case Intrinsic::fma: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {
          Value *dif0 = Builder2.CreateFMul(
              vdiff, lookup(gutils->getNewFromOriginal(orig_ops[1]), Builder2));
          addToDiffe(orig_ops[0], dif0, Builder2,
                     II.getType()->getScalarType());
        }
        if (vdiff && !gutils->isConstantValue(orig_ops[1])) {
          Value *dif1 = Builder2.CreateFMul(
              vdiff, lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2));
          addToDiffe(orig_ops[1], dif1, Builder2,
                     II.getType()->getScalarType());
        }
        if (vdiff && !gutils->isConstantValue(orig_ops[2])) {
          addToDiffe(orig_ops[2], vdiff, Builder2,
                     II.getType()->getScalarType());
        }
        return;
      }

      case Intrinsic::log: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {
          Value *dif0 = Builder2.CreateFDiv(
              vdiff, lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2));
          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }
        return;
      }

      case Intrinsic::log2: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {
          Value *dif0 = Builder2.CreateFDiv(
              vdiff,
              Builder2.CreateFMul(
                  ConstantFP::get(II.getType(), 0.6931471805599453),
                  lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2)));
          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }
        return;
      }
      case Intrinsic::log10: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {
          Value *dif0 = Builder2.CreateFDiv(
              vdiff,
              Builder2.CreateFMul(
                  ConstantFP::get(II.getType(), 2.302585092994046),
                  lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2)));
          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }
        return;
      }

      case Intrinsic::exp: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {
          SmallVector<Value *, 2> args = {
              lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2)};
          Type *tys[] = {orig_ops[0]->getType()};
          auto cal = cast<CallInst>(Builder2.CreateCall(
              Intrinsic::getDeclaration(M, II.getIntrinsicID(), tys), args));
          cal->copyIRFlags(&II);
          cal->setAttributes(II.getAttributes());
          cal->setCallingConv(II.getCallingConv());
          cal->setTailCallKind(II.getTailCallKind());
          cal->setDebugLoc(gutils->getNewFromOriginal(II.getDebugLoc()));

          Value *dif0 = Builder2.CreateFMul(vdiff, lookup(cal, Builder2));
          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }
        return;
      }
      case Intrinsic::exp2: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {
          SmallVector<Value *, 2> args = {
              lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2)};
          Type *tys[] = {orig_ops[0]->getType()};
          auto cal = cast<CallInst>(Builder2.CreateCall(
              Intrinsic::getDeclaration(M, II.getIntrinsicID(), tys), args));
          cal->copyIRFlags(&II);
          cal->setAttributes(II.getAttributes());
          cal->setCallingConv(II.getCallingConv());
          cal->setTailCallKind(II.getTailCallKind());
          cal->setDebugLoc(gutils->getNewFromOriginal(II.getDebugLoc()));

          Value *dif0 = Builder2.CreateFMul(
              Builder2.CreateFMul(vdiff, lookup(cal, Builder2)),
              ConstantFP::get(II.getType(), 0.6931471805599453));
          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }
        return;
      }
      case Intrinsic::powi: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {

          Value *op0 = gutils->getNewFromOriginal(orig_ops[0]);
          Value *op1 = gutils->getNewFromOriginal(orig_ops[1]);
          SmallVector<Value *, 2> args = {
              lookup(op0, Builder2),
              Builder2.CreateSub(lookup(op1, Builder2),
                                 ConstantInt::get(op1->getType(), 1))};
          Type *tys[] = {orig_ops[0]->getType()};
          auto cal = cast<CallInst>(Builder2.CreateCall(
              Intrinsic::getDeclaration(M, Intrinsic::powi, tys), args));
          cal->copyIRFlags(&II);
          cal->setAttributes(II.getAttributes());
          cal->setCallingConv(II.getCallingConv());
          cal->setTailCallKind(II.getTailCallKind());
          cal->setDebugLoc(gutils->getNewFromOriginal(II.getDebugLoc()));
          Value *dif0 = Builder2.CreateFMul(
              Builder2.CreateFMul(vdiff, cal),
              Builder2.CreateSIToFP(lookup(op1, Builder2), op0->getType()));
          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }
        return;
      }
      case Intrinsic::pow: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {

          Value *op0 = gutils->getNewFromOriginal(orig_ops[0]);
          Value *op1 = gutils->getNewFromOriginal(orig_ops[1]);
          /*
          dif0 = Builder2.CreateFMul(
            Builder2.CreateFMul(vdiff,
              Builder2.CreateFDiv(lookup(&II), lookup(II.getOperand(0)))),
          lookup(II.getOperand(1))
          );
          */
          SmallVector<Value *, 2> args = {
              lookup(op0, Builder2),
              Builder2.CreateFSub(lookup(op1, Builder2),
                                  ConstantFP::get(II.getType(), 1.0))};
          Type *tys[] = {orig_ops[0]->getType()};
          auto cal = cast<CallInst>(Builder2.CreateCall(
              Intrinsic::getDeclaration(M, Intrinsic::pow, tys), args));
          cal->copyIRFlags(&II);
          cal->setAttributes(II.getAttributes());
          cal->setCallingConv(II.getCallingConv());
          cal->setTailCallKind(II.getTailCallKind());
          cal->setDebugLoc(gutils->getNewFromOriginal(II.getDebugLoc()));

          Value *dif0 = Builder2.CreateFMul(Builder2.CreateFMul(vdiff, cal),
                                            lookup(op1, Builder2));
          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }

        if (vdiff && !gutils->isConstantValue(orig_ops[1])) {

          CallInst *cal;
          {
            SmallVector<Value *, 2> args = {
                lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2),
                lookup(gutils->getNewFromOriginal(orig_ops[1]), Builder2)};

            Type *tys[] = {orig_ops[0]->getType()};
            cal = cast<CallInst>(Builder2.CreateCall(
                Intrinsic::getDeclaration(M, II.getIntrinsicID(), tys), args));
            cal->copyIRFlags(&II);
            cal->setAttributes(II.getAttributes());
            cal->setCallingConv(II.getCallingConv());
            cal->setTailCallKind(II.getTailCallKind());
            cal->setDebugLoc(gutils->getNewFromOriginal(II.getDebugLoc()));
          }

          Value *args[] = {
              lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2)};
          Type *tys[] = {orig_ops[0]->getType()};

          Value *dif1 = Builder2.CreateFMul(
              Builder2.CreateFMul(vdiff, cal),
              Builder2.CreateCall(
                  Intrinsic::getDeclaration(M, Intrinsic::log, tys), args));
          addToDiffe(orig_ops[1], dif1, Builder2, II.getType());
        }

        return;
      }
      case Intrinsic::sin: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {
          Value *args[] = {
              lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2)};
          Type *tys[] = {orig_ops[0]->getType()};
          CallInst *cal = cast<CallInst>(Builder2.CreateCall(
              Intrinsic::getDeclaration(M, Intrinsic::cos, tys), args));
          cal->setTailCallKind(II.getTailCallKind());
          Value *dif0 = Builder2.CreateFMul(vdiff, cal);
          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }
        return;
      }
      case Intrinsic::cos: {
        if (vdiff && !gutils->isConstantValue(orig_ops[0])) {
          Value *args[] = {
              lookup(gutils->getNewFromOriginal(orig_ops[0]), Builder2)};
          Type *tys[] = {orig_ops[0]->getType()};
          CallInst *cal = cast<CallInst>(Builder2.CreateCall(
              Intrinsic::getDeclaration(M, Intrinsic::sin, tys), args));
          cal->setTailCallKind(II.getTailCallKind());
          Value *dif0 = Builder2.CreateFMul(vdiff, Builder2.CreateFNeg(cal));
          addToDiffe(orig_ops[0], dif0, Builder2, II.getType());
        }
        return;
      }
      default:
        if (gutils->isConstantInstruction(&II))
          return;
        llvm::errs() << *gutils->oldFunc << "\n";
        llvm::errs() << *gutils->newFunc << "\n";
        llvm::errs() << "cannot handle (reverse) unknown intrinsic\n" << II;
        report_fatal_error("(reverse) unknown intrinsic");
      }
    }

    llvm::InstVisitor<
        AdjointGenerator<AugmentedReturnType>>::visitIntrinsicInst(II);
  }

  void visitOMPCall(llvm::CallInst &call) {
    Function *kmpc = call.getCalledFunction();

    if (uncacheable_args_map.find(&call) == uncacheable_args_map.end()) {
      llvm::errs() << " call: " << call << "\n";
      for (auto &pair : uncacheable_args_map) {
        llvm::errs() << " + " << *pair.first << "\n";
      }
    }

    assert(uncacheable_args_map.find(&call) != uncacheable_args_map.end());
    const std::map<Argument *, bool> &uncacheable_argsAbove =
        uncacheable_args_map.find(&call)->second;

    IRBuilder<> BuilderZ(gutils->getNewFromOriginal(&call));
    BuilderZ.setFastMathFlags(getFast());

    Function *task = dyn_cast<Function>(call.getArgOperand(2));
    if (task == nullptr && isa<ConstantExpr>(call.getArgOperand(2))) {
      task = dyn_cast<Function>(
          cast<ConstantExpr>(call.getArgOperand(2))->getOperand(0));
    }
    if (task == nullptr) {
      llvm::errs() << "could not derive underlying task from omp call: " << call
                   << "\n";
      llvm_unreachable("could not derive underlying task from omp call");
    }
    if (task->empty()) {
      llvm::errs()
          << "could not derive underlying task contents from omp call: " << call
          << "\n";
      llvm_unreachable(
          "could not derive underlying task contents from omp call");
    }

    std::map<Argument *, bool> uncacheable_args;
    {
      auto in_arg = call.getCalledFunction()->arg_begin();
      auto pp_arg = task->arg_begin();

      // Global.tid is cacheable
      uncacheable_args[pp_arg] = false;
      ++pp_arg;
      // Bound.tid is cacheable
      uncacheable_args[pp_arg] = false;
      ++pp_arg;

      // Ignore the first three args of init call
      ++in_arg;
      ++in_arg;
      ++in_arg;

      for (auto &a : uncacheable_argsAbove) {
        llvm::errs() << "- " << *a.first << "\n";
      }
      for (; pp_arg != task->arg_end();) {
        // If var-args then we may still have args even though outermost
        // has no more
        if (in_arg == call.getCalledFunction()->arg_end()) {
          uncacheable_args[pp_arg] = true;
        } else {
          llvm::errs() << *in_arg << "\n";
          assert(uncacheable_argsAbove.find(in_arg) !=
                 uncacheable_argsAbove.end());
          uncacheable_args[pp_arg] = uncacheable_argsAbove.find(in_arg)->second;
          ++in_arg;
        }
        ++pp_arg;
      }
    }

    auto called = task;
    // bool modifyPrimal = true;

    bool foreignFunction = called == nullptr || called->empty();

    SmallVector<Value *, 8> args = {0, 0, 0};
    SmallVector<Value *, 8> pre_args = {0, 0, 0};
    std::vector<DIFFE_TYPE> argsInverted = {DIFFE_TYPE::CONSTANT,
                                            DIFFE_TYPE::CONSTANT};
    std::vector<Instruction *> postCreate;
    std::vector<Instruction *> userReplace;

    for (unsigned i = 3; i < call.getNumArgOperands(); ++i) {

      auto argi = gutils->getNewFromOriginal(call.getArgOperand(i));

      pre_args.push_back(argi);

      if (Mode != DerivativeMode::Forward) {
        IRBuilder<> Builder2(call.getParent());
        getReverseBuilder(Builder2);
        args.push_back(lookup(argi, Builder2));
      }

      if (gutils->isConstantValue(call.getArgOperand(i)) && !foreignFunction) {
        argsInverted.push_back(DIFFE_TYPE::CONSTANT);
        continue;
      }

      auto argType = argi->getType();

      if (!argType->isFPOrFPVectorTy() &&
          TR.query(call.getArgOperand(i)).Inner0().isPossiblePointer()) {
        DIFFE_TYPE ty = DIFFE_TYPE::DUP_ARG;
        if (argType->isPointerTy()) {
#if LLVM_VERSION_MAJOR >= 12
          auto at = getUnderlyingObject(call.getArgOperand(i), 100);
#else
          auto at = GetUnderlyingObject(
              call.getArgOperand(i),
              gutils->oldFunc->getParent()->getDataLayout(), 100);
#endif
          if (auto arg = dyn_cast<Argument>(at)) {
            if (constant_args[arg->getArgNo()] == DIFFE_TYPE::DUP_NONEED) {
              ty = DIFFE_TYPE::DUP_NONEED;
            }
          }
        }
        argsInverted.push_back(ty);

        if (Mode != DerivativeMode::Forward) {
          IRBuilder<> Builder2(call.getParent());
          getReverseBuilder(Builder2);
          args.push_back(
              gutils->invertPointerM(call.getArgOperand(i), Builder2));
        }
        pre_args.push_back(
            gutils->invertPointerM(call.getArgOperand(i), BuilderZ));

        // Note sometimes whattype mistakenly says something should be constant
        // [because composed of integer pointers alone]
        assert(whatType(argType) == DIFFE_TYPE::DUP_ARG ||
               whatType(argType) == DIFFE_TYPE::CONSTANT);
      } else {
        assert(0 && "out for omp not handled");
        argsInverted.push_back(DIFFE_TYPE::OUT_DIFF);
        assert(whatType(argType) == DIFFE_TYPE::OUT_DIFF ||
               whatType(argType) == DIFFE_TYPE::CONSTANT);
      }
    }

    DIFFE_TYPE subretType = DIFFE_TYPE::CONSTANT;

    Value *tape = nullptr;
    CallInst *augmentcall = nullptr;
    // Value *cachereplace = nullptr;

    FnTypeInfo nextTypeInfo(called);

    if (called) {
      std::map<Value *, std::set<int64_t>> intseen;

      TypeTree IntPtr;
      IntPtr.insert({-1, -1}, BaseType::Integer);
      IntPtr.insert({-1}, BaseType::Pointer);

      int argnum = 0;
      for (auto &arg : called->args()) {
        if (argnum <= 1) {
          nextTypeInfo.Arguments.insert(
              std::pair<Argument *, TypeTree>(&arg, IntPtr));
          nextTypeInfo.KnownValues.insert(
              std::pair<Argument *, std::set<int64_t>>(&arg, {}));
        } else {
          nextTypeInfo.Arguments.insert(std::pair<Argument *, TypeTree>(
              &arg, TR.query(call.getArgOperand(argnum - 2 + 3))));
          nextTypeInfo.KnownValues.insert(
              std::pair<Argument *, std::set<int64_t>>(
                  &arg,
                  TR.knownIntegralValues(call.getArgOperand(argnum - 2 + 3))));
        }

        ++argnum;
      }
      nextTypeInfo.Return = TR.query(&call);
    }

    // llvm::Optional<std::map<std::pair<Instruction*, std::string>, unsigned>>
    // sub_index_map;
    Optional<int> tapeIdx;
    Optional<int> returnIdx;
    Optional<int> differetIdx;

    const AugmentedReturn *subdata = nullptr;
    if (Mode == DerivativeMode::Reverse) {
      assert(augmentedReturn);
      if (augmentedReturn) {
        auto fd = augmentedReturn->subaugmentations.find(&call);
        if (fd != augmentedReturn->subaugmentations.end()) {
          subdata = fd->second;
        }
      }
    }

    if (Mode == DerivativeMode::Forward || Mode == DerivativeMode::Both) {
      if (called) {
        subdata = &CreateAugmentedPrimal(
            cast<Function>(called), subretType, argsInverted, gutils->TLI,
            TR.analysis, gutils->AA, /*return is used*/ false, nextTypeInfo,
            uncacheable_args, false, /*AtomicAdd*/ true);
        if (Mode == DerivativeMode::Forward) {
          assert(augmentedReturn);
          auto subaugmentations =
              (std::map<const llvm::CallInst *, AugmentedReturn *>
                   *)&augmentedReturn->subaugmentations;
          insert_or_assign2<const llvm::CallInst *, AugmentedReturn *>(
              *subaugmentations, &call, (AugmentedReturn *)subdata);
        }

        assert(subdata);
        auto numargs = ConstantInt::get(Type::getInt32Ty(call.getContext()),
                                        pre_args.size() - 3);
        auto newcalled = subdata->fn;
        pre_args[0] = gutils->getNewFromOriginal(call.getArgOperand(0));
        pre_args[1] = numargs;
        pre_args[2] = BuilderZ.CreatePointerCast(
            newcalled, kmpc->getFunctionType()->getParamType(2));
        augmentcall =
            BuilderZ.CreateCall(kmpc->getFunctionType(), kmpc, pre_args);
        augmentcall->setCallingConv(call.getCallingConv());
        augmentcall->setDebugLoc(
            gutils->getNewFromOriginal(call.getDebugLoc()));
        if (tapeIdx.hasValue()) {
          tape = (tapeIdx.getValue() == -1)
                     ? augmentcall
                     : BuilderZ.CreateExtractValue(
                           augmentcall, {(unsigned)tapeIdx.getValue()},
                           "subcache");
          if (tape->getType()->isEmptyTy()) {
            auto tt = tape->getType();
            gutils->erase(cast<Instruction>(tape));
            tape = UndefValue::get(tt);
          }
          tape = gutils->cacheForReverse(BuilderZ, tape,
                                         getIndex(&call, CacheType::Tape));
        }
        gutils->getNewFromOriginal(&call)->eraseFromParent();

      } else {
        assert(0 && "unhandled unknown outline");
      }
    }

    if (!subdata) {
      llvm::errs() << *gutils->oldFunc->getParent() << "\n";
      llvm::errs() << *gutils->oldFunc << "\n";
      llvm::errs() << *gutils->newFunc << "\n";
      llvm::errs() << *called << "\n";
      llvm_unreachable("no subdata");
    }

    auto found = subdata->returns.find(AugmentedStruct::DifferentialReturn);
    assert(found == subdata->returns.end());
    ;

    found = subdata->returns.find(AugmentedStruct::Return);
    assert(found == subdata->returns.end());

    found = subdata->returns.find(AugmentedStruct::Tape);
    if (found != subdata->returns.end()) {
      assert(0 && "openmp tape is unhandled");
      tapeIdx = found->second;
    }

    if (Mode == DerivativeMode::Reverse || Mode == DerivativeMode::Both) {
      IRBuilder<> Builder2(call.getParent());
      getReverseBuilder(Builder2);

      Value *newcalled = nullptr;
      if (called) {
        newcalled = CreatePrimalAndGradient(
            cast<Function>(called), subretType, argsInverted, gutils->TLI,
            TR.analysis, gutils->AA, /*returnValue*/ false,
            /*subdretptr*/ false, /*topLevel*/ false,
            tape ? tape->getType() : nullptr, nextTypeInfo, uncacheable_args,
            subdata, /*AtomicAdd*/ true);
        if (!newcalled)
          return;

        auto numargs = ConstantInt::get(Type::getInt32Ty(call.getContext()),
                                        args.size() - 3);
        args[0] =
            lookup(gutils->getNewFromOriginal(call.getArgOperand(0)), Builder2);
        args[1] = numargs;
        args[2] = Builder2.CreatePointerCast(
            newcalled, kmpc->getFunctionType()->getParamType(2));
        CallInst *diffes =
            Builder2.CreateCall(kmpc->getFunctionType(), kmpc, args);
        diffes->setCallingConv(call.getCallingConv());
        diffes->setDebugLoc(gutils->getNewFromOriginal(call.getDebugLoc()));

      } else {
        assert(0 && "openmp indirect unhandled");
      }
    }
  }

  // Return
  void visitCallInst(llvm::CallInst &call) {

    IRBuilder<> BuilderZ(gutils->getNewFromOriginal(&call));
    BuilderZ.setFastMathFlags(getFast());

    if (uncacheable_args_map.find(&call) == uncacheable_args_map.end()) {
      llvm::errs() << " call: " << call << "\n";
      for (auto &pair : uncacheable_args_map) {
        llvm::errs() << " + " << *pair.first << "\n";
      }
    }

    assert(uncacheable_args_map.find(&call) != uncacheable_args_map.end());
    const std::map<Argument *, bool> &uncacheable_args =
        uncacheable_args_map.find(&call)->second;

    CallInst *orig = &call;

    Function *called = orig->getCalledFunction();

    if (Mode != DerivativeMode::Forward) {
      if (called && called->getName() == "__kmpc_for_static_init_4") {
        IRBuilder<> Builder2(call.getParent());
        getReverseBuilder(Builder2);
        auto fini = called->getParent()->getFunction("__kmpc_for_static_fini");
        assert(fini);
        Value *args[] = {
            lookup(gutils->getNewFromOriginal(call.getArgOperand(0)), Builder2),
            lookup(gutils->getNewFromOriginal(call.getArgOperand(1)),
                   Builder2)};
        auto fcall = Builder2.CreateCall(fini->getFunctionType(), fini, args);
        fcall->setCallingConv(fini->getCallingConv());
        return;
      }
    }

#if LLVM_VERSION_MAJOR >= 11
    if (auto castinst = dyn_cast<ConstantExpr>(orig->getCalledOperand())) {
#else
    if (auto castinst = dyn_cast<ConstantExpr>(orig->getCalledValue())) {
#endif
      if (castinst->isCast())
        if (auto fn = dyn_cast<Function>(castinst->getOperand(0))) {
          if (isAllocationFunction(*fn, gutils->TLI) ||
              isDeallocationFunction(*fn, gutils->TLI)) {
            called = fn;
          }
        }
    }

    if (called &&
        (called->getName() == "printf" || called->getName() == "puts" ||
         called->getName().startswith("_ZN3std2io5stdio6_print") ||
         called->getName().startswith("_ZN4core3fmt"))) {
      if (Mode == DerivativeMode::Reverse) {
        eraseIfUnused(*orig, /*erase*/ true, /*check*/ false);
      }
      return;
    }
    if (called && (called->getName() == "__enzyme_float" ||
                   called->getName() == "__enzyme_double" ||
                   called->getName() == "__enzyme_integer" ||
                   called->getName() == "__enzyme_pointer")) {
      eraseIfUnused(*orig, /*erase*/ true, /*check*/ false);
      return;
    }

    // Handle lgamma, safe to recompute so no store/change to forward
    if (called) {
      auto n = called->getName();
      if (called->getName() == "__kmpc_fork_call") {
        visitOMPCall(call);
        return;
      }
      if (called &&
          (called->getName() == "asin" || called->getName() == "asinf" ||
           called->getName() == "asinl")) {
        if (gutils->isConstantInstruction(orig))
          return;

        IRBuilder<> Builder2(call.getParent());
        getReverseBuilder(Builder2);
        Value *x = lookup(gutils->getNewFromOriginal(orig->getArgOperand(0)),
                          Builder2);
        Value *oneMx2 = Builder2.CreateFSub(ConstantFP::get(x->getType(), 1.0),
                                            Builder2.CreateFMul(x, x));

        SmallVector<Value *, 1> args = {oneMx2};
        Type *tys[] = {x->getType()};
        auto cal = cast<CallInst>(
            Builder2.CreateCall(Intrinsic::getDeclaration(called->getParent(),
                                                          Intrinsic::sqrt, tys),
                                args));

        Value *dif0 = Builder2.CreateFDiv(diffe(orig, Builder2), cal);
        addToDiffe(orig->getArgOperand(0), dif0, Builder2, x->getType());
        return;
      }

      if (called &&
          (called->getName() == "tanhf" || called->getName() == "tanh")) {
        if (Mode == DerivativeMode::Forward ||
            gutils->isConstantInstruction(orig))
          return;

        IRBuilder<> Builder2(call.getParent());
        getReverseBuilder(Builder2);
        Value *x = lookup(gutils->getNewFromOriginal(orig->getArgOperand(0)),
                          Builder2);

        SmallVector<Value *, 1> args = {x};
        auto coshf = gutils->oldFunc->getParent()->getOrInsertFunction(
            (called->getName() == "tanh") ? "cosh" : "coshf",
            called->getFunctionType(), called->getAttributes());
        auto cal = cast<CallInst>(Builder2.CreateCall(coshf, args));
        Value *dif0 = Builder2.CreateFDiv(diffe(orig, Builder2),
                                          Builder2.CreateFMul(cal, cal));
        setDiffe(orig, Constant::getNullValue(orig->getType()), Builder2);
        addToDiffe(orig->getArgOperand(0), dif0, Builder2, x->getType());
        return;
      }

      if (n == "lgamma" || n == "lgammaf" || n == "lgammal" ||
          n == "lgamma_r" || n == "lgammaf_r" || n == "lgammal_r" ||
          n == "__lgamma_r_finite" || n == "__lgammaf_r_finite" ||
          n == "__lgammal_r_finite" || n == "acos" || n == "atan") {
        if (Mode == DerivativeMode::Forward ||
            gutils->isConstantInstruction(orig)) {
          return;
        }
      }
    }

    if (called && isAllocationFunction(*called, gutils->TLI)) {

      bool constval = gutils->isConstantValue(orig);

      if (!constval) {
        auto anti =
            gutils->createAntiMalloc(orig, getIndex(orig, CacheType::Shadow));
        if (Mode == DerivativeMode::Both || Mode == DerivativeMode::Reverse) {
          IRBuilder<> Builder2(call.getParent());
          getReverseBuilder(Builder2);
          Value *tofree = lookup(anti, Builder2);
          assert(tofree);
          assert(tofree->getType());
          assert(Type::getInt8Ty(tofree->getContext()));
          assert(PointerType::getUnqual(Type::getInt8Ty(tofree->getContext())));
          assert(Type::getInt8PtrTy(tofree->getContext()));
          freeKnownAllocation(Builder2, tofree, *called, gutils->TLI)
              ->addAttribute(AttributeList::FirstArgIndex, Attribute::NonNull);
        }
      }

      CallInst *const op = cast<CallInst>(gutils->getNewFromOriginal(&call));
      // TODO enable this if we need to free the memory
      // NOTE THAT TOPLEVEL IS THERE SIMPLY BECAUSE THAT WAS PREVIOUS ATTITUTE
      // TO FREE'ing
      if (Mode != DerivativeMode::Both) {
        if (is_value_needed_in_reverse<Primal>(
                TR, gutils, orig, /*topLevel*/ Mode == DerivativeMode::Both)) {

          gutils->cacheForReverse(BuilderZ, op,
                                  getIndex(orig, CacheType::Self));
        } else if (Mode != DerivativeMode::Forward) {
          // Note that here we cannot simply replace with null as users who try
          // to find the shadow pointer will use the shadow of null rather than
          // the true shadow of this
          auto pn = BuilderZ.CreatePHI(
              orig->getType(), 1, (orig->getName() + "_replacementB").str());
          gutils->fictiousPHIs.push_back(pn);
          gutils->replaceAWithB(op, pn);
          gutils->erase(op);
        }
      } else {
        IRBuilder<> Builder2(call.getParent());
        getReverseBuilder(Builder2);
        freeKnownAllocation(Builder2, lookup(op, Builder2), *called,
                            gutils->TLI);
      }

      return;
    }

    if (called && called->getName() == "posix_memalign") {
      if (gutils->invertedPointers.count(orig)) {
        auto placeholder = cast<PHINode>(gutils->invertedPointers[orig]);
        gutils->invertedPointers.erase(orig);
        gutils->erase(placeholder);
      }

      bool constval = gutils->isConstantValue(orig);

      if (!constval) {
        Value *val;
        IRBuilder<> Builder2(gutils->getNewFromOriginal(orig));
        if (Mode == DerivativeMode::Forward || Mode == DerivativeMode::Both) {
          Value *ptrshadow =
              gutils->invertPointerM(call.getArgOperand(0), Builder2);
          Builder2.CreateCall(
              called,
              std::vector<Value *>(
                  {ptrshadow, gutils->getNewFromOriginal(call.getArgOperand(1)),
                   gutils->getNewFromOriginal(call.getArgOperand(2))}));
          val = Builder2.CreateLoad(ptrshadow);
          val = gutils->cacheForReverse(Builder2, val,
                                        getIndex(orig, CacheType::Shadow));

          auto dst_arg = Builder2.CreateBitCast(
              val, Type::getInt8PtrTy(call.getContext()));
          auto val_arg =
              ConstantInt::get(Type::getInt8Ty(call.getContext()), 0);
          auto len_arg = Builder2.CreateZExtOrTrunc(
              gutils->getNewFromOriginal(call.getArgOperand(2)),
              Type::getInt64Ty(call.getContext()));
          auto volatile_arg = ConstantInt::getFalse(call.getContext());

#if LLVM_VERSION_MAJOR == 6
          auto align_arg =
              ConstantInt::get(Type::getInt32Ty(call.getContext()), 1);
          Value *nargs[] = {dst_arg, val_arg, len_arg, align_arg, volatile_arg};
#else
          Value *nargs[] = {dst_arg, val_arg, len_arg, volatile_arg};
#endif

          Type *tys[] = {dst_arg->getType(), len_arg->getType()};

          auto memset = cast<CallInst>(Builder2.CreateCall(
              Intrinsic::getDeclaration(gutils->newFunc->getParent(),
                                        Intrinsic::memset, tys),
              nargs));
          // memset->addParamAttr(0, Attribute::getWithAlignment(Context,
          // inst->getAlignment()));
          memset->addParamAttr(0, Attribute::NonNull);
        } else {
          PHINode *toReplace = Builder2.CreatePHI(
              cast<PointerType>(call.getArgOperand(0)->getType())
                  ->getElementType(),
              1, orig->getName() + "_psxtmp");
          val = gutils->cacheForReverse(Builder2, toReplace,
                                        getIndex(orig, CacheType::Shadow));
        }

        if (Mode == DerivativeMode::Both || Mode == DerivativeMode::Reverse) {
          IRBuilder<> Builder2(call.getParent());
          getReverseBuilder(Builder2);
          Value *tofree = gutils->lookupM(val, Builder2, ValueToValueMapTy(),
                                          /*tryLegalRecompute*/ false);
          auto freeCall = cast<CallInst>(
              CallInst::CreateFree(tofree, Builder2.GetInsertBlock()));
          Builder2.GetInsertBlock()->getInstList().push_back(freeCall);
        }
      }

      // CallInst *const op = cast<CallInst>(gutils->getNewFromOriginal(&call));
      // TODO enable this if we need to free the memory
      // NOTE THAT TOPLEVEL IS THERE SIMPLY BECAUSE THAT WAS PREVIOUS ATTITUTE
      // TO FREE'ing
      if (Mode != DerivativeMode::Both) {
        // if (is_value_needed_in_reverse<Primal>(
        //        TR, gutils, orig, /*topLevel*/ Mode == DerivativeMode::Both))
        //        {

        //  gutils->cacheForReverse(BuilderZ, op,
        //                          getIndex(orig, CacheType::Self));
        //} else if (Mode != DerivativeMode::Forward) {
        // Note that here we cannot simply replace with null as users who try
        // to find the shadow pointer will use the shadow of null rather than
        // the true shadow of this
        //}
      } else {
        IRBuilder<> Builder2(gutils->getNewFromOriginal(&call)->getNextNode());
        auto load = Builder2.CreateLoad(
            gutils->getNewFromOriginal(call.getOperand(0)), "posix_preread");
        Builder2.SetInsertPoint(&call);
        getReverseBuilder(Builder2);
        auto freeCall = cast<CallInst>(CallInst::CreateFree(
            gutils->lookupM(load, Builder2, ValueToValueMapTy(),
                            /*tryLegal*/ false),
            Builder2.GetInsertBlock()));
        Builder2.GetInsertBlock()->getInstList().push_back(freeCall);
      }

      return;
    }

    // Remove free's in forward pass so the memory can be used in the reverse
    // pass
    if (called && isDeallocationFunction(*called, gutils->TLI)) {
      if (gutils->invertedPointers.count(orig)) {
        auto placeholder = cast<PHINode>(gutils->invertedPointers[orig]);
        gutils->invertedPointers.erase(orig);
        gutils->erase(placeholder);
      }

      llvm::Value *val = orig->getArgOperand(0);
      while (auto cast = dyn_cast<CastInst>(val))
        val = cast->getOperand(0);

      if (auto dc = dyn_cast<CallInst>(val)) {
        if (dc->getCalledFunction() &&
            isAllocationFunction(*dc->getCalledFunction(), gutils->TLI)) {
          // llvm::errs() << "erasing free(orig): " << *orig << "\n";
          eraseIfUnused(*orig, /*erase*/ true, /*check*/ false);
          return;
        }
      }

      if (isa<ConstantPointerNull>(val)) {
        llvm::errs() << "removing free of null pointer\n";
        eraseIfUnused(*orig, /*erase*/ true, /*check*/ false);
        return;
      }

      // TODO HANDLE FREE
      llvm::errs() << "freeing without malloc " << *val << "\n";
      eraseIfUnused(*orig, /*erase*/ true, /*check*/ false);
      return;
    }

    bool subretused = unnecessaryValues.find(orig) == unnecessaryValues.end();
    // llvm::errs() << "orig: " << *orig << " ici:" <<
    // gutils->isConstantInstruction(orig) << " icv: " <<
    // gutils->isConstantValue(orig) << " subretused=" << subretused << " ivn:"
    // << is_value_needed_in_reverse<Primal>(TR, gutils, &call, /*topLevel*/Mode
    // == DerivativeMode::Both) << "\n";

    if (gutils->isConstantInstruction(orig) && gutils->isConstantValue(orig)) {
      // If we need this value and it is illegal to recompute it (it writes or
      // may load uncacheable data)
      //    Store and reload it
      if (Mode != DerivativeMode::Both && subretused &&
          !orig->doesNotAccessMemory()) {
        CallInst *const op = cast<CallInst>(gutils->getNewFromOriginal(&call));
        gutils->cacheForReverse(BuilderZ, op, getIndex(orig, CacheType::Self));
        return;
      }

      // If this call may write to memory and is a copy (in the just reverse
      // pass), erase it
      //  Any uses of it should be handled by the case above so it is safe to
      //  RAUW
      if (orig->mayWriteToMemory() && Mode == DerivativeMode::Reverse) {
        eraseIfUnused(*orig, /*erase*/ true, /*check*/ false);
        return;
      }

      // if call does not write memory and isn't used, we can erase it
      if (!orig->mayWriteToMemory() && !subretused) {
        eraseIfUnused(*orig, /*erase*/ true, /*check*/ false);
        return;
      }

      return;
    }

    bool modifyPrimal = shouldAugmentCall(orig, gutils, TR);

    bool foreignFunction = called == nullptr || called->empty();

    SmallVector<Value *, 8> args;
    SmallVector<Value *, 8> pre_args;
    std::vector<DIFFE_TYPE> argsInverted;
    std::vector<Instruction *> postCreate;
    std::vector<Instruction *> userReplace;

    for (unsigned i = 0; i < orig->getNumArgOperands(); ++i) {

      auto argi = gutils->getNewFromOriginal(orig->getArgOperand(i));

      pre_args.push_back(argi);

      if (Mode != DerivativeMode::Forward) {
        IRBuilder<> Builder2(call.getParent());
        getReverseBuilder(Builder2);
        args.push_back(lookup(argi, Builder2));
      }

      if (gutils->isConstantValue(orig->getArgOperand(i)) && !foreignFunction) {
        argsInverted.push_back(DIFFE_TYPE::CONSTANT);
        continue;
      }

      auto argType = argi->getType();

      if (!argType->isFPOrFPVectorTy() &&
          (TR.query(orig->getArgOperand(i)).Inner0().isPossiblePointer() ||
           foreignFunction)) {
        DIFFE_TYPE ty = DIFFE_TYPE::DUP_ARG;
        if (argType->isPointerTy()) {
#if LLVM_VERSION_MAJOR >= 12
          auto at = getUnderlyingObject(orig->getArgOperand(i), 100);
#else
          auto at = GetUnderlyingObject(
              orig->getArgOperand(i),
              gutils->oldFunc->getParent()->getDataLayout(), 100);
#endif
          if (auto arg = dyn_cast<Argument>(at)) {
            if (constant_args[arg->getArgNo()] == DIFFE_TYPE::DUP_NONEED) {
              ty = DIFFE_TYPE::DUP_NONEED;
            }
          }
        }
        argsInverted.push_back(ty);

        if (Mode != DerivativeMode::Forward) {
          IRBuilder<> Builder2(call.getParent());
          getReverseBuilder(Builder2);
          args.push_back(
              gutils->invertPointerM(orig->getArgOperand(i), Builder2));
        }
        pre_args.push_back(
            gutils->invertPointerM(orig->getArgOperand(i), BuilderZ));

        // Note sometimes whattype mistakenly says something should be constant
        // [because composed of integer pointers alone]
        assert(whatType(argType) == DIFFE_TYPE::DUP_ARG ||
               whatType(argType) == DIFFE_TYPE::CONSTANT);
      } else {
        if (foreignFunction)
          assert(!argType->isIntOrIntVectorTy());
        argsInverted.push_back(DIFFE_TYPE::OUT_DIFF);
        assert(whatType(argType) == DIFFE_TYPE::OUT_DIFF ||
               whatType(argType) == DIFFE_TYPE::CONSTANT);
      }
    }
    if (called) {
      if (orig->getNumArgOperands() !=
          cast<Function>(called)->getFunctionType()->getNumParams()) {
        llvm::errs() << *gutils->oldFunc << "\n";
        llvm::errs() << *orig << "\n";
      }
      assert(orig->getNumArgOperands() ==
             cast<Function>(called)->getFunctionType()->getNumParams());
      assert(argsInverted.size() ==
             cast<Function>(called)->getFunctionType()->getNumParams());
    }

    DIFFE_TYPE subretType;
    if (gutils->isConstantValue(orig)) {
      subretType = DIFFE_TYPE::CONSTANT;
    } else if (!orig->getType()->isFPOrFPVectorTy() &&
               TR.query(orig).Inner0().isPossiblePointer()) {
      subretType = DIFFE_TYPE::DUP_ARG;
      // TODO interprocedural dup_noneed
    } else {
      subretType = DIFFE_TYPE::OUT_DIFF;
    }

    bool replaceFunction = false;

    if (Mode == DerivativeMode::Both && !foreignFunction) {
      replaceFunction = legalCombinedForwardReverse(
          orig, *replacedReturns, postCreate, userReplace, gutils, TR,
          unnecessaryInstructions, subretused);
      if (replaceFunction)
        modifyPrimal = false;
    }

    Value *tape = nullptr;
    CallInst *augmentcall = nullptr;
    Value *cachereplace = nullptr;

    FnTypeInfo nextTypeInfo(called);
    int argnum = 0;

    if (called) {
      std::map<Value *, std::set<int64_t>> intseen;

      for (auto &arg : called->args()) {
        nextTypeInfo.Arguments.insert(std::pair<Argument *, TypeTree>(
            &arg, TR.query(orig->getArgOperand(argnum))));
        nextTypeInfo.KnownValues.insert(
            std::pair<Argument *, std::set<int64_t>>(
                &arg, TR.knownIntegralValues(orig->getArgOperand(argnum))));

        ++argnum;
      }
      nextTypeInfo.Return = TR.query(orig);
    }

    // llvm::Optional<std::map<std::pair<Instruction*, std::string>, unsigned>>
    // sub_index_map;
    Optional<int> tapeIdx;
    Optional<int> returnIdx;
    Optional<int> differetIdx;

    const AugmentedReturn *subdata = nullptr;
    if (Mode == DerivativeMode::Reverse) {
      assert(augmentedReturn);
      if (augmentedReturn) {
        auto fd = augmentedReturn->subaugmentations.find(&call);
        if (fd != augmentedReturn->subaugmentations.end()) {
          subdata = fd->second;
        }
      }
    }

    if (modifyPrimal) {

      Value *newcalled = nullptr;
      const AugmentedReturn *fnandtapetype = nullptr;

      if (!called) {
#if LLVM_VERSION_MAJOR >= 11
        auto callval = orig->getCalledOperand();
#else
        auto callval = orig->getCalledValue();
#endif
        newcalled = gutils->invertPointerM(callval, BuilderZ);

        auto ft = cast<FunctionType>(
            cast<PointerType>(callval->getType())->getElementType());

        DIFFE_TYPE subretType = orig->getType()->isFPOrFPVectorTy()
                                    ? DIFFE_TYPE::OUT_DIFF
                                    : DIFFE_TYPE::DUP_ARG;
        if (orig->getType()->isVoidTy() || orig->getType()->isEmptyTy())
          subretType = DIFFE_TYPE::CONSTANT;
        auto res = getDefaultFunctionTypeForAugmentation(
            ft, /*returnUsed*/ true, /*subretType*/ subretType);
        auto fptype = PointerType::getUnqual(FunctionType::get(
            StructType::get(newcalled->getContext(), res.second), res.first,
            ft->isVarArg()));
        newcalled = BuilderZ.CreatePointerCast(newcalled,
                                               PointerType::getUnqual(fptype));
        newcalled = BuilderZ.CreateLoad(newcalled);
        tapeIdx = 0;

        if (subretType == DIFFE_TYPE::DUP_ARG ||
            subretType == DIFFE_TYPE::DUP_NONEED) {
          returnIdx = 1;
          differetIdx = 2;
        }

      } else {
        if (Mode == DerivativeMode::Forward || Mode == DerivativeMode::Both) {
          subdata = &CreateAugmentedPrimal(
              cast<Function>(called), subretType, argsInverted, gutils->TLI,
              TR.analysis, gutils->AA, /*return is used*/ subretused,
              nextTypeInfo, uncacheable_args, false, gutils->AtomicAdd);
          if (Mode == DerivativeMode::Forward) {
            assert(augmentedReturn);
            auto subaugmentations =
                (std::map<const llvm::CallInst *, AugmentedReturn *>
                     *)&augmentedReturn->subaugmentations;
            insert_or_assign2<const llvm::CallInst *, AugmentedReturn *>(
                *subaugmentations, orig, (AugmentedReturn *)subdata);
          }
        }
        if (!subdata) {
          llvm::errs() << *gutils->oldFunc->getParent() << "\n";
          llvm::errs() << *gutils->oldFunc << "\n";
          llvm::errs() << *gutils->newFunc << "\n";
          llvm::errs() << *called << "\n";
        }
        assert(subdata);
        fnandtapetype = subdata;
        newcalled = subdata->fn;

        auto found = subdata->returns.find(AugmentedStruct::DifferentialReturn);
        if (found != subdata->returns.end()) {
          differetIdx = found->second;
        }

        found = subdata->returns.find(AugmentedStruct::Return);
        if (found != subdata->returns.end()) {
          returnIdx = found->second;
        }

        found = subdata->returns.find(AugmentedStruct::Tape);
        if (found != subdata->returns.end()) {
          tapeIdx = found->second;
        }
      }
      // sub_index_map = fnandtapetype.tapeIndices;

      assert(newcalled);
      FunctionType *FT = cast<FunctionType>(
          cast<PointerType>(newcalled->getType())->getElementType());

      // llvm::errs() << "seeing sub_index_map of " << sub_index_map->size() <<
      // " in ap " << cast<Function>(called)->getName() << "\n";
      if (Mode == DerivativeMode::Both || Mode == DerivativeMode::Forward) {

        if (false) {
        badaugmentedfn:;
          auto NC = dyn_cast<Function>(newcalled);
          llvm::errs() << *gutils->oldFunc << "\n";
          llvm::errs() << *gutils->newFunc << "\n";
          if (NC)
            llvm::errs() << " trying to call " << NC->getName() << " " << *FT
                         << "\n";
          else
            llvm::errs() << " trying to call " << *newcalled << " " << *FT
                         << "\n";

          for (unsigned i = 0; i < pre_args.size(); ++i) {
            assert(pre_args[i]);
            assert(pre_args[i]->getType());
            llvm::errs() << "args[" << i << "] = " << *pre_args[i]
                         << " FT:" << *FT->getParamType(i) << "\n";
          }
          assert(0 && "calling with wrong number of arguments");
          exit(1);
        }

        if (pre_args.size() != FT->getNumParams())
          goto badaugmentedfn;

        for (unsigned i = 0; i < pre_args.size(); ++i) {
          if (pre_args[i]->getType() != FT->getParamType(i))
            goto badaugmentedfn;
        }

        augmentcall = BuilderZ.CreateCall(FT, newcalled, pre_args);
        augmentcall->setCallingConv(orig->getCallingConv());
        augmentcall->setDebugLoc(
            gutils->getNewFromOriginal(orig->getDebugLoc()));

        if (!augmentcall->getType()->isVoidTy())
          augmentcall->setName(orig->getName() + "_augmented");

        if (tapeIdx.hasValue()) {
          tape = (tapeIdx.getValue() == -1)
                     ? augmentcall
                     : BuilderZ.CreateExtractValue(
                           augmentcall, {(unsigned)tapeIdx.getValue()},
                           "subcache");
          if (tape->getType()->isEmptyTy()) {
            auto tt = tape->getType();
            gutils->erase(cast<Instruction>(tape));
            tape = UndefValue::get(tt);
          }
          tape = gutils->cacheForReverse(BuilderZ, tape,
                                         getIndex(orig, CacheType::Tape));
        }

        if (subretused) {
          CallInst *const op =
              cast<CallInst>(gutils->getNewFromOriginal(&call));

          Value *dcall = nullptr;
          dcall = (returnIdx.getValue() < 0)
                      ? augmentcall
                      : BuilderZ.CreateExtractValue(
                            augmentcall, {(unsigned)returnIdx.getValue()});
          gutils->originalToNewFn[orig] = dcall;
          assert(dcall->getType() == orig->getType());
          assert(dcall);

          if (!gutils->isConstantValue(orig)) {
            gutils->originalToNewFn[orig] = dcall;
            if (!orig->getType()->isFPOrFPVectorTy() &&
                TR.query(orig).Inner0().isPossiblePointer()) {
            } else if (Mode != DerivativeMode::Forward) {
              ((DiffeGradientUtils *)gutils)->differentials[dcall] =
                  ((DiffeGradientUtils *)gutils)->differentials[op];
              ((DiffeGradientUtils *)gutils)->differentials.erase(op);
            }
          }
          assert(dcall->getType() == orig->getType());
          gutils->replaceAWithB(op, dcall);

          auto name = op->getName().str();
          op->setName("");
          if (isa<Instruction>(dcall) && !isa<PHINode>(dcall)) {
            cast<Instruction>(dcall)->setName(name);
          }

          if (Mode == DerivativeMode::Forward &&
              is_value_needed_in_reverse<Primal>(TR, gutils, orig,
                                                 /*topLevel*/ Mode ==
                                                     DerivativeMode::Both)) {
            gutils->cacheForReverse(BuilderZ, dcall,
                                    getIndex(orig, CacheType::Self));
          }

          BuilderZ.SetInsertPoint(op->getNextNode());
          gutils->erase(op);
        } else {
          BuilderZ.SetInsertPoint(BuilderZ.GetInsertPoint()->getNextNode());
          eraseIfUnused(*orig, /*erase*/ true, /*check*/ false);
          gutils->originalToNewFn[orig] = augmentcall;
        }

      } else {
        if (subdata && subdata->returns.find(AugmentedStruct::Tape) ==
                           subdata->returns.end()) {
        } else {
          // assert(!tape);
          // assert(subdata);
          if (!tape) {
            assert(tapeIdx.hasValue());
            tape = BuilderZ.CreatePHI(
                (tapeIdx == -1) ? FT->getReturnType()
                                : cast<StructType>(FT->getReturnType())
                                      ->getElementType(tapeIdx.getValue()),
                1, "tapeArg");
          }
          tape = gutils->cacheForReverse(BuilderZ, tape,
                                         getIndex(orig, CacheType::Tape));
        }

        if (subretused) {
          if (is_value_needed_in_reverse<Primal>(
                  TR, gutils, orig, Mode == DerivativeMode::Both)) {
            cachereplace = BuilderZ.CreatePHI(orig->getType(), 1,
                                              orig->getName() + "_tmpcacheB");
            cachereplace = gutils->cacheForReverse(
                BuilderZ, cachereplace, getIndex(orig, CacheType::Self));
          } else {
            auto pn = BuilderZ.CreatePHI(
                orig->getType(), 1, (orig->getName() + "_replacementE").str());
            gutils->fictiousPHIs.push_back(pn);
            cachereplace = pn;
          }
        } else {
          // TODO move right after op for the insertion point of BuilderZ

          BuilderZ.SetInsertPoint(BuilderZ.GetInsertPoint()->getNextNode());
          eraseIfUnused(*orig, /*erase*/ true, /*check*/ false);
        }
      }

      if (gutils->invertedPointers.count(orig)) {

        auto placeholder = cast<PHINode>(gutils->invertedPointers[orig]);

        bool subcheck = (subretType == DIFFE_TYPE::DUP_ARG ||
                         subretType == DIFFE_TYPE::DUP_NONEED);

        //! We only need the shadow pointer for non-forward Mode if it is used
        //! in a non return setting
        bool hasNonReturnUse = false;
        for (auto use : orig->users()) {
          if (Mode == DerivativeMode::Forward ||
              !isa<ReturnInst>(
                  use)) { // || returnuses.find(cast<Instruction>(use)) ==
                          // returnuses.end()) {
            hasNonReturnUse = true;
          }
        }

        if (subcheck && hasNonReturnUse) {

          Value *newip = nullptr;
          if (Mode == DerivativeMode::Both || Mode == DerivativeMode::Forward) {
            newip = (differetIdx.getValue() < 0)
                        ? augmentcall
                        : BuilderZ.CreateExtractValue(
                              augmentcall, {(unsigned)differetIdx.getValue()},
                              orig->getName() + "'ac");
            assert(newip->getType() == orig->getType());
            placeholder->replaceAllUsesWith(newip);
            gutils->erase(placeholder);
          } else {
            newip = placeholder;
          }

          newip = gutils->cacheForReverse(BuilderZ, newip,
                                          getIndex(orig, CacheType::Shadow));

          gutils->invertedPointers[orig] = newip;
        } else {
          gutils->invertedPointers.erase(orig);
          gutils->erase(placeholder);
        }
      }

      if (fnandtapetype && fnandtapetype->tapeType &&
          Mode != DerivativeMode::Forward) {
        auto tapep = BuilderZ.CreatePointerCast(
            tape, PointerType::getUnqual(fnandtapetype->tapeType));
        auto truetape = BuilderZ.CreateLoad(tapep, "tapeld");
        truetape->setMetadata("enzyme_mustcache",
                              MDNode::get(truetape->getContext(), {}));

        CallInst *ci = cast<CallInst>(
            CallInst::CreateFree(tape, &*BuilderZ.GetInsertPoint()));
        ci->addAttribute(AttributeList::FirstArgIndex, Attribute::NonNull);
        tape = truetape;
      }
    } else {
      if (gutils->invertedPointers.count(orig)) {
        auto placeholder = cast<PHINode>(gutils->invertedPointers[orig]);
        gutils->invertedPointers.erase(orig);
        gutils->erase(placeholder);
      }
      if (/*!topLevel*/ Mode != DerivativeMode::Both && subretused &&
          !orig->doesNotAccessMemory()) {
        if (is_value_needed_in_reverse<Primal>(TR, gutils, orig,
                                               Mode == DerivativeMode::Both)) {
          assert(!replaceFunction);
          cachereplace = BuilderZ.CreatePHI(orig->getType(), 1,
                                            orig->getName() + "_cachereplace2");
          cachereplace = gutils->cacheForReverse(
              BuilderZ, cachereplace, getIndex(orig, CacheType::Self));
        } else {
          auto pn = BuilderZ.CreatePHI(
              orig->getType(), 1, (orig->getName() + "_replacementC").str());
          gutils->fictiousPHIs.push_back(pn);
          cachereplace = pn; // UndefValue::get(op->getType());
          // cachereplace = UndefValue::get(op->getType());
        }
      }

      if (!subretused && !replaceFunction)
        eraseIfUnused(*orig, /*erase*/ true, /*check*/ false);
    }

    // Note here down only contains the reverse bits
    if (Mode == DerivativeMode::Forward) {
      return;
    }

    IRBuilder<> Builder2(call.getParent());
    getReverseBuilder(Builder2);

    bool retUsed = replaceFunction && subretused;
    Value *newcalled = nullptr;

    bool subdretptr = (subretType == DIFFE_TYPE::DUP_ARG ||
                       subretType == DIFFE_TYPE::DUP_NONEED) &&
                      replaceFunction && (call.getNumUses() != 0);
    bool subtopLevel = replaceFunction || !modifyPrimal;
    if (called) {
      newcalled = CreatePrimalAndGradient(
          cast<Function>(called), subretType, argsInverted, gutils->TLI,
          TR.analysis, gutils->AA, /*returnValue*/ retUsed,
          /*subdretptr*/ subdretptr, /*topLevel*/ subtopLevel,
          tape ? tape->getType() : nullptr, nextTypeInfo, uncacheable_args,
          subdata, gutils->AtomicAdd); //, LI, DT);
      if (!newcalled)
        return;
    } else {

      assert(!subtopLevel);

#if LLVM_VERSION_MAJOR >= 11
      auto callval = orig->getCalledOperand();
#else
      auto callval = orig->getCalledValue();
#endif

      newcalled = gutils->invertPointerM(callval, Builder2);

      auto ft = cast<FunctionType>(
          cast<PointerType>(callval->getType())->getElementType());

      DIFFE_TYPE subretType = orig->getType()->isFPOrFPVectorTy()
                                  ? DIFFE_TYPE::OUT_DIFF
                                  : DIFFE_TYPE::DUP_ARG;
      if (orig->getType()->isVoidTy() || orig->getType()->isEmptyTy())
        subretType = DIFFE_TYPE::CONSTANT;
      auto res =
          getDefaultFunctionTypeForGradient(ft, /*subretType*/ subretType);
      // TODO Note there is empty tape added here, replace with generic
      res.first.push_back(Type::getInt8PtrTy(newcalled->getContext()));
      auto fptype = PointerType::getUnqual(FunctionType::get(
          StructType::get(newcalled->getContext(), res.second), res.first,
          ft->isVarArg()));
      newcalled =
          Builder2.CreatePointerCast(newcalled, PointerType::getUnqual(fptype));
      newcalled =
          Builder2.CreateLoad(Builder2.CreateConstGEP1_64(newcalled, 1));
    }

    if (subretType == DIFFE_TYPE::OUT_DIFF) {
      args.push_back(diffe(orig, Builder2));
    }

    if (tape) {
      auto ntape = gutils->lookupM(tape, Builder2);
      assert(ntape);
      assert(ntape->getType());
      args.push_back(ntape);
    }

    assert(newcalled);
    // if (auto NC = dyn_cast<Function>(newcalled)) {
    FunctionType *FT = cast<FunctionType>(
        cast<PointerType>(newcalled->getType())->getElementType());

    if (false) {
    badfn:;
      auto NC = dyn_cast<Function>(newcalled);
      llvm::errs() << *gutils->oldFunc << "\n";
      llvm::errs() << *gutils->newFunc << "\n";
      if (NC)
        llvm::errs() << " trying to call " << NC->getName() << " " << *FT
                     << "\n";
      else
        llvm::errs() << " trying to call " << *newcalled << " " << *FT << "\n";

      for (unsigned i = 0; i < args.size(); ++i) {
        assert(args[i]);
        assert(args[i]->getType());
        llvm::errs() << "args[" << i << "] = " << *args[i]
                     << " FT:" << *FT->getParamType(i) << "\n";
      }
      assert(0 && "calling with wrong number of arguments");
      exit(1);
    }

    if (args.size() != FT->getNumParams())
      goto badfn;

    for (unsigned i = 0; i < args.size(); ++i) {
      if (args[i]->getType() != FT->getParamType(i))
        goto badfn;
    }

    CallInst *diffes = Builder2.CreateCall(FT, newcalled, args);
    diffes->setCallingConv(orig->getCallingConv());
    diffes->setDebugLoc(gutils->getNewFromOriginal(orig->getDebugLoc()));

    unsigned structidx = retUsed ? 1 : 0;
    if (subdretptr)
      ++structidx;

    for (unsigned i = 0; i < orig->getNumArgOperands(); ++i) {
      if (argsInverted[i] == DIFFE_TYPE::OUT_DIFF) {
        Value *diffeadd = Builder2.CreateExtractValue(diffes, {structidx});
        ++structidx;

        size_t size = 1;
        if (orig->getArgOperand(i)->getType()->isSized())
          size =
              (gutils->newFunc->getParent()->getDataLayout().getTypeSizeInBits(
                   orig->getArgOperand(i)->getType()) +
               7) /
              8;

        addToDiffe(orig->getArgOperand(i), diffeadd, Builder2,
                   TR.intType(size, orig->getArgOperand(i), false).isFloat());
      }
    }

    if (diffes->getType()->isVoidTy()) {
      if (structidx != 0) {
        llvm::errs() << *gutils->oldFunc->getParent() << "\n";
        llvm::errs() << "diffes: " << *diffes << " structidx=" << structidx
                     << " retUsed=" << retUsed << " subretptr=" << subdretptr
                     << "\n";
      }
      assert(structidx == 0);
    } else {
      assert(cast<StructType>(diffes->getType())->getNumElements() ==
             structidx);
    }

    if (subretType == DIFFE_TYPE::OUT_DIFF)
      setDiffe(orig, Constant::getNullValue(orig->getType()), Builder2);

    if (replaceFunction) {

      // if a function is replaced for joint forward/reverse, handle inverted
      // pointers
      if (gutils->invertedPointers.count(orig)) {
        auto placeholder = cast<PHINode>(gutils->invertedPointers[orig]);
        gutils->invertedPointers.erase(orig);
        if (subdretptr) {
          dumpMap(gutils->invertedPointers);
          auto dretval =
              cast<Instruction>(Builder2.CreateExtractValue(diffes, {1}));
          /* todo handle this case later */
          assert(!subretused);
          gutils->invertedPointers[orig] = dretval;
        }
        gutils->erase(placeholder);
      }

      Instruction *retval = nullptr;

      CallInst *const op = cast<CallInst>(gutils->getNewFromOriginal(&call));

      ValueToValueMapTy mapp;
      if (subretused) {
        retval = cast<Instruction>(Builder2.CreateExtractValue(diffes, {0}));
        gutils->replaceAWithB(op, retval, /*storeInCache*/ true);
        mapp[op] = retval;
      } else {
        eraseIfUnused(*orig, /*erase*/ false, /*check*/ false);
      }

      for (auto &a : *gutils->reverseBlocks[cast<BasicBlock>(
               gutils->getNewFromOriginal(orig->getParent()))]) {
        mapp[&a] = &a;
      }

      std::reverse(postCreate.begin(), postCreate.end());
      for (auto a : postCreate) {

        // If is the store to return handle manually since no original inst for
        bool fromStore = false;
        for (auto &pair : *replacedReturns) {
          if (pair.second == a) {
            for (unsigned i = 0; i < a->getNumOperands(); ++i) {
              a->setOperand(i, gutils->unwrapM(a->getOperand(i), Builder2, mapp,
                                               UnwrapMode::LegalFullUnwrap));
            }
            a->moveBefore(*Builder2.GetInsertBlock(),
                          Builder2.GetInsertPoint());
            fromStore = true;
            break;
          }
        }
        if (fromStore)
          continue;

        auto orig_a = gutils->isOriginal(a);
        if (orig_a) {
          for (unsigned i = 0; i < a->getNumOperands(); ++i) {
            a->setOperand(i,
                          gutils->unwrapM(
                              gutils->getNewFromOriginal(orig_a->getOperand(i)),
                              Builder2, mapp, UnwrapMode::LegalFullUnwrap));
          }
        }
        a->moveBefore(*Builder2.GetInsertBlock(), Builder2.GetInsertPoint());
        mapp[a] = a;
      }

      gutils->originalToNewFn[orig] = retval ? retval : diffes;

      // llvm::errs() << "newFunc postrep: " << *gutils->newFunc << "\n";

      erased.insert(orig);
      gutils->erase(op);

      return;
    }

    if (cachereplace) {
      if (subretused) {
        Value *dcall = nullptr;
        assert(cachereplace->getType() == orig->getType());
        assert(dcall == nullptr);
        dcall = cachereplace;
        assert(dcall);

        CallInst *const op = cast<CallInst>(gutils->getNewFromOriginal(&call));

        if (!gutils->isConstantValue(orig)) {
          gutils->originalToNewFn[orig] = dcall;
          if (!orig->getType()->isFPOrFPVectorTy() &&
              TR.query(orig).Inner0().isPossiblePointer()) {
          } else {
            ((DiffeGradientUtils *)gutils)->differentials[dcall] =
                ((DiffeGradientUtils *)gutils)->differentials[op];
            ((DiffeGradientUtils *)gutils)->differentials.erase(op);
          }
        }
        assert(dcall->getType() == orig->getType());
        op->replaceAllUsesWith(dcall);
        auto name = orig->getName();
        op->setName("");
        if (isa<Instruction>(dcall) && !isa<PHINode>(dcall)) {
          cast<Instruction>(dcall)->setName(name);
        }
        gutils->erase(op);
      } else {
        eraseIfUnused(*orig, /*erase*/ true, /*check*/ false);
        if (augmentcall) {
          gutils->originalToNewFn[orig] = augmentcall;
        }
      }
    }
    return;
  }
};
