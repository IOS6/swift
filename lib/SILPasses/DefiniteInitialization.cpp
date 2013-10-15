//===--- DefiniteInitialization.cpp - Perform definite init analysis ------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "definite-init"
#include "swift/Subsystems.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/Diagnostics.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/Basic/Fixnum.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/StringExtras.h"
using namespace swift;

STATISTIC(NumLoadPromoted, "Number of loads promoted");
STATISTIC(NumAssignRewritten, "Number of assigns rewritten");

static llvm::cl::opt<bool>
EnableCopyAddrForwarding("enable-copyaddr-forwarding");

template<typename ...ArgTypes>
static void diagnose(SILModule &M, SILLocation loc, ArgTypes... args) {
  M.getASTContext().Diags.diagnose(loc.getSourceLoc(), Diagnostic(args...));
}

/// Emit the sequence that an assign instruction lowers to once we know
/// if it is an initialization or an assignment.  If it is an assignment,
/// a live-in value can be provided to optimize out the reload.
static void LowerAssignInstruction(SILBuilder &B, AssignInst *Inst,
                                   bool isInitialization) {
  DEBUG(llvm::errs() << "  *** Lowering [isInit=" << isInitialization << "]: "
            << *Inst << "\n");

  ++NumAssignRewritten;

  auto &M = Inst->getModule();
  SILValue Src = Inst->getSrc();

  auto &destTL = M.getTypeLowering(Inst->getDest().getType());

  // If this is an initialization, or the storage type is trivial, we
  // can just replace the assignment with a store.

  // Otherwise, if it has trivial type, we can always just replace the
  // assignment with a store.  If it has non-trivial type and is an
  // initialization, we can also replace it with a store.
  if (isInitialization || destTL.isTrivial()) {
    B.createStore(Inst->getLoc(), Src, Inst->getDest());
  } else {
    // Otherwise, we need to replace the assignment with the full
    // load/store/release dance.  Note that the new value is already
    // considered to be retained (by the semantics of the storage type),
    // and we're transfering that ownership count into the destination.

    // This is basically destTL.emitStoreOfCopy, except that if we have
    // a known incoming value, we can avoid the load.
    SILValue IncomingVal = B.createLoad(Inst->getLoc(), Inst->getDest());
    B.createStore(Inst->getLoc(), Src, Inst->getDest());
    destTL.emitDestroyValue(B, Inst->getLoc(), IncomingVal);
  }

  Inst->eraseFromParent();
}



//===----------------------------------------------------------------------===//
// Tuple Element Flattening/Counting Logic
//===----------------------------------------------------------------------===//

/// getTupleElementCount - Return the number of elements in the flattened
/// SILType.  For tuples, this is the (recursive) count of the fields it
/// contains.
static unsigned getTupleElementCount(CanType T) {
  TupleType *TT = T->getAs<TupleType>();

  // If this isn't a tuple, it is a single element.
  if (!TT) return 1;

  unsigned NumElements = 0;
  for (auto &Elt : TT->getFields())
    NumElements += getTupleElementCount(Elt.getType()->getCanonicalType());
  return NumElements;
}

#if 0
/// Given a symbolic element number, return the type of the element.
static CanType getTupleElementType(CanType T, unsigned EltNo) {
  TupleType *TT = T->getAs<TupleType>();

  // If this isn't a tuple, it is a leaf element.
  if (!TT) {
    assert(EltNo == 0);
    return T;
  }

  for (auto &Elt : TT->getFields()) {
    auto FieldType = Elt.getType()->getCanonicalType();
    unsigned NumFields = getTupleElementCount(FieldType);
    if (EltNo < NumFields)
      return getTupleElementType(FieldType, EltNo);
    EltNo -= NumFields;
  }

  assert(0 && "invalid element number");
  abort();
}
#endif

/// Push the symbolic path name to the specified element number onto the
/// specified std::string.
static void getPathStringToElement(CanType T, unsigned Element,
                                   std::string &Result) {
  TupleType *TT = T->getAs<TupleType>();
  if (!TT) return;

  unsigned FieldNo = 0;
  for (auto &Field : TT->getFields()) {
    unsigned ElementsForField =
      getTupleElementCount(Field.getType()->getCanonicalType());
    
    if (Element < ElementsForField) {
      Result += '.';
      if (Field.hasName())
        Result += Field.getName().str();
      else
        Result += llvm::utostr(FieldNo);
      return getPathStringToElement(Field.getType()->getCanonicalType(),
                                    Element, Result);
    }
    
    Element -= ElementsForField;
    
    ++FieldNo;
  }
  assert(0 && "Element number is out of range for this type!");
}

//===----------------------------------------------------------------------===//
// Scalarization Logic
//===----------------------------------------------------------------------===//

/// Given a pointer to an aggregate type, compute the addresses of each
/// element and add them to the ElementAddrs vector.
static void getScalarizedElementAddresses(SILValue Pointer,
                              SmallVectorImpl<SILInstruction*> &ElementAddrs) {
  CanType AggType = Pointer.getType().getSwiftRValueType();

  SILInstruction *PointerInst = cast<SILInstruction>(Pointer.getDef());
  SILBuilder B(++SILBasicBlock::iterator(PointerInst));

  if (TupleType *TT = AggType->getAs<TupleType>()) {
    for (auto &Field : TT->getFields()) {
      (void)Field;
      ElementAddrs.push_back(B.createTupleElementAddr(PointerInst->getLoc(),
                                                      Pointer,
                                                      ElementAddrs.size()));
    }
    return;
  }

  assert(AggType->is<StructType>() || AggType->is<BoundGenericStructType>());
  StructDecl *SD = cast<StructDecl>(AggType->getAnyNominal());

  for (auto *VD : SD->getStoredProperties()) {
    ElementAddrs.push_back(B.createStructElementAddr(PointerInst->getLoc(),
                                                     Pointer, VD));
  }
}

/// Given an RValue of aggregate type, compute the values of the elements by
/// emitting a series of tuple_element instructions.
static void getScalarizedElements(SILValue V,
                                  SmallVectorImpl<SILValue> &ElementVals,
                                  SILLocation Loc, SILBuilder &B) {
  CanType AggType = V.getType().getSwiftRValueType();

  if (TupleType *TT = AggType->getAs<TupleType>()) {
    // If this is exploding a tuple_inst, just return the element values.  This
    // can happen when recursively scalarizing stuff.
    if (auto *TI = dyn_cast<TupleInst>(V)) {
      for (unsigned i = 0, e = TI->getNumOperands(); i != e; ++i)
        ElementVals.push_back(TI->getOperand(i));
      return;
    }

    for (auto &Field : TT->getFields()) {
      (void)Field;
      ElementVals.push_back(B.createTupleExtract(Loc, V, ElementVals.size()));
    }
    return;
  }

  assert(AggType->is<StructType>() ||
         AggType->is<BoundGenericStructType>());

  // If this is exploding a struct_inst, just return the element values.  This
  // can happen when recursively scalarizing stuff.
  if (auto *SI = dyn_cast<StructInst>(V)) {
    for (unsigned i = 0, e = SI->getNumOperands(); i != e; ++i)
      ElementVals.push_back(SI->getOperand(i));
    return;
  }

  StructDecl *SD = cast<StructDecl>(AggType->getAnyNominal());
  for (auto *VD : SD->getStoredProperties()) {
    ElementVals.push_back(B.createStructExtract(Loc, V, VD));
  }
}

/// Remove dead tuple_element_addr and struct_element_addr chains - only.
static void RemoveDeadAddressingInstructions(SILValue Pointer) {
  if (!Pointer.use_empty()) return;

  SILInstruction *I = dyn_cast<SILInstruction>(Pointer);
  if (I == 0 ||
      !(isa<TupleElementAddrInst>(Pointer) ||
        isa<StructElementAddrInst>(Pointer)))
    return;
  Pointer = I->getOperand(0);
  I->eraseFromParent();
  RemoveDeadAddressingInstructions(Pointer);
}


