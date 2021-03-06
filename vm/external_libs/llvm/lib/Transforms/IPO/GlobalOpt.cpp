//===- GlobalOpt.cpp - Optimize Global Variables --------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass transforms simple global variables that never have their address
// taken.  If obviously true, it marks read/write globals as constant, deletes
// variables only stored to, etc.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "globalopt"
#include "llvm/Transforms/IPO.h"
#include "llvm/CallingConv.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include <algorithm>
#include <map>
#include <set>
using namespace llvm;

STATISTIC(NumMarked    , "Number of globals marked constant");
STATISTIC(NumSRA       , "Number of aggregate globals broken into scalars");
STATISTIC(NumHeapSRA   , "Number of heap objects SRA'd");
STATISTIC(NumSubstitute,"Number of globals with initializers stored into them");
STATISTIC(NumDeleted   , "Number of globals deleted");
STATISTIC(NumFnDeleted , "Number of functions deleted");
STATISTIC(NumGlobUses  , "Number of global uses devirtualized");
STATISTIC(NumLocalized , "Number of globals localized");
STATISTIC(NumShrunkToBool  , "Number of global vars shrunk to booleans");
STATISTIC(NumFastCallFns   , "Number of functions converted to fastcc");
STATISTIC(NumCtorsEvaluated, "Number of static ctors evaluated");
STATISTIC(NumNestRemoved   , "Number of nest attributes removed");

namespace {
  struct VISIBILITY_HIDDEN GlobalOpt : public ModulePass {
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<TargetData>();
    }
    static char ID; // Pass identification, replacement for typeid
    GlobalOpt() : ModulePass((intptr_t)&ID) {}

    bool runOnModule(Module &M);

  private:
    GlobalVariable *FindGlobalCtors(Module &M);
    bool OptimizeFunctions(Module &M);
    bool OptimizeGlobalVars(Module &M);
    bool OptimizeGlobalCtorsList(GlobalVariable *&GCL);
    bool ProcessInternalGlobal(GlobalVariable *GV,Module::global_iterator &GVI);
  };

  char GlobalOpt::ID = 0;
  RegisterPass<GlobalOpt> X("globalopt", "Global Variable Optimizer");
}

ModulePass *llvm::createGlobalOptimizerPass() { return new GlobalOpt(); }

/// GlobalStatus - As we analyze each global, keep track of some information
/// about it.  If we find out that the address of the global is taken, none of
/// this info will be accurate.
struct VISIBILITY_HIDDEN GlobalStatus {
  /// isLoaded - True if the global is ever loaded.  If the global isn't ever
  /// loaded it can be deleted.
  bool isLoaded;

  /// StoredType - Keep track of what stores to the global look like.
  ///
  enum StoredType {
    /// NotStored - There is no store to this global.  It can thus be marked
    /// constant.
    NotStored,

    /// isInitializerStored - This global is stored to, but the only thing
    /// stored is the constant it was initialized with.  This is only tracked
    /// for scalar globals.
    isInitializerStored,

    /// isStoredOnce - This global is stored to, but only its initializer and
    /// one other value is ever stored to it.  If this global isStoredOnce, we
    /// track the value stored to it in StoredOnceValue below.  This is only
    /// tracked for scalar globals.
    isStoredOnce,

    /// isStored - This global is stored to by multiple values or something else
    /// that we cannot track.
    isStored
  } StoredType;

  /// StoredOnceValue - If only one value (besides the initializer constant) is
  /// ever stored to this global, keep track of what value it is.
  Value *StoredOnceValue;

  /// AccessingFunction/HasMultipleAccessingFunctions - These start out
  /// null/false.  When the first accessing function is noticed, it is recorded.
  /// When a second different accessing function is noticed,
  /// HasMultipleAccessingFunctions is set to true.
  Function *AccessingFunction;
  bool HasMultipleAccessingFunctions;

  /// HasNonInstructionUser - Set to true if this global has a user that is not
  /// an instruction (e.g. a constant expr or GV initializer).
  bool HasNonInstructionUser;

  /// HasPHIUser - Set to true if this global has a user that is a PHI node.
  bool HasPHIUser;
  
  GlobalStatus() : isLoaded(false), StoredType(NotStored), StoredOnceValue(0),
                   AccessingFunction(0), HasMultipleAccessingFunctions(false),
                   HasNonInstructionUser(false), HasPHIUser(false) {}
};



/// ConstantIsDead - Return true if the specified constant is (transitively)
/// dead.  The constant may be used by other constants (e.g. constant arrays and
/// constant exprs) as long as they are dead, but it cannot be used by anything
/// else.
static bool ConstantIsDead(Constant *C) {
  if (isa<GlobalValue>(C)) return false;

  for (Value::use_iterator UI = C->use_begin(), E = C->use_end(); UI != E; ++UI)
    if (Constant *CU = dyn_cast<Constant>(*UI)) {
      if (!ConstantIsDead(CU)) return false;
    } else
      return false;
  return true;
}


/// AnalyzeGlobal - Look at all uses of the global and fill in the GlobalStatus
/// structure.  If the global has its address taken, return true to indicate we
/// can't do anything with it.
///
static bool AnalyzeGlobal(Value *V, GlobalStatus &GS,
                          std::set<PHINode*> &PHIUsers) {
  for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E; ++UI)
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(*UI)) {
      GS.HasNonInstructionUser = true;

      if (AnalyzeGlobal(CE, GS, PHIUsers)) return true;

    } else if (Instruction *I = dyn_cast<Instruction>(*UI)) {
      if (!GS.HasMultipleAccessingFunctions) {
        Function *F = I->getParent()->getParent();
        if (GS.AccessingFunction == 0)
          GS.AccessingFunction = F;
        else if (GS.AccessingFunction != F)
          GS.HasMultipleAccessingFunctions = true;
      }
      if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
        GS.isLoaded = true;
        if (LI->isVolatile()) return true;  // Don't hack on volatile loads.
      } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
        // Don't allow a store OF the address, only stores TO the address.
        if (SI->getOperand(0) == V) return true;

        if (SI->isVolatile()) return true;  // Don't hack on volatile stores.

        // If this is a direct store to the global (i.e., the global is a scalar
        // value, not an aggregate), keep more specific information about
        // stores.
        if (GS.StoredType != GlobalStatus::isStored) {
          if (GlobalVariable *GV = dyn_cast<GlobalVariable>(SI->getOperand(1))){
            Value *StoredVal = SI->getOperand(0);
            if (StoredVal == GV->getInitializer()) {
              if (GS.StoredType < GlobalStatus::isInitializerStored)
                GS.StoredType = GlobalStatus::isInitializerStored;
            } else if (isa<LoadInst>(StoredVal) &&
                       cast<LoadInst>(StoredVal)->getOperand(0) == GV) {
              // G = G
              if (GS.StoredType < GlobalStatus::isInitializerStored)
                GS.StoredType = GlobalStatus::isInitializerStored;
            } else if (GS.StoredType < GlobalStatus::isStoredOnce) {
              GS.StoredType = GlobalStatus::isStoredOnce;
              GS.StoredOnceValue = StoredVal;
            } else if (GS.StoredType == GlobalStatus::isStoredOnce &&
                       GS.StoredOnceValue == StoredVal) {
              // noop.
            } else {
              GS.StoredType = GlobalStatus::isStored;
            }
          } else {
            GS.StoredType = GlobalStatus::isStored;
          }
        }
      } else if (isa<GetElementPtrInst>(I)) {
        if (AnalyzeGlobal(I, GS, PHIUsers)) return true;
      } else if (isa<SelectInst>(I)) {
        if (AnalyzeGlobal(I, GS, PHIUsers)) return true;
      } else if (PHINode *PN = dyn_cast<PHINode>(I)) {
        // PHI nodes we can check just like select or GEP instructions, but we
        // have to be careful about infinite recursion.
        if (PHIUsers.insert(PN).second)  // Not already visited.
          if (AnalyzeGlobal(I, GS, PHIUsers)) return true;
        GS.HasPHIUser = true;
      } else if (isa<CmpInst>(I)) {
      } else if (isa<MemCpyInst>(I) || isa<MemMoveInst>(I)) {
        if (I->getOperand(1) == V)
          GS.StoredType = GlobalStatus::isStored;
        if (I->getOperand(2) == V)
          GS.isLoaded = true;
      } else if (isa<MemSetInst>(I)) {
        assert(I->getOperand(1) == V && "Memset only takes one pointer!");
        GS.StoredType = GlobalStatus::isStored;
      } else {
        return true;  // Any other non-load instruction might take address!
      }
    } else if (Constant *C = dyn_cast<Constant>(*UI)) {
      GS.HasNonInstructionUser = true;
      // We might have a dead and dangling constant hanging off of here.
      if (!ConstantIsDead(C))
        return true;
    } else {
      GS.HasNonInstructionUser = true;
      // Otherwise must be some other user.
      return true;
    }

  return false;
}

static Constant *getAggregateConstantElement(Constant *Agg, Constant *Idx) {
  ConstantInt *CI = dyn_cast<ConstantInt>(Idx);
  if (!CI) return 0;
  unsigned IdxV = CI->getZExtValue();

  if (ConstantStruct *CS = dyn_cast<ConstantStruct>(Agg)) {
    if (IdxV < CS->getNumOperands()) return CS->getOperand(IdxV);
  } else if (ConstantArray *CA = dyn_cast<ConstantArray>(Agg)) {
    if (IdxV < CA->getNumOperands()) return CA->getOperand(IdxV);
  } else if (ConstantVector *CP = dyn_cast<ConstantVector>(Agg)) {
    if (IdxV < CP->getNumOperands()) return CP->getOperand(IdxV);
  } else if (isa<ConstantAggregateZero>(Agg)) {
    if (const StructType *STy = dyn_cast<StructType>(Agg->getType())) {
      if (IdxV < STy->getNumElements())
        return Constant::getNullValue(STy->getElementType(IdxV));
    } else if (const SequentialType *STy =
               dyn_cast<SequentialType>(Agg->getType())) {
      return Constant::getNullValue(STy->getElementType());
    }
  } else if (isa<UndefValue>(Agg)) {
    if (const StructType *STy = dyn_cast<StructType>(Agg->getType())) {
      if (IdxV < STy->getNumElements())
        return UndefValue::get(STy->getElementType(IdxV));
    } else if (const SequentialType *STy =
               dyn_cast<SequentialType>(Agg->getType())) {
      return UndefValue::get(STy->getElementType());
    }
  }
  return 0;
}