/// Scalarize a load down to its subelements.  If NewLoads is specified, this
/// can return the newly generated sub-element loads.
static SILValue scalarizeLoad(LoadInst *LI,
                              SmallVectorImpl<SILInstruction*> &ElementAddrs,
                        SmallVectorImpl<SILInstruction*> *NewLoads = nullptr) {
  SILBuilder B(LI);
  SmallVector<SILValue, 4> ElementTmps;

  for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i) {
    auto *SubLI = B.createLoad(LI->getLoc(), ElementAddrs[i]);
    ElementTmps.push_back(SubLI);
    if (NewLoads) NewLoads->push_back(SubLI);
  }

  if (LI->getType().is<TupleType>())
    return B.createTuple(LI->getLoc(), LI->getType(), ElementTmps);
  return B.createStruct(LI->getLoc(), LI->getType(), ElementTmps);
}

//===----------------------------------------------------------------------===//
// Access Path Analysis Logic
//===----------------------------------------------------------------------===//

static unsigned getNumSubElements(CanType T) {
  if (TupleType *TT = T->getAs<TupleType>()) {
    unsigned NumElements = 0;
    for (auto &Elt : TT->getFields())
      NumElements += getNumSubElements(Elt.getType()->getCanonicalType());
    return NumElements;
  }

  if (auto *SD = T->getStructOrBoundGenericStruct()) {
    unsigned NumElements = 0;
    for (auto *D : SD->getStoredProperties())
      NumElements += getNumSubElements(SILBuilder::getStructFieldType(T, D));
    return NumElements;
  }

  // If this isn't a tuple or struct, it is a single element.
  return 1;
}

/// Given a pointer that is known to be derived from an alloc_box, chase up to
/// the alloc box, computing the access path.  This returns true if the access
/// path to the specified RootInst was successfully computed, false otherwise.
static bool TryComputingAccessPath(SILValue Pointer, unsigned &SubEltNumber,
                                   SILInstruction *RootInst) {
  SubEltNumber = 0;
  while (1) {
    // If we got to the root, we're done.
    if (RootInst == Pointer.getDef())
      return true;

    if (auto *TEAI = dyn_cast<TupleElementAddrInst>(Pointer)) {
      TupleType *TT = TEAI->getTupleType();

      // Keep track of what subelement is being referenced.
      for (unsigned i = 0, e = TEAI->getFieldNo(); i != e; ++i)
        SubEltNumber += getNumSubElements(TT->getElementType(i)
                                          ->getCanonicalType());
      Pointer = TEAI->getOperand();
    } else if (auto *SEAI = dyn_cast<StructElementAddrInst>(Pointer)) {
      CanType ST = SEAI->getOperand().getType().getSwiftRValueType();

      // Keep track of what subelement is being referenced.
      StructDecl *SD = SEAI->getStructDecl();
      for (auto *D : SD->getStoredProperties()) {
        if (D == SEAI->getField()) break;
        SubEltNumber += getNumSubElements(SILBuilder::getStructFieldType(ST,D));
      }

      Pointer = SEAI->getOperand();
    } else {
      return false;
    }
  }
}

/// Compute the access path indicated by the specified pointer (which is derived
/// from the root by a series of tuple/struct element addresses) and return
/// the first subelement addressed by the address.  For example, given:
///
///   root = alloc { a: { c: i64, d: i64 }, b: (i64, i64) }
///   tmp1 = struct_element_addr root, 1
///   tmp2 = tuple_element_addr tmp1, 0
///
/// This will return an access path of [struct: 'b', tuple: 0] and a base
/// element of 2.
///
static unsigned ComputeAccessPath(SILValue Pointer, SILInstruction *RootInst) {
  unsigned FirstSubElement = 0;
  bool Result = TryComputingAccessPath(Pointer, FirstSubElement, RootInst);
  assert(Result && "Failed to compute an access path to our root?");
  (void)Result;

  return FirstSubElement;
}


/// Given an aggregate value and an access path, extract the value indicated by
/// the path.
static SILValue ExtractSubElement(SILValue Val, unsigned SubElementNumber,
                                  SILBuilder &B, SILLocation Loc) {
  CanType ValTy = Val.getType().getSwiftRValueType();

  // Extract tuple elements.
  if (TupleType *TT = ValTy->getAs<TupleType>()) {
    unsigned EltNo = 0;
    for (auto &Elt : TT->getFields()) {
      // Keep track of what subelement is being referenced.
      unsigned NumSubElt = getNumSubElements(Elt.getType()->getCanonicalType());
      if (SubElementNumber < NumSubElt) {
        Val = B.createTupleExtract(Loc, Val, EltNo);
        return ExtractSubElement(Val, SubElementNumber, B, Loc);
      }

      SubElementNumber -= NumSubElt;
      ++EltNo;
    }

    assert(0 && "Didn't find field");
  }

  // Extract struct elements.
  if (auto *SD = ValTy->getStructOrBoundGenericStruct()) {
    for (auto *D : SD->getStoredProperties()) {
      unsigned NumSubElt =
        getNumSubElements(SILBuilder::getStructFieldType(ValTy, D));

      if (SubElementNumber < NumSubElt) {
        Val = B.createStructExtract(Loc, Val, D);
        return ExtractSubElement(Val, SubElementNumber, B, Loc);
      }

      SubElementNumber -= NumSubElt;

    }
    assert(0 && "Didn't find field");
  }

  // Otherwise, we're down to a scalar.
  assert(SubElementNumber == 0 && "Miscalculation indexing subelements");
  return Val;
}



//===----------------------------------------------------------------------===//
// Per-Element Promotion Logic
//===----------------------------------------------------------------------===//

namespace {
  enum UseKind {
    // The instruction is a Load.
    Load,

    // The instruction is a Store.
    Store,

    // The instruction is a store to a member of a larger struct value.
    PartialStore,

    /// The instruction is an Apply, this is a inout or indirect return.
    InOutUse,

    /// This instruction is a general escape of the value, e.g. a call to a
    /// closure that captures it.
    Escape,

    /// This instruction is a release, which may be a last use.
    /// TODO: remove this when we support partially constructed values.
    Release
  };

  /// ElementUses - This class keeps track of all of the uses of a single
  /// element (i.e. tuple element or struct field) of a memory object.
  typedef std::vector<std::pair<SILInstruction*, UseKind>> ElementUses;

  enum class EscapeKind {
    Unknown,
    Yes,
    No
  };

  /// LiveOutBlockState - Keep track of information about blocks that have
  /// already been analyzed.  Since this is a global analysis, we need this to
  /// cache information about different paths through the CFG.
  struct LiveOutBlockState {
    /// For this block, keep track of whether there is a path from the entry
    /// of the function to the end of the block that crosses an escape site.
    EscapeKind EscapeInfo = EscapeKind::Unknown;

    /// Keep track of whether there is a Store, InOutUse, or Escape locally in
    /// this block.
    bool HasNonLoadUse = false;

    /// Keep track of whether the element is live out of this block or not.
    ///
    enum LiveOutAvailability {
      IsNotLiveOut,
      IsLiveOut,
      IsComputingLiveOut,
      IsUnknown
    } Availability = IsUnknown;
  };
} // end anonymous namespace

namespace {
  /// ElementPromotion - This is the main heavy lifting for processing the uses
  /// of an element of an allocation.
  class ElementPromotion {
    /// TheMemory - This is either an alloc_box instruction or a
    /// mark_uninitialized instruction.  This represents the start of the
    /// lifetime of the value being analyzed.
    SILInstruction *TheMemory;
    unsigned ElementNumber;

    /// The number of primitive subelements across all elements of this memory
    /// value.
    unsigned NumMemorySubElements;

    ElementUses &Uses;
    llvm::SmallDenseMap<SILBasicBlock*, LiveOutBlockState, 32> PerBlockInfo;