/// CleanupConstantGlobalUsers - We just marked GV constant.  Loop over all
/// users of the global, cleaning up the obvious ones.  This is largely just a
/// quick scan over the use list to clean up the easy and obvious cruft.  This
/// returns true if it made a change.
static bool CleanupConstantGlobalUsers(Value *V, Constant *Init) {
  bool Changed = false;
  for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E;) {
    User *U = *UI++;

    if (LoadInst *LI = dyn_cast<LoadInst>(U)) {
      if (Init) {
        // Replace the load with the initializer.
        LI->replaceAllUsesWith(Init);
        LI->eraseFromParent();
        Changed = true;
      }
    } else if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
      // Store must be unreachable or storing Init into the global.
      SI->eraseFromParent();
      Changed = true;
    } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(U)) {
      if (CE->getOpcode() == Instruction::GetElementPtr) {
        Constant *SubInit = 0;
        if (Init)
          SubInit = ConstantFoldLoadThroughGEPConstantExpr(Init, CE);
        Changed |= CleanupConstantGlobalUsers(CE, SubInit);
      } else if (CE->getOpcode() == Instruction::BitCast && 
                 isa<PointerType>(CE->getType())) {
        // Pointer cast, delete any stores and memsets to the global.
        Changed |= CleanupConstantGlobalUsers(CE, 0);
      }

      if (CE->use_empty()) {
        CE->destroyConstant();
        Changed = true;
      }
    } else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(U)) {
      // Do not transform "gepinst (gep constexpr (GV))" here, because forming
      // "gepconstexpr (gep constexpr (GV))" will cause the two gep's to fold
      // and will invalidate our notion of what Init is.
      Constant *SubInit = 0;
      if (!isa<ConstantExpr>(GEP->getOperand(0))) {
        ConstantExpr *CE = 
          dyn_cast_or_null<ConstantExpr>(ConstantFoldInstruction(GEP));
        if (Init && CE && CE->getOpcode() == Instruction::GetElementPtr)
          SubInit = ConstantFoldLoadThroughGEPConstantExpr(Init, CE);
      }
      Changed |= CleanupConstantGlobalUsers(GEP, SubInit);

      if (GEP->use_empty()) {
        GEP->eraseFromParent();
        Changed = true;
      }
    } else if (MemIntrinsic *MI = dyn_cast<MemIntrinsic>(U)) { // memset/cpy/mv
      if (MI->getRawDest() == V) {
        MI->eraseFromParent();
        Changed = true;
      }

    } else if (Constant *C = dyn_cast<Constant>(U)) {
      // If we have a chain of dead constantexprs or other things dangling from
      // us, and if they are all dead, nuke them without remorse.
      if (ConstantIsDead(C)) {
        C->destroyConstant();
        // This could have invalidated UI, start over from scratch.
        CleanupConstantGlobalUsers(V, Init);
        return true;
      }
    }
  }
  return Changed;
}

/// isSafeSROAElementUse - Return true if the specified instruction is a safe
/// user of a derived expression from a global that we want to SROA.
static bool isSafeSROAElementUse(Value *V) {
  // We might have a dead and dangling constant hanging off of here.
  if (Constant *C = dyn_cast<Constant>(V))
    return ConstantIsDead(C);
  
  Instruction *I = dyn_cast<Instruction>(V);
  if (!I) return false;

  // Loads are ok.
  if (isa<LoadInst>(I)) return true;

  // Stores *to* the pointer are ok.
  if (StoreInst *SI = dyn_cast<StoreInst>(I))
    return SI->getOperand(0) != V;
    
  // Otherwise, it must be a GEP.
  GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(I);
  if (GEPI == 0) return false;
  
  if (GEPI->getNumOperands() < 3 || !isa<Constant>(GEPI->getOperand(1)) ||
      !cast<Constant>(GEPI->getOperand(1))->isNullValue())
    return false;
  
  for (Value::use_iterator I = GEPI->use_begin(), E = GEPI->use_end();
       I != E; ++I)
    if (!isSafeSROAElementUse(*I))
      return false;
  return true;
}


/// IsUserOfGlobalSafeForSRA - U is a direct user of the specified global value.
/// Look at it and its uses and decide whether it is safe to SROA this global.
///
static bool IsUserOfGlobalSafeForSRA(User *U, GlobalValue *GV) {
  // The user of the global must be a GEP Inst or a ConstantExpr GEP.
  if (!isa<GetElementPtrInst>(U) && 
      (!isa<ConstantExpr>(U) || 
       cast<ConstantExpr>(U)->getOpcode() != Instruction::GetElementPtr))
    return false;
  
  // Check to see if this ConstantExpr GEP is SRA'able.  In particular, we
  // don't like < 3 operand CE's, and we don't like non-constant integer
  // indices.  This enforces that all uses are 'gep GV, 0, C, ...' for some
  // value of C.
  if (U->getNumOperands() < 3 || !isa<Constant>(U->getOperand(1)) ||
      !cast<Constant>(U->getOperand(1))->isNullValue() ||
      !isa<ConstantInt>(U->getOperand(2)))
    return false;

  gep_type_iterator GEPI = gep_type_begin(U), E = gep_type_end(U);
  ++GEPI;  // Skip over the pointer index.
  
  // If this is a use of an array allocation, do a bit more checking for sanity.
  if (const ArrayType *AT = dyn_cast<ArrayType>(*GEPI)) {
    uint64_t NumElements = AT->getNumElements();
    ConstantInt *Idx = cast<ConstantInt>(U->getOperand(2));
    
    // Check to make sure that index falls within the array.  If not,
    // something funny is going on, so we won't do the optimization.
    //
    if (Idx->getZExtValue() >= NumElements)
      return false;
      
    // We cannot scalar repl this level of the array unless any array
    // sub-indices are in-range constants.  In particular, consider:
    // A[0][i].  We cannot know that the user isn't doing invalid things like
    // allowing i to index an out-of-range subscript that accesses A[1].
    //
    // Scalar replacing *just* the outer index of the array is probably not
    // going to be a win anyway, so just give up.
    for (++GEPI; // Skip array index.
         GEPI != E && (isa<ArrayType>(*GEPI) || isa<VectorType>(*GEPI));
         ++GEPI) {
      uint64_t NumElements;
      if (const ArrayType *SubArrayTy = dyn_cast<ArrayType>(*GEPI))
        NumElements = SubArrayTy->getNumElements();
      else
        NumElements = cast<VectorType>(*GEPI)->getNumElements();
      
      ConstantInt *IdxVal = dyn_cast<ConstantInt>(GEPI.getOperand());
      if (!IdxVal || IdxVal->getZExtValue() >= NumElements)
        return false;
    }
  }

  for (Value::use_iterator I = U->use_begin(), E = U->use_end(); I != E; ++I)
    if (!isSafeSROAElementUse(*I))
      return false;
  return true;
}

/// GlobalUsersSafeToSRA - Look at all uses of the global and decide whether it
/// is safe for us to perform this transformation.
///
static bool GlobalUsersSafeToSRA(GlobalValue *GV) {
  for (Value::use_iterator UI = GV->use_begin(), E = GV->use_end();
       UI != E; ++UI) {
    if (!IsUserOfGlobalSafeForSRA(*UI, GV))
      return false;
  }
  return true;
}
 

/// SRAGlobal - Perform scalar replacement of aggregates on the specified global
/// variable.  This opens the door for other optimizations by exposing the
/// behavior of the program in a more fine-grained way.  We have determined that
/// this transformation is safe already.  We return the first global variable we
/// insert so that the caller can reprocess it.
static GlobalVariable *SRAGlobal(GlobalVariable *GV, const TargetData &TD) {
  // Make sure this global only has simple uses that we can SRA.
  if (!GlobalUsersSafeToSRA(GV))
    return 0;
  
  assert(GV->hasInternalLinkage() && !GV->isConstant());
  Constant *Init = GV->getInitializer();
  const Type *Ty = Init->getType();

  std::vector<GlobalVariable*> NewGlobals;
  Module::GlobalListType &Globals = GV->getParent()->getGlobalList();

  // Get the alignment of the global, either explicit or target-specific.
  unsigned StartAlignment = GV->getAlignment();
  if (StartAlignment == 0)
    StartAlignment = TD.getABITypeAlignment(GV->getType());
   
  if (const StructType *STy = dyn_cast<StructType>(Ty)) {
    NewGlobals.reserve(STy->getNumElements());
    const StructLayout &Layout = *TD.getStructLayout(STy);
    for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i) {
      Constant *In = getAggregateConstantElement(Init,
                                            ConstantInt::get(Type::Int32Ty, i));
      assert(In && "Couldn't get element of initializer?");
      GlobalVariable *NGV = new GlobalVariable(STy->getElementType(i), false,
                                               GlobalVariable::InternalLinkage,
                                               In, GV->getName()+"."+utostr(i),
                                               (Module *)NULL,
                                               GV->isThreadLocal());
      Globals.insert(GV, NGV);
      NewGlobals.push_back(NGV);
      
      // Calculate the known alignment of the field.  If the original aggregate
      // had 256 byte alignment for example, something might depend on that:
      // propagate info to each field.
      uint64_t FieldOffset = Layout.getElementOffset(i);
      unsigned NewAlign = (unsigned)MinAlign(StartAlignment, FieldOffset);
      if (NewAlign > TD.getABITypeAlignment(STy->getElementType(i)))
        NGV->setAlignment(NewAlign);
    }
  } else if (const SequentialType *STy = dyn_cast<SequentialType>(Ty)) {
    unsigned NumElements = 0;
    if (const ArrayType *ATy = dyn_cast<ArrayType>(STy))
      NumElements = ATy->getNumElements();
    else
      NumElements = cast<VectorType>(STy)->getNumElements();

    if (NumElements > 16 && GV->hasNUsesOrMore(16))
      return 0; // It's not worth it.
    NewGlobals.reserve(NumElements);
    
    uint64_t EltSize = TD.getABITypeSize(STy->getElementType());
    unsigned EltAlign = TD.getABITypeAlignment(STy->getElementType());
    for (unsigned i = 0, e = NumElements; i != e; ++i) {
      Constant *In = getAggregateConstantElement(Init,
                                            ConstantInt::get(Type::Int32Ty, i));
      assert(In && "Couldn't get element of initializer?");

      GlobalVariable *NGV = new GlobalVariable(STy->getElementType(), false,
                                               GlobalVariable::InternalLinkage,
                                               In, GV->getName()+"."+utostr(i),
                                               (Module *)NULL,
                                               GV->isThreadLocal());
      Globals.insert(GV, NGV);
      NewGlobals.push_back(NGV);
      
      // Calculate the known alignment of the field.  If the original aggregate
      // had 256 byte alignment for example, something might depend on that:
      // propagate info to each field.
      unsigned NewAlign = (unsigned)MinAlign(StartAlignment, EltSize*i);
      if (NewAlign > EltAlign)
        NGV->setAlignment(NewAlign);
    }
  }

  if (NewGlobals.empty())
    return 0;

  DOUT << "PERFORMING GLOBAL SRA ON: " << *GV;

  Constant *NullInt = Constant::getNullValue(Type::Int32Ty);

  // Loop over all of the uses of the global, replacing the constantexpr geps,
  // with smaller constantexpr geps or direct references.
  while (!GV->use_empty()) {
    User *GEP = GV->use_back();
    assert(((isa<ConstantExpr>(GEP) &&
             cast<ConstantExpr>(GEP)->getOpcode()==Instruction::GetElementPtr)||
            isa<GetElementPtrInst>(GEP)) && "NonGEP CE's are not SRAable!");

    // Ignore the 1th operand, which has to be zero or else the program is quite
    // broken (undefined).  Get the 2nd operand, which is the structure or array
    // index.
    unsigned Val = cast<ConstantInt>(GEP->getOperand(2))->getZExtValue();
    if (Val >= NewGlobals.size()) Val = 0; // Out of bound array access.

    Value *NewPtr = NewGlobals[Val];

    // Form a shorter GEP if needed.
    if (GEP->getNumOperands() > 3) {
      if (ConstantExpr *CE = dyn_cast<ConstantExpr>(GEP)) {
        SmallVector<Constant*, 8> Idxs;
        Idxs.push_back(NullInt);
        for (unsigned i = 3, e = CE->getNumOperands(); i != e; ++i)
          Idxs.push_back(CE->getOperand(i));
        NewPtr = ConstantExpr::getGetElementPtr(cast<Constant>(NewPtr),
                                                &Idxs[0], Idxs.size());
      } else {
        GetElementPtrInst *GEPI = cast<GetElementPtrInst>(GEP);
        SmallVector<Value*, 8> Idxs;
        Idxs.push_back(NullInt);
        for (unsigned i = 3, e = GEPI->getNumOperands(); i != e; ++i)
          Idxs.push_back(GEPI->getOperand(i));
        NewPtr = GetElementPtrInst::Create(NewPtr, Idxs.begin(), Idxs.end(),
                                           GEPI->getName()+"."+utostr(Val), GEPI);
      }
    }
    GEP->replaceAllUsesWith(NewPtr);

    if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(GEP))
      GEPI->eraseFromParent();
    else
      cast<ConstantExpr>(GEP)->destroyConstant();
  }

  // Delete the old global, now that it is dead.
  Globals.erase(GV);
  ++NumSRA;

  // Loop over the new globals array deleting any globals that are obviously
  // dead.  This can arise due to scalarization of a structure or an array that
  // has elements that are dead.
  unsigned FirstGlobal = 0;
  for (unsigned i = 0, e = NewGlobals.size(); i != e; ++i)
    if (NewGlobals[i]->use_empty()) {
      Globals.erase(NewGlobals[i]);
      if (FirstGlobal == i) ++FirstGlobal;
    }

  return FirstGlobal != NewGlobals.size() ? NewGlobals[FirstGlobal] : 0;
}

/// AllUsesOfValueWillTrapIfNull - Return true if all users of the specified
/// value will trap if the value is dynamically null.  PHIs keeps track of any 
/// phi nodes we've seen to avoid reprocessing them.
static bool AllUsesOfValueWillTrapIfNull(Value *V,
                                         SmallPtrSet<PHINode*, 8> &PHIs) {
  for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E; ++UI)
    if (isa<LoadInst>(*UI)) {
      // Will trap.
    } else if (StoreInst *SI = dyn_cast<StoreInst>(*UI)) {
      if (SI->getOperand(0) == V) {
        //cerr << "NONTRAPPING USE: " << **UI;
        return false;  // Storing the value.
      }
    } else if (CallInst *CI = dyn_cast<CallInst>(*UI)) {
      if (CI->getOperand(0) != V) {
        //cerr << "NONTRAPPING USE: " << **UI;
        return false;  // Not calling the ptr
      }
    } else if (InvokeInst *II = dyn_cast<InvokeInst>(*UI)) {
      if (II->getOperand(0) != V) {
        //cerr << "NONTRAPPING USE: " << **UI;
        return false;  // Not calling the ptr
      }
    } else if (BitCastInst *CI = dyn_cast<BitCastInst>(*UI)) {
      if (!AllUsesOfValueWillTrapIfNull(CI, PHIs)) return false;
    } else if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(*UI)) {
      if (!AllUsesOfValueWillTrapIfNull(GEPI, PHIs)) return false;
    } else if (PHINode *PN = dyn_cast<PHINode>(*UI)) {
      // If we've already seen this phi node, ignore it, it has already been
      // checked.
      if (PHIs.insert(PN))
        return AllUsesOfValueWillTrapIfNull(PN, PHIs);
    } else if (isa<ICmpInst>(*UI) &&
               isa<ConstantPointerNull>(UI->getOperand(1))) {
      // Ignore setcc X, null
    } else {
      //cerr << "NONTRAPPING USE: " << **UI;
      return false;
    }
  return true;
}

/// AllUsesOfLoadedValueWillTrapIfNull - Return true if all uses of any loads
/// from GV will trap if the loaded value is null.  Note that this also permits
/// comparisons of the loaded value against null, as a special case.
static bool AllUsesOfLoadedValueWillTrapIfNull(GlobalVariable *GV) {
  for (Value::use_iterator UI = GV->use_begin(), E = GV->use_end(); UI!=E; ++UI)
    if (LoadInst *LI = dyn_cast<LoadInst>(*UI)) {
      SmallPtrSet<PHINode*, 8> PHIs;
      if (!AllUsesOfValueWillTrapIfNull(LI, PHIs))
        return false;
    } else if (isa<StoreInst>(*UI)) {
      // Ignore stores to the global.
    } else {
      // We don't know or understand this user, bail out.
      //cerr << "UNKNOWN USER OF GLOBAL!: " << **UI;
      return false;
    }

  return true;
}

static bool OptimizeAwayTrappingUsesOfValue(Value *V, Constant *NewV) {
  bool Changed = false;
  for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E; ) {
    Instruction *I = cast<Instruction>(*UI++);
    if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
      LI->setOperand(0, NewV);
      Changed = true;
    } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
      if (SI->getOperand(1) == V) {
        SI->setOperand(1, NewV);
        Changed = true;
      }
    } else if (isa<CallInst>(I) || isa<InvokeInst>(I)) {
      if (I->getOperand(0) == V) {
        // Calling through the pointer!  Turn into a direct call, but be careful
        // that the pointer is not also being passed as an argument.
        I->setOperand(0, NewV);
        Changed = true;
        bool PassedAsArg = false;
        for (unsigned i = 1, e = I->getNumOperands(); i != e; ++i)
          if (I->getOperand(i) == V) {
            PassedAsArg = true;
            I->setOperand(i, NewV);
          }

        if (PassedAsArg) {
          // Being passed as an argument also.  Be careful to not invalidate UI!
          UI = V->use_begin();
        }
      }
    } else if (CastInst *CI = dyn_cast<CastInst>(I)) {
      Changed |= OptimizeAwayTrappingUsesOfValue(CI,
                                ConstantExpr::getCast(CI->getOpcode(),
                                                      NewV, CI->getType()));
      if (CI->use_empty()) {
        Changed = true;
        CI->eraseFromParent();
      }
    } else if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(I)) {
      // Should handle GEP here.
      SmallVector<Constant*, 8> Idxs;
      Idxs.reserve(GEPI->getNumOperands()-1);
      for (unsigned i = 1, e = GEPI->getNumOperands(); i != e; ++i)
        if (Constant *C = dyn_cast<Constant>(GEPI->getOperand(i)))
          Idxs.push_back(C);
        else
          break;
      if (Idxs.size() == GEPI->getNumOperands()-1)
        Changed |= OptimizeAwayTrappingUsesOfValue(GEPI,
                                ConstantExpr::getGetElementPtr(NewV, &Idxs[0],
                                                               Idxs.size()));
      if (GEPI->use_empty()) {
        Changed = true;
        GEPI->eraseFromParent();
      }
    }
  }

  return Changed;
}


/// OptimizeAwayTrappingUsesOfLoads - The specified global has only one non-null
/// value stored into it.  If there are uses of the loaded value that would trap
/// if the loaded value is dynamically null, then we know that they cannot be
/// reachable with a null optimize away the load.
static bool OptimizeAwayTrappingUsesOfLoads(GlobalVariable *GV, Constant *LV) {
  std::vector<LoadInst*> Loads;
  bool Changed = false;

  // Replace all uses of loads with uses of uses of the stored value.
  for (Value::use_iterator GUI = GV->use_begin(), E = GV->use_end();
       GUI != E; ++GUI)
    if (LoadInst *LI = dyn_cast<LoadInst>(*GUI)) {
      Loads.push_back(LI);
      Changed |= OptimizeAwayTrappingUsesOfValue(LI, LV);
    } else {
      // If we get here we could have stores, selects, or phi nodes whose values
      // are loaded.
      assert((isa<StoreInst>(*GUI) || isa<PHINode>(*GUI) ||
              isa<SelectInst>(*GUI) || isa<ConstantExpr>(*GUI)) &&
             "Only expect load and stores!");
    }

  if (Changed) {
    DOUT << "OPTIMIZED LOADS FROM STORED ONCE POINTER: " << *GV;
    ++NumGlobUses;
  }

  // Delete all of the loads we can, keeping track of whether we nuked them all!
  bool AllLoadsGone = true;
  while (!Loads.empty()) {
    LoadInst *L = Loads.back();
    if (L->use_empty()) {
      L->eraseFromParent();
      Changed = true;
    } else {
      AllLoadsGone = false;
    }
    Loads.pop_back();
  }

  // If we nuked all of the loads, then none of the stores are needed either,
  // nor is the global.
  if (AllLoadsGone) {
    DOUT << "  *** GLOBAL NOW DEAD!\n";
    CleanupConstantGlobalUsers(GV, 0);
    if (GV->use_empty()) {
      GV->eraseFromParent();
      ++NumDeleted;
    }
    Changed = true;
  }
  return Changed;
}

/// ConstantPropUsersOf - Walk the use list of V, constant folding all of the
/// instructions that are foldable.
static void ConstantPropUsersOf(Value *V) {
  for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E; )
    if (Instruction *I = dyn_cast<Instruction>(*UI++))
      if (Constant *NewC = ConstantFoldInstruction(I)) {
        I->replaceAllUsesWith(NewC);

        // Advance UI to the next non-I use to avoid invalidating it!
        // Instructions could multiply use V.
        while (UI != E && *UI == I)
          ++UI;
        I->eraseFromParent();
      }
}

/// OptimizeGlobalAddressOfMalloc - This function takes the specified global
/// variable, and transforms the program as if it always contained the result of
/// the specified malloc.  Because it is always the result of the specified
/// malloc, there is no reason to actually DO the malloc.  Instead, turn the
/// malloc into a global, and any loads of GV as uses of the new global.
static GlobalVariable *OptimizeGlobalAddressOfMalloc(GlobalVariable *GV,
                                                     MallocInst *MI) {
  DOUT << "PROMOTING MALLOC GLOBAL: " << *GV << "  MALLOC = " << *MI;
  ConstantInt *NElements = cast<ConstantInt>(MI->getArraySize());

  if (NElements->getZExtValue() != 1) {
    // If we have an array allocation, transform it to a single element
    // allocation to make the code below simpler.
    Type *NewTy = ArrayType::get(MI->getAllocatedType(),
                                 NElements->getZExtValue());
    MallocInst *NewMI =
      new MallocInst(NewTy, Constant::getNullValue(Type::Int32Ty),
                     MI->getAlignment(), MI->getName(), MI);
    Value* Indices[2];
    Indices[0] = Indices[1] = Constant::getNullValue(Type::Int32Ty);
    Value *NewGEP = GetElementPtrInst::Create(NewMI, Indices, Indices + 2,
                                              NewMI->getName()+".el0", MI);
    MI->replaceAllUsesWith(NewGEP);
    MI->eraseFromParent();
    MI = NewMI;
  }

  // Create the new global variable.  The contents of the malloc'd memory is
  // undefined, so initialize with an undef value.
  Constant *Init = UndefValue::get(MI->getAllocatedType());
  GlobalVariable *NewGV = new GlobalVariable(MI->getAllocatedType(), false,
                                             GlobalValue::InternalLinkage, Init,
                                             GV->getName()+".body",
                                             (Module *)NULL,
                                             GV->isThreadLocal());
  // FIXME: This new global should have the alignment returned by malloc.  Code
  // could depend on malloc returning large alignment (on the mac, 16 bytes) but
  // this would only guarantee some lower alignment.
  GV->getParent()->getGlobalList().insert(GV, NewGV);

  // Anything that used the malloc now uses the global directly.
  MI->replaceAllUsesWith(NewGV);

  Constant *RepValue = NewGV;
  if (NewGV->getType() != GV->getType()->getElementType())
    RepValue = ConstantExpr::getBitCast(RepValue, 
                                        GV->getType()->getElementType());

  // If there is a comparison against null, we will insert a global bool to
  // keep track of whether the global was initialized yet or not.
  GlobalVariable *InitBool =
    new GlobalVariable(Type::Int1Ty, false, GlobalValue::InternalLinkage,
                       ConstantInt::getFalse(), GV->getName()+".init",
                       (Module *)NULL, GV->isThreadLocal());
  bool InitBoolUsed = false;

  // Loop over all uses of GV, processing them in turn.
  std::vector<StoreInst*> Stores;
  while (!GV->use_empty())
    if (LoadInst *LI = dyn_cast<LoadInst>(GV->use_back())) {
      while (!LI->use_empty()) {
        Use &LoadUse = LI->use_begin().getUse();
        if (!isa<ICmpInst>(LoadUse.getUser()))
          LoadUse = RepValue;
        else {
          ICmpInst *CI = cast<ICmpInst>(LoadUse.getUser());
          // Replace the cmp X, 0 with a use of the bool value.
          Value *LV = new LoadInst(InitBool, InitBool->getName()+".val", CI);
          InitBoolUsed = true;
          switch (CI->getPredicate()) {
          default: assert(0 && "Unknown ICmp Predicate!");
          case ICmpInst::ICMP_ULT:
          case ICmpInst::ICMP_SLT:
            LV = ConstantInt::getFalse();   // X < null -> always false
            break;
          case ICmpInst::ICMP_ULE:
          case ICmpInst::ICMP_SLE:
          case ICmpInst::ICMP_EQ:
            LV = BinaryOperator::createNot(LV, "notinit", CI);
            break;
          case ICmpInst::ICMP_NE:
          case ICmpInst::ICMP_UGE:
          case ICmpInst::ICMP_SGE:
          case ICmpInst::ICMP_UGT:
          case ICmpInst::ICMP_SGT:
            break;  // no change.
          }
          CI->replaceAllUsesWith(LV);
          CI->eraseFromParent();
        }
      }
      LI->eraseFromParent();
    } else {
      StoreInst *SI = cast<StoreInst>(GV->use_back());
      // The global is initialized when the store to it occurs.
      new StoreInst(ConstantInt::getTrue(), InitBool, SI);
      SI->eraseFromParent();
    }

  // If the initialization boolean was used, insert it, otherwise delete it.
  if (!InitBoolUsed) {
    while (!InitBool->use_empty())  // Delete initializations
      cast<Instruction>(InitBool->use_back())->eraseFromParent();
    delete InitBool;
  } else
    GV->getParent()->getGlobalList().insert(GV, InitBool);


  // Now the GV is dead, nuke it and the malloc.
  GV->eraseFromParent();
  MI->eraseFromParent();

  // To further other optimizations, loop over all users of NewGV and try to
  // constant prop them.  This will promote GEP instructions with constant
  // indices into GEP constant-exprs, which will allow global-opt to hack on it.
  ConstantPropUsersOf(NewGV);
  if (RepValue != NewGV)
    ConstantPropUsersOf(RepValue);

  return NewGV;
}