    /// This is the set of uses that are not loads (i.e., they are Stores,
    /// InOutUses, and Escapes).
    llvm::SmallPtrSet<SILInstruction*, 16> NonLoadUses;

    /// Does this value escape anywhere in the function.
    bool HasAnyEscape = false;

    // Keep track of whether we've emitted an error.  We only emit one error per
    // element as a policy decision.
    bool HadError = false;
  public:
    ElementPromotion(SILInstruction *TheMemory, unsigned ElementNumber,
                     ElementUses &Uses);

    void doIt();

  private:
    CanType getTheMemoryType() const {
      if (auto *ABI = dyn_cast<AllocBoxInst>(TheMemory))
        return ABI->getElementType().getSwiftRValueType();
      if (auto *ASI = dyn_cast<AllocStackInst>(TheMemory))
        return ASI->getElementType().getSwiftRValueType();
      // mark_uninitialized.
      return TheMemory->getType(0).getObjectType().getSwiftRValueType();
    }

    void handleLoadUse(SILInstruction *Inst);
    void handleStoreUse(SILInstruction *Inst, bool isPartialStore);
    void handleInOutUse(SILInstruction *Inst);
    void handleEscape(SILInstruction *Inst);
    void handleRelease(SILInstruction *Inst);

    void promoteLoad(SILInstruction *Inst);

    enum DIKind {
      DI_Yes,
      DI_No,
      DI_Partial
    };
    DIKind checkDefinitelyInit(SILInstruction *Inst);

    bool isLiveOut(SILBasicBlock *BB);

    void diagnoseInitError(SILInstruction *Use, Diag<StringRef> DiagMessage);

    // Load promotion.
    bool hasEscapedAt(SILInstruction *I);
    bool updateAvailableValues(SILInstruction *Inst,
                               llvm::SmallBitVector &RequiredElts,
                       SmallVectorImpl<std::pair<SILValue, unsigned>> &Result);
    bool computeAvailableValues(SILInstruction *StartingFrom,
                                llvm::SmallBitVector &RequiredElts,
                        SmallVectorImpl<std::pair<SILValue, unsigned>> &Result);

    void explodeCopyAddr(CopyAddrInst *CAI, SILValue &StoredValue);

  };
} // end anonymous namespace


ElementPromotion::ElementPromotion(SILInstruction *TheMemory,
                                   unsigned ElementNumber, ElementUses &Uses)
  : TheMemory(TheMemory), ElementNumber(ElementNumber), Uses(Uses) {

  NumMemorySubElements = getNumSubElements(getTheMemoryType());

  // The first step of processing an element is to collect information about the
  // element into data structures we use later.
  for (auto Use : Uses) {
    assert(Use.first);

    // Keep track of all the uses that aren't loads.
    if (Use.second == UseKind::Load)
      continue;

    NonLoadUses.insert(Use.first);

    auto &BBInfo = PerBlockInfo[Use.first->getParent()];
    BBInfo.HasNonLoadUse = true;

    // Each of the non-load instructions will each be checked to make sure that
    // they are live-in or a full element store.  This means that the block they
    // are in should be treated as a live out for cross-block analysis purposes.
    BBInfo.Availability = LiveOutBlockState::IsLiveOut;

    if (Use.second == UseKind::Escape) {
      // Determine which blocks the value can escape from.  We aren't allowed to
      // promote loads in blocks reachable from an escape point.
      HasAnyEscape = true;
      BBInfo.EscapeInfo = EscapeKind::Yes;
    }
  }

  // If isn't really a use, but we account for the alloc_box/mark_uninitialized
  // as a use so we see it in our dataflow walks.
  NonLoadUses.insert(TheMemory);
  PerBlockInfo[TheMemory->getParent()].HasNonLoadUse = true;

  // If there was not another store in the memory definition block, then it is
  // known to be not live out.
  auto &BBInfo = PerBlockInfo[TheMemory->getParent()];
  if (BBInfo.Availability == LiveOutBlockState::IsUnknown)
    BBInfo.Availability = LiveOutBlockState::IsNotLiveOut;
}

void ElementPromotion::diagnoseInitError(SILInstruction *Use,
                                         Diag<StringRef> DiagMessage) {
  HadError = true;

  // If the definition is a declaration, try to reconstruct a name and
  // optionally an access path to the uninitialized element.
  std::string Name;
  if (ValueDecl *VD =
        dyn_cast_or_null<ValueDecl>(TheMemory->getLoc().getAsASTNode<Decl>()))
    Name = VD->getName().str();
  else
    Name = "<unknown>";

  // If the overall memory allocation is a tuple with multiple elements,
  // then dive in to explain *which* element is being used uninitialized.
  CanType AllocTy = getTheMemoryType();
  getPathStringToElement(AllocTy, ElementNumber, Name);
  
  diagnose(Use->getModule(), Use->getLoc(), DiagMessage, Name);

  // Provide context as note diagnostics.

  // TODO: The QoI could be improved in many different ways here.  For example,
  // We could give some path information where the use was uninitialized, like
  // the static analyzer.
  diagnose(Use->getModule(), TheMemory->getLoc(), diag::variable_defined_here);
}


void ElementPromotion::doIt() {
  // With any escapes tallied up, we can work through all the uses, checking
  // for definitive initialization, promoting loads, rewriting assigns, and
  // performing other tasks.

  // Note that this should not use a for-each loop, as the Uses list can grow
  // and reallocate as we iterate over it.
  for (unsigned i = 0; i != Uses.size(); ++i) {
    auto &Use = Uses[i];
    // Ignore entries for instructions that got expanded along the way.
    if (Use.first == nullptr) continue;

    switch (Use.second) {
    case UseKind::Load:         handleLoadUse(Use.first); break;
    case UseKind::Store:        handleStoreUse(Use.first, false); break;
    case UseKind::PartialStore: handleStoreUse(Use.first, true); break;
    case UseKind::InOutUse:     handleInOutUse(Use.first); break;
    case UseKind::Escape:       handleEscape(Use.first); break;
    case UseKind::Release:      handleRelease(Use.first); break;
    }

    if (HadError) return;
  }

  // If we've successfully checked all of the definitive initialization
  // requirements, try to promote loads.
  for (unsigned i = 0; i != Uses.size(); ++i) {
    auto &Use = Uses[i];
    // Ignore entries for instructions that got expanded along the way.
    if (Use.first && Use.second == UseKind::Load)
      promoteLoad(Use.first);
  }
}

/// Given a load (i.e., a LoadInst, CopyAddr, LoadWeak, or ProjectExistential),
/// determine whether the loaded value is definitely assigned or not.  If not,
/// produce a diagnostic.
void ElementPromotion::handleLoadUse(SILInstruction *Inst) {
  auto DI = checkDefinitelyInit(Inst);

  // If the value is not definitively initialized, emit an error.

  // TODO: In the "No" case, we can emit a fixit adding a default initialization
  // of the type.
  // TODO: In the "partial" case, we can produce a more specific diagnostic
  // indicating where the control flow merged.
  if (DI != DI_Yes) {
    // Otherwise, this is a use of an uninitialized value.  Emit a diagnostic.
    diagnoseInitError(Inst, diag::variable_used_before_initialized);
    return;
  }
}