/// ValueIsOnlyUsedLocallyOrStoredToOneGlobal - Scan the use-list of V checking
/// to make sure that there are no complex uses of V.  We permit simple things
/// like dereferencing the pointer, but not storing through the address, unless
/// it is to the specified global.
static bool ValueIsOnlyUsedLocallyOrStoredToOneGlobal(Instruction *V,
                                                      GlobalVariable *GV,
                                              SmallPtrSet<PHINode*, 8> &PHIs) {
  for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E; ++UI)
    if (isa<LoadInst>(*UI) || isa<CmpInst>(*UI)) {
      // Fine, ignore.
    } else if (StoreInst *SI = dyn_cast<StoreInst>(*UI)) {
      if (SI->getOperand(0) == V && SI->getOperand(1) != GV)
        return false;  // Storing the pointer itself... bad.
      // Otherwise, storing through it, or storing into GV... fine.
    } else if (isa<GetElementPtrInst>(*UI)) {
      if (!ValueIsOnlyUsedLocallyOrStoredToOneGlobal(cast<Instruction>(*UI),
                                                     GV, PHIs))
        return false;
    } else if (PHINode *PN = dyn_cast<PHINode>(*UI)) {
      // PHIs are ok if all uses are ok.  Don't infinitely recurse through PHI
      // cycles.
      if (PHIs.insert(PN))
        if (!ValueIsOnlyUsedLocallyOrStoredToOneGlobal(PN, GV, PHIs))
          return false;
    } else {
      return false;
    }
  return true;
}

/// ReplaceUsesOfMallocWithGlobal - The Alloc pointer is stored into GV
/// somewhere.  Transform all uses of the allocation into loads from the
/// global and uses of the resultant pointer.  Further, delete the store into
/// GV.  This assumes that these value pass the 
/// 'ValueIsOnlyUsedLocallyOrStoredToOneGlobal' predicate.
static void ReplaceUsesOfMallocWithGlobal(Instruction *Alloc, 
                                          GlobalVariable *GV) {
  while (!Alloc->use_empty()) {
    Instruction *U = cast<Instruction>(*Alloc->use_begin());
    Instruction *InsertPt = U;
    if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
      // If this is the store of the allocation into the global, remove it.
      if (SI->getOperand(1) == GV) {
        SI->eraseFromParent();
        continue;
      }
    } else if (PHINode *PN = dyn_cast<PHINode>(U)) {
      // Insert the load in the corresponding predecessor, not right before the
      // PHI.
      unsigned PredNo = Alloc->use_begin().getOperandNo()/2;
      InsertPt = PN->getIncomingBlock(PredNo)->getTerminator();
    }
    
    // Insert a load from the global, and use it instead of the malloc.
    Value *NL = new LoadInst(GV, GV->getName()+".val", InsertPt);
    U->replaceUsesOfWith(Alloc, NL);
  }
}

/// GlobalLoadUsesSimpleEnoughForHeapSRA - If all users of values loaded from
/// GV are simple enough to perform HeapSRA, return true.
static bool GlobalLoadUsesSimpleEnoughForHeapSRA(GlobalVariable *GV,
                                                 MallocInst *MI) {
  for (Value::use_iterator UI = GV->use_begin(), E = GV->use_end(); UI != E; 
       ++UI)
    if (LoadInst *LI = dyn_cast<LoadInst>(*UI)) {
      // We permit two users of the load: setcc comparing against the null
      // pointer, and a getelementptr of a specific form.
      for (Value::use_iterator UI = LI->use_begin(), E = LI->use_end(); UI != E; 
           ++UI) {
        // Comparison against null is ok.
        if (ICmpInst *ICI = dyn_cast<ICmpInst>(*UI)) {
          if (!isa<ConstantPointerNull>(ICI->getOperand(1)))
            return false;
          continue;
        }
        
        // getelementptr is also ok, but only a simple form.
        if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(*UI)) {
          // Must index into the array and into the struct.
          if (GEPI->getNumOperands() < 3)
            return false;
          
          // Otherwise the GEP is ok.
          continue;
        }
        
        if (PHINode *PN = dyn_cast<PHINode>(*UI)) {
          // We have a phi of a load from the global.  We can only handle this
          // if the other PHI'd values are actually the same.  In this case,
          // the rewriter will just drop the phi entirely.
          for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
            Value *IV = PN->getIncomingValue(i);
            if (IV == LI) continue;  // Trivial the same.
            
            // If the phi'd value is from the malloc that initializes the value,
            // we can xform it.
            if (IV == MI) continue;
            
            // Otherwise, we don't know what it is.
            return false;
          }
          return true;
        }
        
        // Otherwise we don't know what this is, not ok.
        return false;
      }
    }
  return true;
}

/// GetHeapSROALoad - Return the load for the specified field of the HeapSROA'd
/// value, lazily creating it on demand.
static Value *GetHeapSROALoad(Instruction *Load, unsigned FieldNo,
                              const std::vector<GlobalVariable*> &FieldGlobals,
                              std::vector<Value *> &InsertedLoadsForPtr) {
  if (InsertedLoadsForPtr.size() <= FieldNo)
    InsertedLoadsForPtr.resize(FieldNo+1);
  if (InsertedLoadsForPtr[FieldNo] == 0)
    InsertedLoadsForPtr[FieldNo] = new LoadInst(FieldGlobals[FieldNo],
                                                Load->getName()+".f" + 
                                                utostr(FieldNo), Load);
  return InsertedLoadsForPtr[FieldNo];
}

/// RewriteHeapSROALoadUser - Given a load instruction and a value derived from
/// the load, rewrite the derived value to use the HeapSRoA'd load.
static void RewriteHeapSROALoadUser(LoadInst *Load, Instruction *LoadUser, 
                               const std::vector<GlobalVariable*> &FieldGlobals,
                                    std::vector<Value *> &InsertedLoadsForPtr) {
  // If this is a comparison against null, handle it.
  if (ICmpInst *SCI = dyn_cast<ICmpInst>(LoadUser)) {
    assert(isa<ConstantPointerNull>(SCI->getOperand(1)));
    // If we have a setcc of the loaded pointer, we can use a setcc of any
    // field.
    Value *NPtr;
    if (InsertedLoadsForPtr.empty()) {
      NPtr = GetHeapSROALoad(Load, 0, FieldGlobals, InsertedLoadsForPtr);
    } else {
      NPtr = InsertedLoadsForPtr.back();
    }
    
    Value *New = new ICmpInst(SCI->getPredicate(), NPtr,
                              Constant::getNullValue(NPtr->getType()),
                              SCI->getName(), SCI);
    SCI->replaceAllUsesWith(New);
    SCI->eraseFromParent();
    return;
  }
  
  // Handle 'getelementptr Ptr, Idx, uint FieldNo ...'
  if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(LoadUser)) {
    assert(GEPI->getNumOperands() >= 3 && isa<ConstantInt>(GEPI->getOperand(2))
           && "Unexpected GEPI!");
  
    // Load the pointer for this field.
    unsigned FieldNo = cast<ConstantInt>(GEPI->getOperand(2))->getZExtValue();
    Value *NewPtr = GetHeapSROALoad(Load, FieldNo,
                                    FieldGlobals, InsertedLoadsForPtr);
    
    // Create the new GEP idx vector.
    SmallVector<Value*, 8> GEPIdx;
    GEPIdx.push_back(GEPI->getOperand(1));
    GEPIdx.append(GEPI->op_begin()+3, GEPI->op_end());
    
    Value *NGEPI = GetElementPtrInst::Create(NewPtr, GEPIdx.begin(), GEPIdx.end(),
                                             GEPI->getName(), GEPI);
    GEPI->replaceAllUsesWith(NGEPI);
    GEPI->eraseFromParent();
    return;
  }
  
  // Handle PHI nodes.  PHI nodes must be merging in the same values, plus
  // potentially the original malloc.  Insert phi nodes for each field, then
  // process uses of the PHI.
  PHINode *PN = cast<PHINode>(LoadUser);
  std::vector<Value *> PHIsForField;
  PHIsForField.resize(FieldGlobals.size());
  for (unsigned i = 0, e = FieldGlobals.size(); i != e; ++i) {
    Value *LoadV = GetHeapSROALoad(Load, i, FieldGlobals, InsertedLoadsForPtr);

    PHINode *FieldPN = PHINode::Create(LoadV->getType(),
                                       PN->getName()+"."+utostr(i), PN);
    // Fill in the predecessor values.
    for (unsigned pred = 0, e = PN->getNumIncomingValues(); pred != e; ++pred) {
      // Each predecessor either uses the load or the original malloc.
      Value *InVal = PN->getIncomingValue(pred);
      BasicBlock *BB = PN->getIncomingBlock(pred);
      Value *NewVal;
      if (isa<MallocInst>(InVal)) {
        // Insert a reload from the global in the predecessor.
        NewVal = GetHeapSROALoad(BB->getTerminator(), i, FieldGlobals,
                                 PHIsForField);
      } else {
        NewVal = InsertedLoadsForPtr[i];
      }
      FieldPN->addIncoming(NewVal, BB);
    }
    PHIsForField[i] = FieldPN;
  }
  
  // Since PHIsForField specifies a phi for every input value, the lazy inserter
  // will never insert a load.
  while (!PN->use_empty())
    RewriteHeapSROALoadUser(Load, PN->use_back(), FieldGlobals, PHIsForField);
  PN->eraseFromParent();
}

/// RewriteUsesOfLoadForHeapSRoA - We are performing Heap SRoA on a global.  Ptr
/// is a value loaded from the global.  Eliminate all uses of Ptr, making them
/// use FieldGlobals instead.  All uses of loaded values satisfy
/// GlobalLoadUsesSimpleEnoughForHeapSRA.
static void RewriteUsesOfLoadForHeapSRoA(LoadInst *Load, 
                             const std::vector<GlobalVariable*> &FieldGlobals) {
  std::vector<Value *> InsertedLoadsForPtr;
  //InsertedLoadsForPtr.resize(FieldGlobals.size());
  while (!Load->use_empty())
    RewriteHeapSROALoadUser(Load, Load->use_back(), 
                            FieldGlobals, InsertedLoadsForPtr);
}

/// PerformHeapAllocSRoA - MI is an allocation of an array of structures.  Break
/// it up into multiple allocations of arrays of the fields.
static GlobalVariable *PerformHeapAllocSRoA(GlobalVariable *GV, MallocInst *MI){
  DOUT << "SROA HEAP ALLOC: " << *GV << "  MALLOC = " << *MI;
  const StructType *STy = cast<StructType>(MI->getAllocatedType());

  // There is guaranteed to be at least one use of the malloc (storing
  // it into GV).  If there are other uses, change them to be uses of
  // the global to simplify later code.  This also deletes the store
  // into GV.
  ReplaceUsesOfMallocWithGlobal(MI, GV);
  
  // Okay, at this point, there are no users of the malloc.  Insert N
  // new mallocs at the same place as MI, and N globals.
  std::vector<GlobalVariable*> FieldGlobals;
  std::vector<MallocInst*> FieldMallocs;
  
  for (unsigned FieldNo = 0, e = STy->getNumElements(); FieldNo != e;++FieldNo){
    const Type *FieldTy = STy->getElementType(FieldNo);
    const Type *PFieldTy = PointerType::getUnqual(FieldTy);
    
    GlobalVariable *NGV =
      new GlobalVariable(PFieldTy, false, GlobalValue::InternalLinkage,
                         Constant::getNullValue(PFieldTy),
                         GV->getName() + ".f" + utostr(FieldNo), GV,
                         GV->isThreadLocal());
    FieldGlobals.push_back(NGV);
    
    MallocInst *NMI = new MallocInst(FieldTy, MI->getArraySize(),
                                     MI->getName() + ".f" + utostr(FieldNo),MI);
    FieldMallocs.push_back(NMI);
    new StoreInst(NMI, NGV, MI);
  }
  
  // The tricky aspect of this transformation is handling the case when malloc
  // fails.  In the original code, malloc failing would set the result pointer
  // of malloc to null.  In this case, some mallocs could succeed and others
  // could fail.  As such, we emit code that looks like this:
  //    F0 = malloc(field0)
  //    F1 = malloc(field1)
  //    F2 = malloc(field2)
  //    if (F0 == 0 || F1 == 0 || F2 == 0) {
  //      if (F0) { free(F0); F0 = 0; }
  //      if (F1) { free(F1); F1 = 0; }
  //      if (F2) { free(F2); F2 = 0; }
  //    }
  Value *RunningOr = 0;
  for (unsigned i = 0, e = FieldMallocs.size(); i != e; ++i) {
    Value *Cond = new ICmpInst(ICmpInst::ICMP_EQ, FieldMallocs[i],
                             Constant::getNullValue(FieldMallocs[i]->getType()),
                                  "isnull", MI);
    if (!RunningOr)
      RunningOr = Cond;   // First seteq
    else
      RunningOr = BinaryOperator::createOr(RunningOr, Cond, "tmp", MI);
  }

  // Split the basic block at the old malloc.
  BasicBlock *OrigBB = MI->getParent();
  BasicBlock *ContBB = OrigBB->splitBasicBlock(MI, "malloc_cont");
  
  // Create the block to check the first condition.  Put all these blocks at the
  // end of the function as they are unlikely to be executed.
  BasicBlock *NullPtrBlock = BasicBlock::Create("malloc_ret_null",
                                                OrigBB->getParent());
  
  // Remove the uncond branch from OrigBB to ContBB, turning it into a cond
  // branch on RunningOr.
  OrigBB->getTerminator()->eraseFromParent();
  BranchInst::Create(NullPtrBlock, ContBB, RunningOr, OrigBB);
  
  // Within the NullPtrBlock, we need to emit a comparison and branch for each
  // pointer, because some may be null while others are not.
  for (unsigned i = 0, e = FieldGlobals.size(); i != e; ++i) {
    Value *GVVal = new LoadInst(FieldGlobals[i], "tmp", NullPtrBlock);
    Value *Cmp = new ICmpInst(ICmpInst::ICMP_NE, GVVal, 
                              Constant::getNullValue(GVVal->getType()),
                              "tmp", NullPtrBlock);
    BasicBlock *FreeBlock = BasicBlock::Create("free_it", OrigBB->getParent());
    BasicBlock *NextBlock = BasicBlock::Create("next", OrigBB->getParent());
    BranchInst::Create(FreeBlock, NextBlock, Cmp, NullPtrBlock);

    // Fill in FreeBlock.
    new FreeInst(GVVal, FreeBlock);
    new StoreInst(Constant::getNullValue(GVVal->getType()), FieldGlobals[i],
                  FreeBlock);
    BranchInst::Create(NextBlock, FreeBlock);
    
    NullPtrBlock = NextBlock;
  }
  
  BranchInst::Create(ContBB, NullPtrBlock);
  
  // MI is no longer needed, remove it.
  MI->eraseFromParent();

  
  // Okay, the malloc site is completely handled.  All of the uses of GV are now
  // loads, and all uses of those loads are simple.  Rewrite them to use loads
  // of the per-field globals instead.
  while (!GV->use_empty()) {
    if (LoadInst *LI = dyn_cast<LoadInst>(GV->use_back())) {
      RewriteUsesOfLoadForHeapSRoA(LI, FieldGlobals);
      LI->eraseFromParent();
    } else {
      // Must be a store of null.
      StoreInst *SI = cast<StoreInst>(GV->use_back());
      assert(isa<Constant>(SI->getOperand(0)) &&
             cast<Constant>(SI->getOperand(0))->isNullValue() &&
             "Unexpected heap-sra user!");
      
      // Insert a store of null into each global.
      for (unsigned i = 0, e = FieldGlobals.size(); i != e; ++i) {
        Constant *Null = 
          Constant::getNullValue(FieldGlobals[i]->getType()->getElementType());
        new StoreInst(Null, FieldGlobals[i], SI);
      }
      // Erase the original store.
      SI->eraseFromParent();
    }
  }

  // The old global is now dead, remove it.
  GV->eraseFromParent();

  ++NumHeapSRA;
  return FieldGlobals[0];
}


// OptimizeOnceStoredGlobal - Try to optimize globals based on the knowledge
// that only one value (besides its initializer) is ever stored to the global.
static bool OptimizeOnceStoredGlobal(GlobalVariable *GV, Value *StoredOnceVal,
                                     Module::global_iterator &GVI,
                                     TargetData &TD) {
  if (CastInst *CI = dyn_cast<CastInst>(StoredOnceVal))
    StoredOnceVal = CI->getOperand(0);
  else if (GetElementPtrInst *GEPI =dyn_cast<GetElementPtrInst>(StoredOnceVal)){
    // "getelementptr Ptr, 0, 0, 0" is really just a cast.
    bool IsJustACast = true;
    for (unsigned i = 1, e = GEPI->getNumOperands(); i != e; ++i)
      if (!isa<Constant>(GEPI->getOperand(i)) ||
          !cast<Constant>(GEPI->getOperand(i))->isNullValue()) {
        IsJustACast = false;
        break;
      }
    if (IsJustACast)
      StoredOnceVal = GEPI->getOperand(0);
  }

  // If we are dealing with a pointer global that is initialized to null and
  // only has one (non-null) value stored into it, then we can optimize any
  // users of the loaded value (often calls and loads) that would trap if the
  // value was null.
  if (isa<PointerType>(GV->getInitializer()->getType()) &&
      GV->getInitializer()->isNullValue()) {
    if (Constant *SOVC = dyn_cast<Constant>(StoredOnceVal)) {
      if (GV->getInitializer()->getType() != SOVC->getType())
        SOVC = ConstantExpr::getBitCast(SOVC, GV->getInitializer()->getType());

      // Optimize away any trapping uses of the loaded value.
      if (OptimizeAwayTrappingUsesOfLoads(GV, SOVC))
        return true;
    } else if (MallocInst *MI = dyn_cast<MallocInst>(StoredOnceVal)) {
      // If this is a malloc of an abstract type, don't touch it.
      if (!MI->getAllocatedType()->isSized())
        return false;
      
      // We can't optimize this global unless all uses of it are *known* to be
      // of the malloc value, not of the null initializer value (consider a use
      // that compares the global's value against zero to see if the malloc has
      // been reached).  To do this, we check to see if all uses of the global
      // would trap if the global were null: this proves that they must all
      // happen after the malloc.
      if (!AllUsesOfLoadedValueWillTrapIfNull(GV))
        return false;

      // We can't optimize this if the malloc itself is used in a complex way,
      // for example, being stored into multiple globals.  This allows the
      // malloc to be stored into the specified global, loaded setcc'd, and
      // GEP'd.  These are all things we could transform to using the global
      // for.
      {
        SmallPtrSet<PHINode*, 8> PHIs;
        if (!ValueIsOnlyUsedLocallyOrStoredToOneGlobal(MI, GV, PHIs))
          return false;
      }

      
      // If we have a global that is only initialized with a fixed size malloc,
      // transform the program to use global memory instead of malloc'd memory.
      // This eliminates dynamic allocation, avoids an indirection accessing the
      // data, and exposes the resultant global to further GlobalOpt.
      if (ConstantInt *NElements = dyn_cast<ConstantInt>(MI->getArraySize())) {
        // Restrict this transformation to only working on small allocations
        // (2048 bytes currently), as we don't want to introduce a 16M global or
        // something.
        if (NElements->getZExtValue()*
                     TD.getABITypeSize(MI->getAllocatedType()) < 2048) {
          GVI = OptimizeGlobalAddressOfMalloc(GV, MI);
          return true;
        }
      }

      // If the allocation is an array of structures, consider transforming this
      // into multiple malloc'd arrays, one for each field.  This is basically
      // SRoA for malloc'd memory.
      if (const StructType *AllocTy = 
                  dyn_cast<StructType>(MI->getAllocatedType())) {
        // This the structure has an unreasonable number of fields, leave it
        // alone.
        if (AllocTy->getNumElements() <= 16 && AllocTy->getNumElements() > 0 &&
            GlobalLoadUsesSimpleEnoughForHeapSRA(GV, MI)) {
          GVI = PerformHeapAllocSRoA(GV, MI);
          return true;
        }
      }
    }
  }

  return false;
}