void ElementPromotion::handleStoreUse(SILInstruction *Inst,
                                      bool isPartialStore) {

  // We assume that SILGen knows what it is doing when it produces
  // initializations of variables, because it only produces them when it knows
  // they are correct, and this is a super common case for "var x = 4" cases.
  if (!isPartialStore) {
    if (isa<AssignInst>(Inst))
      ;
    else if (auto CA = dyn_cast<CopyAddrInst>(Inst)) {
      if (CA->isInitializationOfDest()) return;
    } else if (auto SW = dyn_cast<StoreWeakInst>(Inst)) {
      if (SW->isInitializationOfDest()) return;
    } else if (isa<InitExistentialInst>(Inst) ||
               isa<UpcastExistentialInst>(Inst) ||
               isa<EnumDataAddrInst>(Inst) ||
               isa<InjectEnumAddrInst>(Inst)) {
      // These instructions *on a box* are only formed by direct initialization
      // like "var x : Proto = foo".
      return;
    } else {
      return;
    }
  }

  // Check to see if the value is known-initialized here or not.
  auto DI = checkDefinitelyInit(Inst);

  // If this is a partial store into a struct and the whole struct hasn't been
  // initialized, diagnose this as an error.
  if (isPartialStore && DI != DI_Yes) {
    diagnoseInitError(Inst, diag::struct_not_fully_initialized);
    return;
  }

  // If it is initialized on some paths, but not others, then we have an
  // inconsistent initialization error.
  //
  // FIXME: This needs to be supported through the introduction of a boolean
  // control path, or (for reference types as an important special case) a store
  // of zero at the definition point.
  if (DI == DI_Partial) {
    diagnoseInitError(Inst, diag::variable_initialized_on_some_paths);
    return;
  }

  // If this is a copy_addr or store_weak, we just set the initialization bit
  // depending on what we find.
  if (auto *CA = dyn_cast<CopyAddrInst>(Inst)) {
    CA->setIsInitializationOfDest(IsInitialization_t(DI == DI_No));
    return;
  }
  if (auto *SW = dyn_cast<StoreWeakInst>(Inst)) {
    SW->setIsInitializationOfDest(IsInitialization_t(DI == DI_No));
    return;
  }

  // If this is an assign, rewrite it based on whether it is an initialization
  // or not.
  if (auto *AI = dyn_cast<AssignInst>(Inst)) {
    NonLoadUses.erase(Inst);

    SmallVector<SILInstruction*, 8> InsertedInsts;
    SILBuilder B(Inst, &InsertedInsts);

    LowerAssignInstruction(B, AI, DI == DI_No);

    // If lowering of the assign introduced any new stores, keep track of them.
    for (auto I : InsertedInsts) {
      if (isa<StoreInst>(I)) {
        NonLoadUses.insert(I);
        Uses.push_back({ I, Store });
      } else if (isa<LoadInst>(I)) {
        Uses.push_back({ I, Load });
      }
    }
  }
}


/// Given a inout use (an Apply), determine whether the loaded
/// value is definitely assigned or not.  If not, produce a diagnostic.
void ElementPromotion::handleInOutUse(SILInstruction *Inst) {
  auto DI = checkDefinitelyInit(Inst);
  if (DI == DI_Yes)
    return;

  // Otherwise, this is a use of an uninitialized value.  Emit a diagnostic.
  diagnoseInitError(Inst, diag::variable_inout_before_initialized);
}

void ElementPromotion::handleEscape(SILInstruction *Inst) {
  auto DI = checkDefinitelyInit(Inst);
  if (DI == DI_Yes)
    return;

  // Otherwise, this is a use of an uninitialized value.  Emit a diagnostic.
  if (isa<MarkFunctionEscapeInst>(Inst))
    diagnoseInitError(Inst, diag::global_variable_function_use_uninit);
  else
    diagnoseInitError(Inst, diag::variable_escape_before_initialized);
}

/// At the time when a box is destroyed, it might be completely uninitialized,
/// and if it is a tuple, it may only be partially initialized.  To avoid
/// ambiguity, we require that all elements of the value are completely
/// initialized at the point of a release.
///
/// TODO: We could make this more powerful to directly support these cases, at
/// lease when the value doesn't escape.
///
void ElementPromotion::handleRelease(SILInstruction *Inst) {
  auto DI = checkDefinitelyInit(Inst);
  if (DI == DI_Yes)
    return;

  // Otherwise, this is a release of an uninitialized value.  Emit a diagnostic.
  diagnoseInitError(Inst, diag::variable_destroyed_before_initialized);
}


bool ElementPromotion::isLiveOut(SILBasicBlock *BB) {
  LiveOutBlockState &BBState = PerBlockInfo[BB];
  switch (BBState.Availability) {
  case LiveOutBlockState::IsNotLiveOut: return false;
  case LiveOutBlockState::IsLiveOut:    return true;
  case LiveOutBlockState::IsComputingLiveOut:
    // Speculate that it will be live out in cyclic cases.
    return true;
  case LiveOutBlockState::IsUnknown:
    // Otherwise, process this block.
    break;
  }

  // Set the block's state to reflect that we're currently processing it.  This
  // is required to handle cycles properly.
  BBState.Availability = LiveOutBlockState::IsComputingLiveOut;

  // Recursively processes all of our predecessor blocks.  If any of them is
  // not live out, then we aren't either.
  for (auto PI = BB->pred_begin(), E = BB->pred_end(); PI != E; ++PI) {
    if (!isLiveOut(*PI)) {
      // If any predecessor fails, then we're not live out either.
      PerBlockInfo[BB].Availability = LiveOutBlockState::IsNotLiveOut;
      return false;
    }
  }

  // Otherwise, we're golden.  Return success.
  PerBlockInfo[BB].Availability = LiveOutBlockState::IsLiveOut;
  return true;
}


/// The specified instruction is a use of the element.  Determine whether the
/// element is definitely initialized at this point or not.  If the value is
/// initialized on some paths, but not others, this returns a partial result.
ElementPromotion::DIKind
ElementPromotion::checkDefinitelyInit(SILInstruction *Inst) {
  SILBasicBlock *InstBB = Inst->getParent();
  // If there is a store in the current block, scan the block to see if the
  // store is before or after the load.  If it is before, it produces the value
  // we are looking for.
  if (PerBlockInfo[InstBB].HasNonLoadUse) {
    for (SILBasicBlock::iterator BBI = Inst, E = Inst->getParent()->begin();
         BBI != E;) {
      SILInstruction *TheInst = --BBI;

      // If this instruction is unrelated to the alloc_box element, ignore it.
      if (!NonLoadUses.count(TheInst))
        continue;

      // If we found the allocation itself, then we are loading something that
      // is not defined at all yet.
      if (TheInst == TheMemory)
        return DI_No;

      return DI_Yes;
    }
  }

  // Okay, the value isn't locally available in this block.  Check to see if it
  // is live in all predecessors and, if interested, collect the list of
  // definitions we'll build SSA form from.
  for (auto PI = InstBB->pred_begin(), E = InstBB->pred_end(); PI != E; ++PI) {
    if (!isLiveOut(*PI))
      return DI_No;
  }

  return DI_Yes;
}


//===----------------------------------------------------------------------===//
//                              Load Promotion
//===----------------------------------------------------------------------===//

/// hasEscapedAt - Return true if the box has escaped at the specified
/// instruction.  We are not allowed to do load promotion in an escape region.
bool ElementPromotion::hasEscapedAt(SILInstruction *I) {
  // FIXME: This is not an aggressive implementation.  :)

  // TODO: At some point, we should special case closures that just *read* from
  // the escaped value (by looking at the body of the closure).  They should not
  // prevent load promotion, and will allow promoting values like X in regions
  // dominated by "... && X != 0".
  return HasAnyEscape;
}


/// The specified instruction is a non-load access of the element being
/// promoted.  See if it provides a value or refines the demanded element mask
/// used for load promotion.  If an available value is provided, this returns
/// true.
bool ElementPromotion::
updateAvailableValues(SILInstruction *Inst, llvm::SmallBitVector &RequiredElts,
                      SmallVectorImpl<std::pair<SILValue, unsigned>> &Result) {

  // Handle store and assign.
  if (isa<StoreInst>(Inst) || isa<AssignInst>(Inst)) {
    bool ProducedSomething = false;
    unsigned StartSubElt = ComputeAccessPath(Inst->getOperand(1), TheMemory);
    CanType ValTy = Inst->getOperand(0).getType().getSwiftRValueType();

    for (unsigned i = 0, e = getNumSubElements(ValTy); i != e; ++i) {
      // If this element is not required, don't fill it in.
      if (!RequiredElts[StartSubElt+i]) continue;

      Result[StartSubElt+i] = { Inst->getOperand(0), i };

      // This element is now provided.
      RequiredElts[StartSubElt+i] = false;
      ProducedSomething = true;
    }

    return ProducedSomething;
  }


#if 0
  // FIXME: CopyAddr
  if (auto *CAI = dyn_cast<CopyAddrInst>(Inst)) {
    // Temporarily gate copyaddr forwarding by a command line flag.
    if (!EnableCopyAddrForwarding)
      return false;
    // We've already filtered to only look at stores to the box, this can't just
    // be a "load" copy_addr from the box (unless it loads *and* stores).
    ComputeAccessPath(CAI->getDest(), TheMemory);
  }
#endif

  // TODO: inout apply's should only clobber pieces passed in.

  // Otherwise, this is some unknown instruction, conservatively assume that all
  // values are clobbered.
  RequiredElts.clear();
  return false;
}


/// Try to find available values of a set of subelements of the current value,
/// starting right before the specified instruction.
///
/// The bitvector indicates which subelements we're interested in, and result
/// captures the available value (plus an indicator of which subelement of that
/// value is needed).  This method returns true if no available values were
/// found or false if some were.
///
bool ElementPromotion::
computeAvailableValues(SILInstruction *StartingFrom,
                       llvm::SmallBitVector &RequiredElts,
                       SmallVectorImpl<std::pair<SILValue, unsigned>> &Result) {

  // If no bits are demanded, we trivially succeed.  This can happen when there
  // is a load of an empty struct.
  if (RequiredElts.none())
    return false;

  bool FoundSomeValues = false;
  SILBasicBlock *InstBB = StartingFrom->getParent();

  // If there is a potential modification in the current block, scan the block
  // to see if the store or escape is before or after the load.  If it is
  // before, check to see if it produces the value we are looking for.
  if (PerBlockInfo[InstBB].HasNonLoadUse) {
    for (SILBasicBlock::iterator BBI = StartingFrom, E = InstBB->begin();
         BBI != E;) {
      SILInstruction *TheInst = --BBI;

      // If this instruction is unrelated to the element, ignore it.
      if (NonLoadUses.count(TheInst)) {
        FoundSomeValues |= updateAvailableValues(TheInst, RequiredElts, Result);

        // If this satisfied all of the demanded values, we're done.
        if (RequiredElts.none())
          return !FoundSomeValues;

        // Otherwise, keep scanning the block.
      }
    }
  }


  // Otherwise, we need to scan up the CFG looking for available values.
  // TODO: Implement this, for now we just return failure as though there is
  // nothing available.
  return !FoundSomeValues;
}

static bool anyMissing(unsigned StartSubElt, unsigned NumSubElts,
                       ArrayRef<std::pair<SILValue, unsigned>> &Values) {
  while (NumSubElts) {
    if (!Values[StartSubElt].first.isValid()) return true;
    ++StartSubElt;
    --NumSubElts;
  }
  return false;
}


/// AggregateAvailableValues - Given a bunch of primitive subelement values,
/// build out the right aggregate type (LoadTy) by emitting tuple and struct
/// instructions as necessary.
static SILValue
AggregateAvailableValues(SILInstruction *Inst, CanType LoadTy,
                         SILValue Address,
                       ArrayRef<std::pair<SILValue, unsigned>> AvailableValues,
                         unsigned FirstElt) {

  // Check to see if the requested value is fully available, as an aggregate.
  // This is a super-common case for single-element structs, but is also a
  // general answer for arbitrary structs and tuples as well.
  if (FirstElt < AvailableValues.size()) {  // #Elements may be zero.
    SILValue FirstVal = AvailableValues[FirstElt].first;
    if (FirstVal.isValid() && AvailableValues[FirstElt].second == 0 &&
        FirstVal.getType().getSwiftRValueType() == LoadTy) {
      // If the first element of this value is available, check any extra ones
      // before declaring success.
      bool AllMatch = true;
      for (unsigned i = 0, e = getNumSubElements(LoadTy); i != e; ++i)
        if (AvailableValues[FirstElt+i].first != FirstVal ||
            AvailableValues[FirstElt+i].second != i) {
          AllMatch = false;
          break;
        }
        
      if (AllMatch)
        return FirstVal;
    }
  }


  SILBuilder B(Inst);

  if (TupleType *TT = LoadTy->getAs<TupleType>()) {
    SmallVector<SILValue, 4> ResultElts;

    unsigned EltNo = 0;
    for (auto &Elt : TT->getFields()) {
      CanType EltTy = Elt.getType()->getCanonicalType();
      unsigned NumSubElt = getNumSubElements(EltTy);

      // If we are missing any of the available values in this struct element,
      // compute an address to load from.
      SILValue EltAddr;
      if (anyMissing(FirstElt, NumSubElt, AvailableValues))
        EltAddr = B.createTupleElementAddr(Inst->getLoc(), Address, EltNo);

      ResultElts.push_back(AggregateAvailableValues(Inst, EltTy, EltAddr,
                                                    AvailableValues, FirstElt));
      FirstElt += NumSubElt;
      ++EltNo;
    }

    return B.createTuple(Inst->getLoc(),
                         SILType::getPrimitiveObjectType(LoadTy),
                         ResultElts);
  }

  // Extract struct elements.
  if (auto *SD = LoadTy->getStructOrBoundGenericStruct()) {
    SmallVector<SILValue, 4> ResultElts;

    for (auto *FD : SD->getStoredProperties()) {
      CanType EltTy = SILBuilder::getStructFieldType(LoadTy, FD);
      unsigned NumSubElt = getNumSubElements(EltTy);

      // If we are missing any of the available values in this struct element,
      // compute an address to load from.
      SILValue EltAddr;
      if (anyMissing(FirstElt, NumSubElt, AvailableValues))
        EltAddr = B.createStructElementAddr(Inst->getLoc(), Address, FD);

      ResultElts.push_back(AggregateAvailableValues(Inst, EltTy, EltAddr,
                                                    AvailableValues, FirstElt));
      FirstElt += NumSubElt;
    }
    return B.createStruct(Inst->getLoc(),
                          SILType::getPrimitiveObjectType(LoadTy),
                          ResultElts);
  }

  // Otherwise, we have a simple primitive.  If the value is available, use it,
  // otherwise emit a load of the value.
  auto Val = AvailableValues[FirstElt];
  if (!Val.first.isValid())
    return B.createLoad(Inst->getLoc(), Address);

  SILValue EltVal = ExtractSubElement(Val.first, Val.second, B, Inst->getLoc());
  // It must be the same type as LoadTy if available.
  assert(EltVal.getType().getSwiftRValueType() == LoadTy &&
         "Subelement types mismatch");
  return EltVal;
}