/// TryToShrinkGlobalToBoolean - At this point, we have learned that the only
/// two values ever stored into GV are its initializer and OtherVal.  See if we
/// can shrink the global into a boolean and select between the two values
/// whenever it is used.  This exposes the values to other scalar optimizations.
static bool TryToShrinkGlobalToBoolean(GlobalVariable *GV, Constant *OtherVal) {
  const Type *GVElType = GV->getType()->getElementType();
  
  // If GVElType is already i1, it is already shrunk.  If the type of the GV is
  // an FP value or vector, don't do this optimization because a select between
  // them is very expensive and unlikely to lead to later simplification.
  if (GVElType == Type::Int1Ty || GVElType->isFloatingPoint() ||
      isa<VectorType>(GVElType))
    return false;
  
  // Walk the use list of the global seeing if all the uses are load or store.
  // If there is anything else, bail out.
  for (Value::use_iterator I = GV->use_begin(), E = GV->use_end(); I != E; ++I)
    if (!isa<LoadInst>(I) && !isa<StoreInst>(I))
      return false;
  
  DOUT << "   *** SHRINKING TO BOOL: " << *GV;
  
  // Create the new global, initializing it to false.
  GlobalVariable *NewGV = new GlobalVariable(Type::Int1Ty, false,
         GlobalValue::InternalLinkage, ConstantInt::getFalse(),
                                             GV->getName()+".b",
                                             (Module *)NULL,
                                             GV->isThreadLocal());
  GV->getParent()->getGlobalList().insert(GV, NewGV);

  Constant *InitVal = GV->getInitializer();
  assert(InitVal->getType() != Type::Int1Ty && "No reason to shrink to bool!");

  // If initialized to zero and storing one into the global, we can use a cast
  // instead of a select to synthesize the desired value.
  bool IsOneZero = false;
  if (ConstantInt *CI = dyn_cast<ConstantInt>(OtherVal))
    IsOneZero = InitVal->isNullValue() && CI->isOne();

  while (!GV->use_empty()) {
    Instruction *UI = cast<Instruction>(GV->use_back());
    if (StoreInst *SI = dyn_cast<StoreInst>(UI)) {
      // Change the store into a boolean store.
      bool StoringOther = SI->getOperand(0) == OtherVal;
      // Only do this if we weren't storing a loaded value.
      Value *StoreVal;
      if (StoringOther || SI->getOperand(0) == InitVal)
        StoreVal = ConstantInt::get(Type::Int1Ty, StoringOther);
      else {
        // Otherwise, we are storing a previously loaded copy.  To do this,
        // change the copy from copying the original value to just copying the
        // bool.
        Instruction *StoredVal = cast<Instruction>(SI->getOperand(0));

        // If we're already replaced the input, StoredVal will be a cast or
        // select instruction.  If not, it will be a load of the original
        // global.
        if (LoadInst *LI = dyn_cast<LoadInst>(StoredVal)) {
          assert(LI->getOperand(0) == GV && "Not a copy!");
          // Insert a new load, to preserve the saved value.
          StoreVal = new LoadInst(NewGV, LI->getName()+".b", LI);
        } else {
          assert((isa<CastInst>(StoredVal) || isa<SelectInst>(StoredVal)) &&
                 "This is not a form that we understand!");
          StoreVal = StoredVal->getOperand(0);
          assert(isa<LoadInst>(StoreVal) && "Not a load of NewGV!");
        }
      }
      new StoreInst(StoreVal, NewGV, SI);
    } else {
      // Change the load into a load of bool then a select.
      LoadInst *LI = cast<LoadInst>(UI);
      LoadInst *NLI = new LoadInst(NewGV, LI->getName()+".b", LI);
      Value *NSI;
      if (IsOneZero)
        NSI = new ZExtInst(NLI, LI->getType(), "", LI);
      else
        NSI = SelectInst::Create(NLI, OtherVal, InitVal, "", LI);
      NSI->takeName(LI);
      LI->replaceAllUsesWith(NSI);
    }
    UI->eraseFromParent();
  }

  GV->eraseFromParent();
  return true;
}


/// ProcessInternalGlobal - Analyze the specified global variable and optimize
/// it if possible.  If we make a change, return true.
bool GlobalOpt::ProcessInternalGlobal(GlobalVariable *GV,
                                      Module::global_iterator &GVI) {
  std::set<PHINode*> PHIUsers;
  GlobalStatus GS;
  GV->removeDeadConstantUsers();

  if (GV->use_empty()) {
    DOUT << "GLOBAL DEAD: " << *GV;
    GV->eraseFromParent();
    ++NumDeleted;
    return true;
  }

  if (!AnalyzeGlobal(GV, GS, PHIUsers)) {
#if 0
    cerr << "Global: " << *GV;
    cerr << "  isLoaded = " << GS.isLoaded << "\n";
    cerr << "  StoredType = ";
    switch (GS.StoredType) {
    case GlobalStatus::NotStored: cerr << "NEVER STORED\n"; break;
    case GlobalStatus::isInitializerStored: cerr << "INIT STORED\n"; break;
    case GlobalStatus::isStoredOnce: cerr << "STORED ONCE\n"; break;
    case GlobalStatus::isStored: cerr << "stored\n"; break;
    }
    if (GS.StoredType == GlobalStatus::isStoredOnce && GS.StoredOnceValue)
      cerr << "  StoredOnceValue = " << *GS.StoredOnceValue << "\n";
    if (GS.AccessingFunction && !GS.HasMultipleAccessingFunctions)
      cerr << "  AccessingFunction = " << GS.AccessingFunction->getName()
                << "\n";
    cerr << "  HasMultipleAccessingFunctions =  "
              << GS.HasMultipleAccessingFunctions << "\n";
    cerr << "  HasNonInstructionUser = " << GS.HasNonInstructionUser<<"\n";
    cerr << "\n";
#endif
    
    // If this is a first class global and has only one accessing function
    // and this function is main (which we know is not recursive we can make
    // this global a local variable) we replace the global with a local alloca
    // in this function.
    //
    // NOTE: It doesn't make sense to promote non first class types since we
    // are just replacing static memory to stack memory.
    if (!GS.HasMultipleAccessingFunctions &&
        GS.AccessingFunction && !GS.HasNonInstructionUser &&
        GV->getType()->getElementType()->isFirstClassType() &&
        GS.AccessingFunction->getName() == "main" &&
        GS.AccessingFunction->hasExternalLinkage()) {
      DOUT << "LOCALIZING GLOBAL: " << *GV;
      Instruction* FirstI = GS.AccessingFunction->getEntryBlock().begin();
      const Type* ElemTy = GV->getType()->getElementType();
      // FIXME: Pass Global's alignment when globals have alignment
      AllocaInst* Alloca = new AllocaInst(ElemTy, NULL, GV->getName(), FirstI);
      if (!isa<UndefValue>(GV->getInitializer()))
        new StoreInst(GV->getInitializer(), Alloca, FirstI);

      GV->replaceAllUsesWith(Alloca);
      GV->eraseFromParent();
      ++NumLocalized;
      return true;
    }
    
    // If the global is never loaded (but may be stored to), it is dead.
    // Delete it now.
    if (!GS.isLoaded) {
      DOUT << "GLOBAL NEVER LOADED: " << *GV;

      // Delete any stores we can find to the global.  We may not be able to
      // make it completely dead though.
      bool Changed = CleanupConstantGlobalUsers(GV, GV->getInitializer());

      // If the global is dead now, delete it.
      if (GV->use_empty()) {
        GV->eraseFromParent();
        ++NumDeleted;
        Changed = true;
      }
      return Changed;

    } else if (GS.StoredType <= GlobalStatus::isInitializerStored) {
      DOUT << "MARKING CONSTANT: " << *GV;
      GV->setConstant(true);

      // Clean up any obviously simplifiable users now.
      CleanupConstantGlobalUsers(GV, GV->getInitializer());

      // If the global is dead now, just nuke it.
      if (GV->use_empty()) {
        DOUT << "   *** Marking constant allowed us to simplify "
             << "all users and delete global!\n";
        GV->eraseFromParent();
        ++NumDeleted;
      }

      ++NumMarked;
      return true;
    } else if (!GV->getInitializer()->getType()->isFirstClassType()) {
      if (GlobalVariable *FirstNewGV = SRAGlobal(GV, 
                                                 getAnalysis<TargetData>())) {
        GVI = FirstNewGV;  // Don't skip the newly produced globals!
        return true;
      }
    } else if (GS.StoredType == GlobalStatus::isStoredOnce) {
      // If the initial value for the global was an undef value, and if only
      // one other value was stored into it, we can just change the
      // initializer to be an undef value, then delete all stores to the
      // global.  This allows us to mark it constant.
      if (Constant *SOVConstant = dyn_cast<Constant>(GS.StoredOnceValue))
        if (isa<UndefValue>(GV->getInitializer())) {
          // Change the initial value here.
          GV->setInitializer(SOVConstant);

          // Clean up any obviously simplifiable users now.
          CleanupConstantGlobalUsers(GV, GV->getInitializer());

          if (GV->use_empty()) {
            DOUT << "   *** Substituting initializer allowed us to "
                 << "simplify all users and delete global!\n";
            GV->eraseFromParent();
            ++NumDeleted;
          } else {
            GVI = GV;
          }
          ++NumSubstitute;
          return true;
        }

      // Try to optimize globals based on the knowledge that only one value
      // (besides its initializer) is ever stored to the global.
      if (OptimizeOnceStoredGlobal(GV, GS.StoredOnceValue, GVI,
                                   getAnalysis<TargetData>()))
        return true;

      // Otherwise, if the global was not a boolean, we can shrink it to be a
      // boolean.
      if (Constant *SOVConstant = dyn_cast<Constant>(GS.StoredOnceValue))
        if (TryToShrinkGlobalToBoolean(GV, SOVConstant)) {
          ++NumShrunkToBool;
          return true;
        }
    }
  }
  return false;
}

/// OnlyCalledDirectly - Return true if the specified function is only called
/// directly.  In other words, its address is never taken.
static bool OnlyCalledDirectly(Function *F) {
  for (Value::use_iterator UI = F->use_begin(), E = F->use_end(); UI != E;++UI){
    Instruction *User = dyn_cast<Instruction>(*UI);
    if (!User) return false;
    if (!isa<CallInst>(User) && !isa<InvokeInst>(User)) return false;

    // See if the function address is passed as an argument.
    for (unsigned i = 1, e = User->getNumOperands(); i != e; ++i)
      if (User->getOperand(i) == F) return false;
  }
  return true;
}

/// ChangeCalleesToFastCall - Walk all of the direct calls of the specified
/// function, changing them to FastCC.
static void ChangeCalleesToFastCall(Function *F) {
  for (Value::use_iterator UI = F->use_begin(), E = F->use_end(); UI != E;++UI){
    CallSite User(cast<Instruction>(*UI));
    User.setCallingConv(CallingConv::Fast);
  }
}

static PAListPtr StripNest(const PAListPtr &Attrs) {
  for (unsigned i = 0, e = Attrs.getNumSlots(); i != e; ++i) {
    if ((Attrs.getSlot(i).Attrs & ParamAttr::Nest) == 0)
      continue;

    // There can be only one.
    return Attrs.removeAttr(Attrs.getSlot(i).Index, ParamAttr::Nest);
  }

  return Attrs;
}

static void RemoveNestAttribute(Function *F) {
  F->setParamAttrs(StripNest(F->getParamAttrs()));
  for (Value::use_iterator UI = F->use_begin(), E = F->use_end(); UI != E;++UI){
    CallSite User(cast<Instruction>(*UI));
    User.setParamAttrs(StripNest(User.getParamAttrs()));
  }
}

bool GlobalOpt::OptimizeFunctions(Module &M) {
  bool Changed = false;
  // Optimize functions.
  for (Module::iterator FI = M.begin(), E = M.end(); FI != E; ) {
    Function *F = FI++;
    F->removeDeadConstantUsers();
    if (F->use_empty() && (F->hasInternalLinkage() ||
                           F->hasLinkOnceLinkage())) {
      M.getFunctionList().erase(F);
      Changed = true;
      ++NumFnDeleted;
    } else if (F->hasInternalLinkage()) {
      if (F->getCallingConv() == CallingConv::C && !F->isVarArg() &&
          OnlyCalledDirectly(F)) {
        // If this function has C calling conventions, is not a varargs
        // function, and is only called directly, promote it to use the Fast
        // calling convention.
        F->setCallingConv(CallingConv::Fast);
        ChangeCalleesToFastCall(F);
        ++NumFastCallFns;
        Changed = true;
      }

      if (F->getParamAttrs().hasAttrSomewhere(ParamAttr::Nest) &&
          OnlyCalledDirectly(F)) {
        // The function is not used by a trampoline intrinsic, so it is safe
        // to remove the 'nest' attribute.
        RemoveNestAttribute(F);
        ++NumNestRemoved;
        Changed = true;
      }
    }
  }
  return Changed;
}