/// At this point, we know that this element satisfies the definitive init
/// requirements, so we can try to promote loads to enable SSA-based dataflow
/// analysis.  We know that accesses to this element only access this element,
/// cross element accesses have been scalarized.
///
void ElementPromotion::promoteLoad(SILInstruction *Inst) {
  // Note that we intentionally don't support forwarding of weak pointers,
  // because the underlying value may drop be deallocated at any time.  We would
  // have to prove that something in this function is holding the weak value
  // live across the promoted region and that isn't desired for a stable
  // diagnostics pass this like one.


  // We only handle load right now, not copy_addr.
  if (!isa<LoadInst>(Inst)) return;

  // If the box has escaped at this instruction, we can't safely promote the
  // load.
  if (hasEscapedAt(Inst))
    return;

  CanType LoadTy = Inst->getType(0).getSwiftRValueType();

  // If this is a load from a struct field that we want to promote, compute the
  // access path down to the field so we can determine precise def/use behavior.
  unsigned FirstElt = ComputeAccessPath(Inst->getOperand(0), TheMemory);

  // Set up the bitvector of elements being demanded by the load.
  llvm::SmallBitVector RequiredElts(NumMemorySubElements);
  RequiredElts.set(FirstElt, FirstElt+getNumSubElements(LoadTy));

  SmallVector<std::pair<SILValue, unsigned>, 8> AvailableValues;
  AvailableValues.resize(NumMemorySubElements);

  // If there are no values available at this load point, then we fail to
  // promote this load and there is nothing to do.
  if (computeAvailableValues(Inst, RequiredElts, AvailableValues))
    return;

  // Verify that we actually got some values back when computeAvailableValues
  // claims it produced them.
#ifndef NDEBUG
  {bool AnyAvailable = getNumSubElements(LoadTy) == 0;
    for (unsigned i = 0, e = AvailableValues.size(); i != e; ++i)
      AnyAvailable |= AvailableValues[i].first.isValid();
    assert(AnyAvailable && "Didn't get any available values!");
  }
#endif

  // Ok, we have some available values.  Aggregate together all of the
  // subelements into something that has the same type as the load did, and emit
  // (smaller) loads for any subelements that were not available.
  auto NewVal = AggregateAvailableValues(Inst, LoadTy, Inst->getOperand(0),
                                         AvailableValues, FirstElt);

  DEBUG(llvm::errs() << "  *** Promoting load: " << *Inst << "\n");
  DEBUG(llvm::errs() << "      To value: " << *NewVal.getDef() << "\n");

  SILValue(Inst, 0).replaceAllUsesWith(NewVal);
  SILValue Addr = Inst->getOperand(0);
  Inst->eraseFromParent();
  RemoveDeadAddressingInstructions(Addr);
  ++NumLoadPromoted;
}


/// Explode a copy_addr instruction of a loadable type into lower level
/// operations like loads, stores, retains, releases, copy_value, etc.  This
/// returns the first instruction of the generated sequence.
void ElementPromotion::explodeCopyAddr(CopyAddrInst *CAI,
                                       SILValue &StoredValue) {
  SILType ValTy = CAI->getDest().getType().getObjectType();
  auto &TL = CAI->getModule().getTypeLowering(ValTy);

  // Keep track of the new instructions emitted.
  SmallVector<SILInstruction*, 4> NewInsts;
  SILBuilder B(CAI, &NewInsts);


  // Use type lowering to lower the copyaddr into a load sequence + store
  // sequence appropriate for the type.
  StoredValue = TL.emitLoadOfCopy(B, CAI->getLoc(), CAI->getSrc(),
                                  CAI->isTakeOfSrc());

  TL.emitStoreOfCopy(B, CAI->getLoc(), StoredValue, CAI->getDest(),
                     CAI->isInitializationOfDest());


  // Next, remove the copy_addr itself.
  CAI->eraseFromParent();

  // Update our internal state for this being gone.
  NonLoadUses.erase(CAI);

  // Remove the copy_addr from Uses.  A single copy_addr can appear multiple
  // times if the source and dest are to elements within a single aggregate, but
  // we only want to pick up the CopyAddrKind from the store.
  UseKind CopyAddrKind = Release;
  for (auto &Use : Uses) {
    if (Use.first == CAI) {
      Use.first = nullptr;

      if (Use.second != UseKind::Load)
        CopyAddrKind = Use.second;

      // Keep scanning in case the copy_addr appears multiple times.
    }
  }

  assert(CopyAddrKind != Release && "Didn't find entry for copyaddr?");
  assert((CopyAddrKind == Store || CopyAddrKind == PartialStore) &&
         "Expected copy_addrs that store");


  // Now that we've emitted a bunch of instructions, including a load and store
  // but also including other stuff, update the internal state of
  // ElementPromotion to reflect them.

  // Update the instructions that touch the memory.  NewInst can grow as this
  // iterates, so we can't use a foreach loop.
  for (unsigned i = 0; i != NewInsts.size(); ++i) {
    auto *NewInst = NewInsts[i];

    switch (NewInst->getKind()) {
    default:
      NewInst->dump();
      assert(0 && "Unknown instruction generated by copy_addr lowering");


    case ValueKind::StoreInst:
      Uses.push_back({ NewInst, CopyAddrKind });
      NonLoadUses.insert(NewInst);
      continue;

    case ValueKind::LoadInst: {
#if 0
      auto *LI = cast<LoadInst>(NewInst);

      // If this is the load of the input, ignore it.  Note that copy_addrs can
      // have both their input and result in the same memory object.
      AccessPathTy NewLoadAccessPath;
      if (!TryComputingAccessPath(LI->getOperand(), NewLoadAccessPath,
                                  TheMemory))
        continue;

      // If the copy addr was of an aggregate type (a struct or tuple), we want
      // to make sure to scalarize the load completely to make store->load
      // forwarding simple.
      if (isStructOrTupleToScalarize(LI->getType())) {
        // Scalarize LoadInst.  Compute the addresses of the elements, then
        // scalarize it into smaller loads.
        SmallVector<SILInstruction*, 4> ElementAddrs;
        getScalarizedElementAddresses(LI->getOperand(), ElementAddrs);

        SmallVector<SILInstruction*, 4> NewLoads;
        SILValue Result = scalarizeLoad(LI, ElementAddrs, &NewLoads);
        SILValue(LI, 0).replaceAllUsesWith(Result);
        LI->eraseFromParent();

        // Make sure we process the newly generated loads.  They may need to be
        // recursively scalarized and need to be registered as uses.
        NewLoads.append(NewLoads.begin(), NewLoads.end());
        continue;
      }
#endif

      // If it is a load from the memory object, track it as an access.
      Uses.push_back({ NewInst, Load });

      continue;
    }

    case ValueKind::CopyValueInst:
    case ValueKind::StrongRetainInst:
    case ValueKind::StrongReleaseInst:
    case ValueKind::UnownedRetainInst:
    case ValueKind::UnownedReleaseInst:
    case ValueKind::DestroyValueInst:   // Destroy overwritten value
      // These are ignored.
      continue;
    }
  }
}





//===----------------------------------------------------------------------===//
//                          Top Level Driver
//===----------------------------------------------------------------------===//

namespace {
  class ElementUseCollector {
    SmallVectorImpl<ElementUses> &Uses;

    /// When walking the use list, if we index into a struct element, keep track
    /// of this, so that any indexes into tuple subelements don't affect the
    /// element we attribute an access to.
    bool InStructSubElement = false;

    /// When walking the use list, if we index into an enum slice, keep track
    /// of this.
    bool InEnumSubElement = false;
  public:
    ElementUseCollector(SmallVectorImpl<ElementUses> &Uses)
      : Uses(Uses) {
    }

    /// This is the main entry point for the use walker.
    void collectUses(SILValue Pointer, unsigned BaseElt);
    
  private:
    void addElementUses(unsigned BaseElt, SILType UseTy,
                        SILInstruction *User, UseKind Kind);
    void collectElementUses(SILInstruction *ElementPtr, unsigned BaseElt);
  };
  
  
} // end anonymous namespace

/// addElementUses - An operation (e.g. load, store, inout use, etc) on a value
/// acts on all of the aggregate elements in that value.  For example, a load
/// of $*(Int,Int) is a use of both Int elements of the tuple.  This is a helper
/// to keep the Uses data structure up to date for aggregate uses.
void ElementUseCollector::addElementUses(unsigned BaseElt, SILType UseTy,
                                         SILInstruction *User, UseKind Kind) {
  // If we're in a subelement of a struct or enum, just mark the struct, not
  // things that come after it in a parent tuple.
  unsigned Slots = 1;
  if (!InStructSubElement && !InEnumSubElement)
    Slots = getTupleElementCount(UseTy.getSwiftRValueType());
  
  for (unsigned i = 0; i != Slots; ++i)
    Uses[BaseElt+i].push_back({ User, Kind });
}