bool GlobalOpt::OptimizeGlobalVars(Module &M) {
  bool Changed = false;
  for (Module::global_iterator GVI = M.global_begin(), E = M.global_end();
       GVI != E; ) {
    GlobalVariable *GV = GVI++;
    if (!GV->isConstant() && GV->hasInternalLinkage() &&
        GV->hasInitializer())
      Changed |= ProcessInternalGlobal(GV, GVI);
  }
  return Changed;
}

/// FindGlobalCtors - Find the llvm.globalctors list, verifying that all
/// initializers have an init priority of 65535.
GlobalVariable *GlobalOpt::FindGlobalCtors(Module &M) {
  for (Module::global_iterator I = M.global_begin(), E = M.global_end();
       I != E; ++I)
    if (I->getName() == "llvm.global_ctors") {
      // Found it, verify it's an array of { int, void()* }.
      const ArrayType *ATy =dyn_cast<ArrayType>(I->getType()->getElementType());
      if (!ATy) return 0;
      const StructType *STy = dyn_cast<StructType>(ATy->getElementType());
      if (!STy || STy->getNumElements() != 2 ||
          STy->getElementType(0) != Type::Int32Ty) return 0;
      const PointerType *PFTy = dyn_cast<PointerType>(STy->getElementType(1));
      if (!PFTy) return 0;
      const FunctionType *FTy = dyn_cast<FunctionType>(PFTy->getElementType());
      if (!FTy || FTy->getReturnType() != Type::VoidTy || FTy->isVarArg() ||
          FTy->getNumParams() != 0)
        return 0;
      
      // Verify that the initializer is simple enough for us to handle.
      if (!I->hasInitializer()) return 0;
      ConstantArray *CA = dyn_cast<ConstantArray>(I->getInitializer());
      if (!CA) return 0;
      for (unsigned i = 0, e = CA->getNumOperands(); i != e; ++i)
        if (ConstantStruct *CS = dyn_cast<ConstantStruct>(CA->getOperand(i))) {
          if (isa<ConstantPointerNull>(CS->getOperand(1)))
            continue;

          // Must have a function or null ptr.
          if (!isa<Function>(CS->getOperand(1)))
            return 0;
          
          // Init priority must be standard.
          ConstantInt *CI = dyn_cast<ConstantInt>(CS->getOperand(0));
          if (!CI || CI->getZExtValue() != 65535)
            return 0;
        } else {
          return 0;
        }
      
      return I;
    }
  return 0;
}

/// ParseGlobalCtors - Given a llvm.global_ctors list that we can understand,
/// return a list of the functions and null terminator as a vector.
static std::vector<Function*> ParseGlobalCtors(GlobalVariable *GV) {
  ConstantArray *CA = cast<ConstantArray>(GV->getInitializer());
  std::vector<Function*> Result;
  Result.reserve(CA->getNumOperands());
  for (unsigned i = 0, e = CA->getNumOperands(); i != e; ++i) {
    ConstantStruct *CS = cast<ConstantStruct>(CA->getOperand(i));
    Result.push_back(dyn_cast<Function>(CS->getOperand(1)));
  }
  return Result;
}

/// InstallGlobalCtors - Given a specified llvm.global_ctors list, install the
/// specified array, returning the new global to use.
static GlobalVariable *InstallGlobalCtors(GlobalVariable *GCL, 
                                          const std::vector<Function*> &Ctors) {
  // If we made a change, reassemble the initializer list.
  std::vector<Constant*> CSVals;
  CSVals.push_back(ConstantInt::get(Type::Int32Ty, 65535));
  CSVals.push_back(0);
  
  // Create the new init list.
  std::vector<Constant*> CAList;
  for (unsigned i = 0, e = Ctors.size(); i != e; ++i) {
    if (Ctors[i]) {
      CSVals[1] = Ctors[i];
    } else {
      const Type *FTy = FunctionType::get(Type::VoidTy,
                                          std::vector<const Type*>(), false);
      const PointerType *PFTy = PointerType::getUnqual(FTy);
      CSVals[1] = Constant::getNullValue(PFTy);
      CSVals[0] = ConstantInt::get(Type::Int32Ty, 2147483647);
    }
    CAList.push_back(ConstantStruct::get(CSVals));
  }
  
  // Create the array initializer.
  const Type *StructTy =
    cast<ArrayType>(GCL->getType()->getElementType())->getElementType();
  Constant *CA = ConstantArray::get(ArrayType::get(StructTy, CAList.size()),
                                    CAList);
  
  // If we didn't change the number of elements, don't create a new GV.
  if (CA->getType() == GCL->getInitializer()->getType()) {
    GCL->setInitializer(CA);
    return GCL;
  }
  
  // Create the new global and insert it next to the existing list.
  GlobalVariable *NGV = new GlobalVariable(CA->getType(), GCL->isConstant(),
                                           GCL->getLinkage(), CA, "",
                                           (Module *)NULL,
                                           GCL->isThreadLocal());
  GCL->getParent()->getGlobalList().insert(GCL, NGV);
  NGV->takeName(GCL);
  
  // Nuke the old list, replacing any uses with the new one.
  if (!GCL->use_empty()) {
    Constant *V = NGV;
    if (V->getType() != GCL->getType())
      V = ConstantExpr::getBitCast(V, GCL->getType());
    GCL->replaceAllUsesWith(V);
  }
  GCL->eraseFromParent();
  
  if (Ctors.size())
    return NGV;
  else
    return 0;
}


static Constant *getVal(std::map<Value*, Constant*> &ComputedValues,
                        Value *V) {
  if (Constant *CV = dyn_cast<Constant>(V)) return CV;
  Constant *R = ComputedValues[V];
  assert(R && "Reference to an uncomputed value!");
  return R;
}

/// isSimpleEnoughPointerToCommit - Return true if this constant is simple
/// enough for us to understand.  In particular, if it is a cast of something,
/// we punt.  We basically just support direct accesses to globals and GEP's of
/// globals.  This should be kept up to date with CommitValueTo.
static bool isSimpleEnoughPointerToCommit(Constant *C) {
  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(C)) {
    if (!GV->hasExternalLinkage() && !GV->hasInternalLinkage())
      return false;  // do not allow weak/linkonce/dllimport/dllexport linkage.
    return !GV->isDeclaration();  // reject external globals.
  }
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(C))
    // Handle a constantexpr gep.
    if (CE->getOpcode() == Instruction::GetElementPtr &&
        isa<GlobalVariable>(CE->getOperand(0))) {
      GlobalVariable *GV = cast<GlobalVariable>(CE->getOperand(0));
      if (!GV->hasExternalLinkage() && !GV->hasInternalLinkage())
        return false;  // do not allow weak/linkonce/dllimport/dllexport linkage.
      return GV->hasInitializer() &&
             ConstantFoldLoadThroughGEPConstantExpr(GV->getInitializer(), CE);
    }
  return false;
}

/// EvaluateStoreInto - Evaluate a piece of a constantexpr store into a global
/// initializer.  This returns 'Init' modified to reflect 'Val' stored into it.
/// At this point, the GEP operands of Addr [0, OpNo) have been stepped into.
static Constant *EvaluateStoreInto(Constant *Init, Constant *Val,
                                   ConstantExpr *Addr, unsigned OpNo) {
  // Base case of the recursion.
  if (OpNo == Addr->getNumOperands()) {
    assert(Val->getType() == Init->getType() && "Type mismatch!");
    return Val;
  }
  
  if (const StructType *STy = dyn_cast<StructType>(Init->getType())) {
    std::vector<Constant*> Elts;

    // Break up the constant into its elements.
    if (ConstantStruct *CS = dyn_cast<ConstantStruct>(Init)) {
      for (unsigned i = 0, e = CS->getNumOperands(); i != e; ++i)
        Elts.push_back(CS->getOperand(i));
    } else if (isa<ConstantAggregateZero>(Init)) {
      for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i)
        Elts.push_back(Constant::getNullValue(STy->getElementType(i)));
    } else if (isa<UndefValue>(Init)) {
      for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i)
        Elts.push_back(UndefValue::get(STy->getElementType(i)));
    } else {
      assert(0 && "This code is out of sync with "
             " ConstantFoldLoadThroughGEPConstantExpr");
    }
    
    // Replace the element that we are supposed to.
    ConstantInt *CU = cast<ConstantInt>(Addr->getOperand(OpNo));
    unsigned Idx = CU->getZExtValue();
    assert(Idx < STy->getNumElements() && "Struct index out of range!");
    Elts[Idx] = EvaluateStoreInto(Elts[Idx], Val, Addr, OpNo+1);
    
    // Return the modified struct.
    return ConstantStruct::get(&Elts[0], Elts.size(), STy->isPacked());
  } else {
    ConstantInt *CI = cast<ConstantInt>(Addr->getOperand(OpNo));
    const ArrayType *ATy = cast<ArrayType>(Init->getType());

    // Break up the array into elements.
    std::vector<Constant*> Elts;
    if (ConstantArray *CA = dyn_cast<ConstantArray>(Init)) {
      for (unsigned i = 0, e = CA->getNumOperands(); i != e; ++i)
        Elts.push_back(CA->getOperand(i));
    } else if (isa<ConstantAggregateZero>(Init)) {
      Constant *Elt = Constant::getNullValue(ATy->getElementType());
      Elts.assign(ATy->getNumElements(), Elt);
    } else if (isa<UndefValue>(Init)) {
      Constant *Elt = UndefValue::get(ATy->getElementType());
      Elts.assign(ATy->getNumElements(), Elt);
    } else {
      assert(0 && "This code is out of sync with "
             " ConstantFoldLoadThroughGEPConstantExpr");
    }
    
    assert(CI->getZExtValue() < ATy->getNumElements());
    Elts[CI->getZExtValue()] =
      EvaluateStoreInto(Elts[CI->getZExtValue()], Val, Addr, OpNo+1);
    return ConstantArray::get(ATy, Elts);
  }    
}

/// CommitValueTo - We have decided that Addr (which satisfies the predicate
/// isSimpleEnoughPointerToCommit) should get Val as its value.  Make it happen.
static void CommitValueTo(Constant *Val, Constant *Addr) {
  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr)) {
    assert(GV->hasInitializer());
    GV->setInitializer(Val);
    return;
  }
  
  ConstantExpr *CE = cast<ConstantExpr>(Addr);
  GlobalVariable *GV = cast<GlobalVariable>(CE->getOperand(0));
  
  Constant *Init = GV->getInitializer();
  Init = EvaluateStoreInto(Init, Val, CE, 2);
  GV->setInitializer(Init);
}

/// ComputeLoadResult - Return the value that would be computed by a load from
/// P after the stores reflected by 'memory' have been performed.  If we can't
/// decide, return null.
static Constant *ComputeLoadResult(Constant *P,
                                const std::map<Constant*, Constant*> &Memory) {
  // If this memory location has been recently stored, use the stored value: it
  // is the most up-to-date.
  std::map<Constant*, Constant*>::const_iterator I = Memory.find(P);
  if (I != Memory.end()) return I->second;
 
  // Access it.
  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(P)) {
    if (GV->hasInitializer())
      return GV->getInitializer();
    return 0;
  }
  
  // Handle a constantexpr getelementptr.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(P))
    if (CE->getOpcode() == Instruction::GetElementPtr &&
        isa<GlobalVariable>(CE->getOperand(0))) {
      GlobalVariable *GV = cast<GlobalVariable>(CE->getOperand(0));
      if (GV->hasInitializer())
        return ConstantFoldLoadThroughGEPConstantExpr(GV->getInitializer(), CE);
    }

  return 0;  // don't know how to evaluate.
}