/// Given a tuple_element_addr or struct_element_addr, compute the new BaseElt
/// implicit in the selected member, and recursively add uses of the
/// instruction.
void ElementUseCollector::
collectElementUses(SILInstruction *ElementPtr, unsigned BaseElt) {
  // struct_element_addr P, #field indexes into the current element.
  if (auto *SEAI = dyn_cast<StructElementAddrInst>(ElementPtr)) {
    // Set the "InStructSubElement" flag and recursively process the uses.
    llvm::SaveAndRestore<bool> X(InStructSubElement, true);
    collectUses(SILValue(SEAI, 0), BaseElt);
    return;
  }

  auto *TEAI = cast<TupleElementAddrInst>(ElementPtr);

  // If we're walking into a tuple within a struct, don't adjust the BaseElt.
  // the uses hanging off the tuple_element_addr are going to be counted as uses
  // of the struct itself.
  if (InStructSubElement)
    return collectUses(SILValue(TEAI, 0), BaseElt);

  // tuple_element_addr P, 42 indexes into the current element.  Recursively
  // process its uses with the adjusted element number.
  unsigned FieldNo = TEAI->getFieldNo();
  auto *TT = TEAI->getTupleType();
  unsigned NewBaseElt = BaseElt;
  for (unsigned i = 0; i != FieldNo; ++i) {
    CanType EltTy = TT->getElementType(i)->getCanonicalType();
    NewBaseElt += getTupleElementCount(EltTy);
  }
  
  collectUses(SILValue(TEAI, 0), NewBaseElt);
}


void ElementUseCollector::collectUses(SILValue Pointer, unsigned BaseElt) {
  assert(Pointer.getType().isAddress() &&
         "Walked through the pointer to the value?");
  SILType PointeeType = Pointer.getType().getObjectType();

  /// This keeps track of instructions in the use list that touch multiple
  /// elements and should be scalarized.  This is done as a second phase to
  /// avoid invalidating the use iterator.
  ///
  SmallVector<SILInstruction*, 4> UsesToScalarize;
  
  for (auto UI : Pointer.getUses()) {
    auto *User = cast<SILInstruction>(UI->getUser());

    // Instructions that compute a subelement are handled by a helper.
    if (isa<TupleElementAddrInst>(User) || isa<StructElementAddrInst>(User)) {
      collectElementUses(User, BaseElt);
      continue;
    }
    
    // Loads are a use of the value.
    if (isa<LoadInst>(User)) {
      if (PointeeType.is<TupleType>())
        UsesToScalarize.push_back(User);
      else
        Uses[BaseElt].push_back({User, UseKind::Load});
      continue;
    }

    if (isa<LoadWeakInst>(User)) {
      Uses[BaseElt].push_back({User, UseKind::Load});
      continue;
    }

    // Stores *to* the allocation are writes.
    if ((isa<StoreInst>(User) || isa<AssignInst>(User) ||
         isa<StoreWeakInst>(User)) &&
        UI->getOperandNumber() == 1) {
      // We only scalarize aggregate stores of tuples to their
      // elements, we do not scalarize stores of structs to their elements.
      if (PointeeType.is<TupleType>()) {
        assert(!isa<StoreWeakInst>(User) &&
               "Can't weak store a struct or tuple");
        UsesToScalarize.push_back(User);
      } else {
        auto Kind = InStructSubElement ? UseKind::PartialStore : UseKind::Store;
        Uses[BaseElt].push_back({ User, Kind });
      }
      continue;
    }

    if (isa<CopyAddrInst>(User)) {
      // If this is the source of the copy_addr, then this is a load.  If it is
      // the destination, then this is a store.  Note that we'll revisit this
      // instruction and add it to Uses twice if it is both a load and store to
      // the same aggregate.
      auto Kind = InStructSubElement ? UseKind::PartialStore : UseKind::Store;
      if (UI->getOperandNumber() == 0) Kind = UseKind::Load;
      addElementUses(BaseElt, PointeeType, User, Kind);
      continue;
    }
    
    // Initializations are definitions of the whole thing.  This is currently
    // used in constructors and should go away someday.
    if (isa<InitializeVarInst>(User)) {
      auto Kind = InStructSubElement ? UseKind::PartialStore : UseKind::Store;
      addElementUses(BaseElt, PointeeType, User, Kind);
      continue;
    }

    // The apply instruction does not capture the pointer when it is passed
    // through [inout] arguments or for indirect returns.  InOut arguments are
    // treated as uses and may-store's, but an indirect return is treated as a
    // full store.
    //
    // Note that partial_apply instructions always close over their argument.
    //
    if (auto *Apply = dyn_cast<ApplyInst>(User)) {
      SILType FnTy = Apply->getSubstCalleeType();
      
      SILFunctionType *FTI = FnTy.getFunctionTypeInfo(Apply->getModule());
      unsigned ArgumentNumber = UI->getOperandNumber()-1;

      auto Param = FTI->getParameters()[ArgumentNumber];

      // If this is an indirect return slot, it is a store.
      if (Param.isIndirectResult()) {
        assert(!InStructSubElement && "We're initializing sub-members?");
        addElementUses(BaseElt, PointeeType, User, UseKind::Store);
        continue;
      }

      // Otherwise, check for @inout.
      if (Param.isIndirectInOut()) {
        addElementUses(BaseElt, PointeeType, User, UseKind::InOutUse);
        continue;
      }

      // Otherwise, it is an escape.
    }

    // enum_data_addr is treated like a tuple_element_addr or other instruction
    // that is looking into the memory object (i.e., the memory object needs to
    // be explicitly initialized by a copy_addr or some other use of the
    // projected address).
    if (isa<EnumDataAddrInst>(User)) {
      assert(!InStructSubElement && !InEnumSubElement &&
             "enum_data_addr shouldn't apply to subelements");
      // Keep track of the fact that we're inside of an enum.  This informs our
      // recursion that tuple
      llvm::SaveAndRestore<bool> X(InEnumSubElement, true);
      collectUses(SILValue(User, 0), BaseElt);
      continue;
    }

    // init_existential is modeled as an initialization store, where the uses
    // are treated as subelement accesses.
    if (isa<InitExistentialInst>(User)) {
      assert(!InStructSubElement && !InEnumSubElement &&
             "init_existential should not apply to subelements");
      Uses[BaseElt].push_back({ User, UseKind::Store });
      
      // Set the "InStructSubElement" flag (so we don't consider stores to be
      // full definitions) and recursively process the uses.
      llvm::SaveAndRestore<bool> X(InStructSubElement, true);
      collectUses(SILValue(User, 0), BaseElt);
      continue;
    }

    // inject_enum_addr is treated as a store unconditionally.
    if (isa<InjectEnumAddrInst>(User)) {
      assert(!InStructSubElement &&
             "inject_enum_addr the subelement of a struct unless in a ctor");
      Uses[BaseElt].push_back({ User, UseKind::Store });
      continue;
    }

    // upcast_existential is modeled as a load or store depending on which
    // operand we're looking at.
    if (isa<UpcastExistentialInst>(User)) {
      if (UI->getOperandNumber() == 1)
        Uses[BaseElt].push_back({ User, UseKind::Store });
      else
        Uses[BaseElt].push_back({ User, UseKind::Load });
      continue;
    }

    // project_existential is a use of the protocol value, so it is modeled as a
    // load.
    if (isa<ProjectExistentialInst>(User) || isa<ProtocolMethodInst>(User)) {
      Uses[BaseElt].push_back({User, UseKind::Load});
      // TODO: Is it safe to ignore all uses of the project_existential?
      continue;
    }

    // Otherwise, the use is something complicated, it escapes.
    addElementUses(BaseElt, PointeeType, User, UseKind::Escape);
  }

  // Now that we've walked all of the immediate uses, scalarize any elements
  // that we need to for canonicalization or analysis reasons.
  if (!UsesToScalarize.empty()) {
    SmallVector<SILInstruction*, 4> ElementAddrs;
    getScalarizedElementAddresses(Pointer, ElementAddrs);
    
    SmallVector<SILValue, 4> ElementTmps;
    for (auto *User : UsesToScalarize) {
      ElementTmps.clear();

      DEBUG(llvm::errs() << "  *** Scalarizing: " << *User << "\n");

      // Scalarize LoadInst
      if (auto *LI = dyn_cast<LoadInst>(User)) {
        SILValue Result = scalarizeLoad(LI, ElementAddrs);
        SILValue(LI, 0).replaceAllUsesWith(Result);
        LI->eraseFromParent();
        continue;
      }

      SILBuilder B(User);

      // Scalarize AssignInst
      if (auto *AI = dyn_cast<AssignInst>(User)) {
        getScalarizedElements(AI->getOperand(0), ElementTmps, AI->getLoc(), B);

        for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i)
          B.createAssign(AI->getLoc(), ElementTmps[i], ElementAddrs[i]);
        AI->eraseFromParent();
        continue;
      }
      
      // Scalarize StoreInst
      auto *SI = cast<StoreInst>(User);
      getScalarizedElements(SI->getOperand(0), ElementTmps, SI->getLoc(), B);
      
      for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i)
        B.createStore(SI->getLoc(), ElementTmps[i], ElementAddrs[i]);
      SI->eraseFromParent();
    }
    
    // Now that we've scalarized some stuff, recurse down into the newly created
    // element address computations to recursively process it.  This can cause
    // further scalarization.
    for (auto EltPtr : ElementAddrs)
      collectElementUses(EltPtr, BaseElt);
  }
}


static void processAllocBox(AllocBoxInst *ABI) {
  DEBUG(llvm::errs() << "*** Definite Init looking at: " << *ABI << "\n");

  // Set up the datastructure used to collect the uses of the alloc_box.  The
  // uses are bucketed up into the elements of the allocation that are being
  // used.  This matters for element-wise tuples and fragile structs.
  SmallVector<ElementUses, 1> Uses;
  Uses.resize(getTupleElementCount(ABI->getElementType().getSwiftRValueType()));

  // Walk the use list of the pointer, collecting them into the Uses array.
  ElementUseCollector(Uses).collectUses(SILValue(ABI, 1), 0);

  // Collect information about the retain count result as well.
  for (auto UI : SILValue(ABI, 0).getUses()) {
    auto *User = cast<SILInstruction>(UI->getUser());

    // If this is a release, then remember it as such.
    if (isa<StrongReleaseInst>(User)) {
      for (auto &UseArray : Uses)
        UseArray.push_back({ User, UseKind::Release });
    }
  }

  // Process each scalar value in the uses array individually.
  unsigned EltNo = 0;
  for (auto &Elt : Uses)
    ElementPromotion(ABI, EltNo++, Elt).doIt();
}

static void processAllocStack(AllocStackInst *ASI) {
  DEBUG(llvm::errs() << "*** Definite Init looking at: " << *ASI << "\n");

  // Set up the datastructure used to collect the uses of the alloc_box.  The
  // uses are bucketed up into the elements of the allocation that are being
  // used.  This matters for element-wise tuples and fragile structs.
  SmallVector<ElementUses, 1> Uses;
  Uses.resize(getTupleElementCount(ASI->getElementType().getSwiftRValueType()));
  
  // Walk the use list of the pointer, collecting them into the Uses array.
  ElementUseCollector(Uses).collectUses(SILValue(ASI, 1), 0);
  
  // Collect information about the retain count result as well.
  for (auto UI : SILValue(ASI, 0).getUses()) {
    auto *User = cast<SILInstruction>(UI->getUser());
    
    // If this is a release or dealloc_stack, then remember it as such.
    if (isa<StrongReleaseInst>(User) || isa<DeallocStackInst>(User)) {
      for (auto &UseArray : Uses)
        UseArray.push_back({ User, UseKind::Release });
    }
  }
  
  // Process each scalar value in the uses array individually.
  unsigned EltNo = 0;
  for (auto &Elt : Uses)
    ElementPromotion(ASI, EltNo++, Elt).doIt();
}

static void processMarkUninitialized(MarkUninitializedInst *MUI) {
  DEBUG(llvm::errs() << "*** Definite Init looking at: " << *MUI << "\n");
  
  // Set up the datastructure used to collect the uses of the
  // mark_uninitialized.  The uses are bucketed up into the elements of the
  // allocation that are being used.  This matters for element-wise tuples and
  // fragile structs.
  SmallVector<ElementUses, 1> Uses;
  Uses.resize(getTupleElementCount(MUI->getType().getObjectType()
                                     .getSwiftRValueType()));
  
  // Walk the use list of the pointer, collecting them into the Uses array.
  ElementUseCollector(Uses).collectUses(SILValue(MUI, 0), 0);
  
  // Process each scalar value in the uses array individually.
  unsigned EltNo = 0;
  for (auto &Elt : Uses)
    ElementPromotion(MUI, EltNo++, Elt).doIt();
}


/// checkDefiniteInitialization - Check that all memory objects that require
/// initialization before use are properly set and transform the code as
/// required for flow-sensitive properties.
static void checkDefiniteInitialization(SILFunction &Fn) {
  for (auto &BB : Fn) {
    auto I = BB.begin(), E = BB.end();
    while (I != E) {
      if (auto *ABI = dyn_cast<AllocBoxInst>(I)) {
        processAllocBox(ABI);
        
        // Carefully move iterator to avoid invalidation problems.
        ++I;
        if (ABI->use_empty())
          ABI->eraseFromParent();
        continue;
      }

      if (auto *ASI = dyn_cast<AllocStackInst>(I))
        processAllocStack(ASI);

      if (auto *MUI = dyn_cast<MarkUninitializedInst>(I))
        processMarkUninitialized(MUI);

      ++I;
    }
  }
}

/// lowerRawSILOperations - There are a variety of raw-sil instructions like
/// 'assign' that are only used by this pass.  Now that definite initialization
/// checking is done, remove them.
static void lowerRawSILOperations(SILFunction &Fn) {
  for (auto &BB : Fn) {
    auto I = BB.begin(), E = BB.end();
    while (I != E) {
      SILInstruction *Inst = I++;
      
      // Unprocessed assigns just lower into assignments, not initializations.
      if (auto *AI = dyn_cast<AssignInst>(Inst)) {
        SILBuilder B(AI);
        LowerAssignInstruction(B, AI, false);
        // Assign lowering may split the block. If it did,
        // reset our iteration range to the block after the insertion.
        if (B.getInsertionBB() != &BB)
          I = E;
        continue;
      }

      // mark_uninitialized just becomes a noop, resolving to its operand.
      if (auto *MUI = dyn_cast<MarkUninitializedInst>(Inst)) {
        SILValue(MUI, 0).replaceAllUsesWith(MUI->getOperand());
        MUI->eraseFromParent();
        continue;
      }
      
      // mark_function_escape just gets zapped.
      if (isa<MarkFunctionEscapeInst>(Inst)) {
        Inst->eraseFromParent();
        continue;
      }
    }
  }
}


/// performSILDefiniteInitialization - Perform definitive initialization
/// analysis and promote alloc_box uses into SSA registers for later SSA-based
/// dataflow passes.
void swift::performSILDefiniteInitialization(SILModule *M) {
  for (auto &Fn : *M) {
    // Walk through and promote all of the alloc_box's that we can.
    checkDefiniteInitialization(Fn);

    if (EnableCopyAddrForwarding)
      Fn.dump();

    // Lower raw-sil only instructions used by this pass, like "assign".
    lowerRawSILOperations(Fn);
  }
}