/// EvaluateFunction - Evaluate a call to function F, returning true if
/// successful, false if we can't evaluate it.  ActualArgs contains the formal
/// arguments for the function.
static bool EvaluateFunction(Function *F, Constant *&RetVal,
                             const std::vector<Constant*> &ActualArgs,
                             std::vector<Function*> &CallStack,
                             std::map<Constant*, Constant*> &MutatedMemory,
                             std::vector<GlobalVariable*> &AllocaTmps) {
  // Check to see if this function is already executing (recursion).  If so,
  // bail out.  TODO: we might want to accept limited recursion.
  if (std::find(CallStack.begin(), CallStack.end(), F) != CallStack.end())
    return false;
  
  CallStack.push_back(F);
  
  /// Values - As we compute SSA register values, we store their contents here.
  std::map<Value*, Constant*> Values;
  
  // Initialize arguments to the incoming values specified.
  unsigned ArgNo = 0;
  for (Function::arg_iterator AI = F->arg_begin(), E = F->arg_end(); AI != E;
       ++AI, ++ArgNo)
    Values[AI] = ActualArgs[ArgNo];

  /// ExecutedBlocks - We only handle non-looping, non-recursive code.  As such,
  /// we can only evaluate any one basic block at most once.  This set keeps
  /// track of what we have executed so we can detect recursive cases etc.
  std::set<BasicBlock*> ExecutedBlocks;
  
  // CurInst - The current instruction we're evaluating.
  BasicBlock::iterator CurInst = F->begin()->begin();
  
  // This is the main evaluation loop.
  while (1) {
    Constant *InstResult = 0;
    
    if (StoreInst *SI = dyn_cast<StoreInst>(CurInst)) {
      if (SI->isVolatile()) return false;  // no volatile accesses.
      Constant *Ptr = getVal(Values, SI->getOperand(1));
      if (!isSimpleEnoughPointerToCommit(Ptr))
        // If this is too complex for us to commit, reject it.
        return false;
      Constant *Val = getVal(Values, SI->getOperand(0));
      MutatedMemory[Ptr] = Val;
    } else if (BinaryOperator *BO = dyn_cast<BinaryOperator>(CurInst)) {
      InstResult = ConstantExpr::get(BO->getOpcode(),
                                     getVal(Values, BO->getOperand(0)),
                                     getVal(Values, BO->getOperand(1)));
    } else if (CmpInst *CI = dyn_cast<CmpInst>(CurInst)) {
      InstResult = ConstantExpr::getCompare(CI->getPredicate(),
                                            getVal(Values, CI->getOperand(0)),
                                            getVal(Values, CI->getOperand(1)));
    } else if (CastInst *CI = dyn_cast<CastInst>(CurInst)) {
      InstResult = ConstantExpr::getCast(CI->getOpcode(),
                                         getVal(Values, CI->getOperand(0)),
                                         CI->getType());
    } else if (SelectInst *SI = dyn_cast<SelectInst>(CurInst)) {
      InstResult = ConstantExpr::getSelect(getVal(Values, SI->getOperand(0)),
                                           getVal(Values, SI->getOperand(1)),
                                           getVal(Values, SI->getOperand(2)));
    } else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(CurInst)) {
      Constant *P = getVal(Values, GEP->getOperand(0));
      SmallVector<Constant*, 8> GEPOps;
      for (unsigned i = 1, e = GEP->getNumOperands(); i != e; ++i)
        GEPOps.push_back(getVal(Values, GEP->getOperand(i)));
      InstResult = ConstantExpr::getGetElementPtr(P, &GEPOps[0], GEPOps.size());
    } else if (LoadInst *LI = dyn_cast<LoadInst>(CurInst)) {
      if (LI->isVolatile()) return false;  // no volatile accesses.
      InstResult = ComputeLoadResult(getVal(Values, LI->getOperand(0)),
                                     MutatedMemory);
      if (InstResult == 0) return false; // Could not evaluate load.
    } else if (AllocaInst *AI = dyn_cast<AllocaInst>(CurInst)) {
      if (AI->isArrayAllocation()) return false;  // Cannot handle array allocs.
      const Type *Ty = AI->getType()->getElementType();
      AllocaTmps.push_back(new GlobalVariable(Ty, false,
                                              GlobalValue::InternalLinkage,
                                              UndefValue::get(Ty),
                                              AI->getName()));
      InstResult = AllocaTmps.back();     
    } else if (CallInst *CI = dyn_cast<CallInst>(CurInst)) {
      // Cannot handle inline asm.
      if (isa<InlineAsm>(CI->getOperand(0))) return false;

      // Resolve function pointers.
      Function *Callee = dyn_cast<Function>(getVal(Values, CI->getOperand(0)));
      if (!Callee) return false;  // Cannot resolve.

      std::vector<Constant*> Formals;
      for (unsigned i = 1, e = CI->getNumOperands(); i != e; ++i)
        Formals.push_back(getVal(Values, CI->getOperand(i)));
      
      if (Callee->isDeclaration()) {
        // If this is a function we can constant fold, do it.
        if (Constant *C = ConstantFoldCall(Callee, &Formals[0],
                                           Formals.size())) {
          InstResult = C;
        } else {
          return false;
        }
      } else {
        if (Callee->getFunctionType()->isVarArg())
          return false;
        
        Constant *RetVal;
        
        // Execute the call, if successful, use the return value.
        if (!EvaluateFunction(Callee, RetVal, Formals, CallStack,
                              MutatedMemory, AllocaTmps))
          return false;
        InstResult = RetVal;
      }
    } else if (isa<TerminatorInst>(CurInst)) {
      BasicBlock *NewBB = 0;
      if (BranchInst *BI = dyn_cast<BranchInst>(CurInst)) {
        if (BI->isUnconditional()) {
          NewBB = BI->getSuccessor(0);
        } else {
          ConstantInt *Cond =
            dyn_cast<ConstantInt>(getVal(Values, BI->getCondition()));
          if (!Cond) return false;  // Cannot determine.

          NewBB = BI->getSuccessor(!Cond->getZExtValue());          
        }
      } else if (SwitchInst *SI = dyn_cast<SwitchInst>(CurInst)) {
        ConstantInt *Val =
          dyn_cast<ConstantInt>(getVal(Values, SI->getCondition()));
        if (!Val) return false;  // Cannot determine.
        NewBB = SI->getSuccessor(SI->findCaseValue(Val));
      } else if (ReturnInst *RI = dyn_cast<ReturnInst>(CurInst)) {
        if (RI->getNumOperands())
          RetVal = getVal(Values, RI->getOperand(0));
        
        CallStack.pop_back();  // return from fn.
        return true;  // We succeeded at evaluating this ctor!
      } else {
        // invoke, unwind, unreachable.
        return false;  // Cannot handle this terminator.
      }
      
      // Okay, we succeeded in evaluating this control flow.  See if we have
      // executed the new block before.  If so, we have a looping function,
      // which we cannot evaluate in reasonable time.
      if (!ExecutedBlocks.insert(NewBB).second)
        return false;  // looped!
      
      // Okay, we have never been in this block before.  Check to see if there
      // are any PHI nodes.  If so, evaluate them with information about where
      // we came from.
      BasicBlock *OldBB = CurInst->getParent();
      CurInst = NewBB->begin();
      PHINode *PN;
      for (; (PN = dyn_cast<PHINode>(CurInst)); ++CurInst)
        Values[PN] = getVal(Values, PN->getIncomingValueForBlock(OldBB));

      // Do NOT increment CurInst.  We know that the terminator had no value.
      continue;
    } else {
      // Did not know how to evaluate this!
      return false;
    }
    
    if (!CurInst->use_empty())
      Values[CurInst] = InstResult;
    
    // Advance program counter.
    ++CurInst;
  }
}

/// EvaluateStaticConstructor - Evaluate static constructors in the function, if
/// we can.  Return true if we can, false otherwise.
static bool EvaluateStaticConstructor(Function *F) {
  /// MutatedMemory - For each store we execute, we update this map.  Loads
  /// check this to get the most up-to-date value.  If evaluation is successful,
  /// this state is committed to the process.
  std::map<Constant*, Constant*> MutatedMemory;

  /// AllocaTmps - To 'execute' an alloca, we create a temporary global variable
  /// to represent its body.  This vector is needed so we can delete the
  /// temporary globals when we are done.
  std::vector<GlobalVariable*> AllocaTmps;
  
  /// CallStack - This is used to detect recursion.  In pathological situations
  /// we could hit exponential behavior, but at least there is nothing
  /// unbounded.
  std::vector<Function*> CallStack;

  // Call the function.
  Constant *RetValDummy;
  bool EvalSuccess = EvaluateFunction(F, RetValDummy, std::vector<Constant*>(),
                                       CallStack, MutatedMemory, AllocaTmps);
  if (EvalSuccess) {
    // We succeeded at evaluation: commit the result.
    DOUT << "FULLY EVALUATED GLOBAL CTOR FUNCTION '"
         << F->getName() << "' to " << MutatedMemory.size()
         << " stores.\n";
    for (std::map<Constant*, Constant*>::iterator I = MutatedMemory.begin(),
         E = MutatedMemory.end(); I != E; ++I)
      CommitValueTo(I->second, I->first);
  }
  
  // At this point, we are done interpreting.  If we created any 'alloca'
  // temporaries, release them now.
  while (!AllocaTmps.empty()) {
    GlobalVariable *Tmp = AllocaTmps.back();
    AllocaTmps.pop_back();
    
    // If there are still users of the alloca, the program is doing something
    // silly, e.g. storing the address of the alloca somewhere and using it
    // later.  Since this is undefined, we'll just make it be null.
    if (!Tmp->use_empty())
      Tmp->replaceAllUsesWith(Constant::getNullValue(Tmp->getType()));
    delete Tmp;
  }
  
  return EvalSuccess;
}



/// OptimizeGlobalCtorsList - Simplify and evaluation global ctors if possible.
/// Return true if anything changed.
bool GlobalOpt::OptimizeGlobalCtorsList(GlobalVariable *&GCL) {
  std::vector<Function*> Ctors = ParseGlobalCtors(GCL);
  bool MadeChange = false;
  if (Ctors.empty()) return false;
  
  // Loop over global ctors, optimizing them when we can.
  for (unsigned i = 0; i != Ctors.size(); ++i) {
    Function *F = Ctors[i];
    // Found a null terminator in the middle of the list, prune off the rest of
    // the list.
    if (F == 0) {
      if (i != Ctors.size()-1) {
        Ctors.resize(i+1);
        MadeChange = true;
      }
      break;
    }
    
    // We cannot simplify external ctor functions.
    if (F->empty()) continue;
    
    // If we can evaluate the ctor at compile time, do.
    if (EvaluateStaticConstructor(F)) {
      Ctors.erase(Ctors.begin()+i);
      MadeChange = true;
      --i;
      ++NumCtorsEvaluated;
      continue;
    }
  }
  
  if (!MadeChange) return false;
  
  GCL = InstallGlobalCtors(GCL, Ctors);
  return true;
}


bool GlobalOpt::runOnModule(Module &M) {
  bool Changed = false;
  
  // Try to find the llvm.globalctors list.
  GlobalVariable *GlobalCtors = FindGlobalCtors(M);

  bool LocalChange = true;
  while (LocalChange) {
    LocalChange = false;
    
    // Delete functions that are trivially dead, ccc -> fastcc
    LocalChange |= OptimizeFunctions(M);
    
    // Optimize global_ctors list.
    if (GlobalCtors)
      LocalChange |= OptimizeGlobalCtorsList(GlobalCtors);
    
    // Optimize non-address-taken globals.
    LocalChange |= OptimizeGlobalVars(M);
    Changed |= LocalChange;
  }
  
  // TODO: Move all global ctors functions to the end of the module for code
  // layout.
  
  return Changed;
}
