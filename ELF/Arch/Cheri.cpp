#include "Cheri.h"
#include "../OutputSections.h"
#include "../SymbolTable.h"
#include "../SyntheticSections.h"
#include "../Target.h"
#include "lld/Common/Memory.h"
#include "llvm/Support/Path.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;

namespace lld {

namespace elf {
template <class ELFT>
CheriCapRelocsSection<ELFT>::CheriCapRelocsSection()
    : SyntheticSection(SHF_ALLOC, SHT_PROGBITS, 8, "__cap_relocs") {
  this->Entsize = RelocSize;
}

// TODO: copy MipsABIFlagsSection::create() instead of current impl?
template <class ELFT>
void CheriCapRelocsSection<ELFT>::addSection(InputSectionBase *S) {
  // FIXME: can this happen with ld -r ?
  // error("Compiler should not have generated __cap_relocs section for " + toString(S));
  assert(S->Name == "__cap_relocs");
  assert(S->AreRelocsRela && "__cap_relocs should be RELA");
  // make sure the section is no longer processed
  S->Live = false;

  if ((S->getSize() % Entsize) != 0) {
    error("__cap_relocs section size is not a multiple of " + Twine(Entsize) +
          ": " + toString(S));
    return;
  }
  size_t NumCapRelocs = S->getSize() / RelocSize;
  if (NumCapRelocs * 2 != S->NumRelocations) {
    error("expected " + Twine(NumCapRelocs * 2) + " relocations for " +
          toString(S) + " but got " + Twine(S->NumRelocations));
    return;
  }
  if (Config->VerboseCapRelocs)
    message("Adding cap relocs from " + toString(S->File) + "\n");

  ContainsLegacyCapRelocs = true; // reduce number of warnings for compat
  processSection(S);
}

template <class ELFT> void CheriCapRelocsSection<ELFT>::finalizeContents() {
  // TODO: sort by address for improved cache behaviour?
  // TODO: add the dynamic relocations here:
  //   for (const auto &I : RelocsMap) {
  //     // TODO: unresolved symbols -> add dynamic reloc
  //     const CheriCapReloc& Reloc = I.second;
  //     Symbol *LocationSym = I.first.first;
  //     uint64_t LocationOffset = I.first.second;
  //
  //   }
}

static inline void nonFatalWarning(const Twine &Str) {
  if (errorHandler().FatalWarnings)
    message("warning: " + Str);
  else
    warn(Str);
}

static SymbolAndOffset sectionWithOffsetToSymbol(InputSectionBase *IS,
                                                 uint64_t Offset, Symbol *Src) {
  Symbol *FallbackResult = nullptr;
  assert((int64_t)Offset >= 0);
  uint64_t FallbackOffset = Offset;
  // For internal symbols we don't have a matching InputFile, just return
  auto* File = IS->File;
  if (!File)
    return {Src, (int64_t)Offset};
  for (Symbol *B : IS->File->getSymbols()) {
    if (auto *D = dyn_cast<Defined>(B)) {
      if (D->Section != IS)
        continue;
      if (D->Value <= Offset && Offset < D->Value + D->Size) {
        // XXXAR: should we accept any symbol that encloses or only exact
        // matches?
        if (D->Value == Offset && (D->isFunc() || D->isObject()))
          return {D, 0}; // perfect match
        FallbackResult = D;
        FallbackOffset = Offset - D->Value;
      }
    }
  }
  // When using the legacy __cap_relocs style (where clang emits __cap_relocs
  // instead of R_CHERI_CAPABILITY) the local symbols might not exist so we
  // may have fall back to the section.
  if (!FallbackResult) {
    // worst case we fall back to the section + offset
    // Don't warn if the relocation is against an anonymous string constant
    // since clang won't emit a symbol (and no size) for those
    if (!IS->Name.startswith(".rodata.str"))
      nonFatalWarning("Could not find a real symbol for __cap_reloc against " + IS->Name +
           "+0x" + utohexstr(Offset) + " in " + toString(Src->File));
    FallbackResult = Src;
  }
  // we should have found at least a section symbol
  assert(FallbackResult && "SHOULD HAVE FOUND A SYMBOL!");
  return {FallbackResult, (int64_t)FallbackOffset};
}

SymbolAndOffset SymbolAndOffset::findRealSymbol() const {
  if (!Symbol->isSection())
    return *this;

  if (Defined *DefinedSym = dyn_cast<Defined>(Symbol)) {
    if (auto *IS = dyn_cast<InputSectionBase>(DefinedSym->Section)) {
      return sectionWithOffsetToSymbol(IS, Offset, Symbol);
    }
  }
  return *this;
}

template <class ELFT>
void CheriCapRelocsSection<ELFT>::processSection(InputSectionBase *S) {
  constexpr endianness E = ELFT::TargetEndianness;
  // TODO: sort by offset (or is that always true?
  const auto Rels = S->relas<ELFT>();
  for (auto I = Rels.begin(), End = Rels.end(); I != End; ++I) {
    const auto &LocationRel = *I;
    ++I;
    const auto &TargetRel = *I;
    if ((LocationRel.r_offset % Entsize) != 0) {
      error("corrupted __cap_relocs:  expected Relocation offset to be a "
            "multiple of " +
            Twine(Entsize) + " but got " + Twine(LocationRel.r_offset));
      return;
    }
    if (TargetRel.r_offset != LocationRel.r_offset + 8) {
      error("corrupted __cap_relocs: expected target relocation (" +
            Twine(TargetRel.r_offset) +
            " to directly follow location relocation (" +
            Twine(LocationRel.r_offset) + ")");
      return;
    }
    if (LocationRel.r_addend < 0) {
      error("corrupted __cap_relocs: addend is less than zero in" +
            toString(S) + ": " + Twine(LocationRel.r_addend));
      return;
    }
    uint64_t CapRelocsOffset = LocationRel.r_offset;
    assert(CapRelocsOffset + Entsize <= S->getSize());
    if (LocationRel.getType(Config->IsMips64EL) != R_MIPS_64) {
      error("Exptected a R_MIPS_64 relocation in __cap_relocs but got " +
            toString(LocationRel.getType(Config->IsMips64EL)));
      continue;
    }
    if (TargetRel.getType(Config->IsMips64EL) != R_MIPS_64) {
      error("Exptected a R_MIPS_64 relocation in __cap_relocs but got " +
            toString(LocationRel.getType(Config->IsMips64EL)));
      continue;
    }
    Symbol *LocationSym =
        &S->getFile<ELFT>()->getRelocTargetSym(LocationRel);
    Symbol &TargetSym = S->getFile<ELFT>()->getRelocTargetSym(TargetRel);

    if (LocationSym->File != S->File) {
      error("Expected capability relocation to point to " + toString(S->File) +
            " but got " + toString(LocationSym->File));
      continue;
    }
    //    errs() << "Adding cap reloc at " << toString(LocationSym) << " type "
    //           << Twine((int)LocationSym.Type) << " against "
    //           << toString(TargetSym) << "\n";
    auto *RawInput = reinterpret_cast<const InMemoryCapRelocEntry<E> *>(
        S->Data.begin() + CapRelocsOffset);
    int64_t TargetCapabilityOffset = (int64_t)RawInput->offset;
    assert(RawInput->size == 0 && "Clang should not have set size in __cap_relocs");
    if (!isa<Defined>(LocationSym)) {
      error("Unhandled symbol kind for cap_reloc: " +
            Twine(LocationSym->kind()));
      continue;
    }

    const SymbolAndOffset RelocLocation{LocationSym, LocationRel.r_addend};
    const SymbolAndOffset RelocTarget{&TargetSym, TargetRel.r_addend};
    SymbolAndOffset RealLocation = RelocLocation.findRealSymbol();
    SymbolAndOffset RealTarget = RelocTarget.findRealSymbol();
    if (Config->VerboseCapRelocs) {
      message("Adding capability relocation at " +
              verboseToString<ELFT>(RealLocation) + "\nagainst " +
              verboseToString<ELFT>(RealTarget));
    }

    bool TargetNeedsDynReloc = false;
    if (TargetSym.IsPreemptible) {
      // Do we need this?
      // TargetNeedsDynReloc = true;
    }
    switch (TargetSym.kind()) {
    case Symbol::DefinedKind:
      break;
    case Symbol::SharedKind:
      if (!Config->Shared && !InX::Interp) {
        error("cannot create a capability relocation against a shared symbol"
              " when linking statically");
        continue;
      }
      TargetNeedsDynReloc = true;
      break;
    case Symbol::UndefinedKind:
      // addCapReloc() will add an error if we are building an executable
      // instead of a shlib
      // TODO: we really should add a dynamic SIZE relocation as well
      TargetNeedsDynReloc = true;
      break;
    default:
      error("Unhandled symbol kind for cap_reloc target: " +
            Twine(TargetSym.kind()));
      continue;
    }
    assert(LocationSym->isSection());
    auto *LocationDef = cast<Defined>(LocationSym);
    auto *LocationSec = cast<InputSectionBase>(LocationDef->Section);
    addCapReloc({LocationSec, (uint64_t)LocationRel.r_addend, false},
                RealTarget, TargetNeedsDynReloc, TargetCapabilityOffset,
                RealLocation.Symbol);
  }
}

template <class ELFT>
void CheriCapRelocsSection<ELFT>::addCapReloc(CheriCapRelocLocation Loc,
                                              const SymbolAndOffset &Target,
                                              bool TargetNeedsDynReloc,
                                              int64_t CapabilityOffset,
                                              Symbol *SourceSymbol) {
  Loc.NeedsDynReloc = Loc.NeedsDynReloc || Config->Pic || Config->Pie;
  TargetNeedsDynReloc = TargetNeedsDynReloc || Config->Pic || Config->Pie;
  uint64_t CurrentEntryOffset = RelocsMap.size() * RelocSize;

  if (Target.Symbol->isUndefined() && !Target.Symbol->isUndefWeak()) {
    std::string SourceMsg = SourceSymbol
                                ? lld::verboseToString<ELFT>(SourceSymbol)
                                : Loc.toString();
    std::string Msg =
        "cap_reloc against undefined symbol: " + toString(*Target.Symbol) +
        "\n>>> referenced by " + SourceMsg;
    if (Config->UnresolvedSymbols == UnresolvedPolicy::ReportError)
      error(Msg);
    else
      nonFatalWarning(Msg);
  }

  // assert(CapabilityOffset >= 0 && "Negative offsets not supported");
  if (errorHandler().Verbose && CapabilityOffset < 0)
    message("global capability offset " + Twine(CapabilityOffset) +
            " is less than 0:\n>>> Location: " + Loc.toString() +
            "\n>>> Target: " + verboseToString<ELFT>(Target));
  if (!addEntry(Loc, {Target, CapabilityOffset, TargetNeedsDynReloc})) {
    return; // Maybe happens with vtables?
  }
  if (Loc.NeedsDynReloc) {
    // XXXAR: We don't need to create a symbol here since if we pass nullptr
    // to the dynamic reloc it will add a relocation against the load address
#if 0
    llvm::sys::path::filename(Loc.Section->File->getName());
    StringRef Filename = llvm::sys::path::filename(Loc.Section->File->getName());
    std::string SymbolHackName = ("__caprelocs_hack_" + Loc.Section->Name + "_" +
                                  Filename).str();
    auto LocationSym = Symtab->find(SymbolHackName);
    if (!LocationSym) {
        Symtab->addRegular<ELFT>(Saver.save(SymbolHackName), STV_DEFAULT,
                                 STT_OBJECT, Loc.Offset, Config->CapabilitySize,
                                 STB_GLOBAL, Loc.Section, Loc.Section->File);
        LocationSym = Symtab->find(SymbolHackName);
        assert(LocationSym);
    }

    // Needed because local symbols cannot be used in dynamic relocations
    // TODO: do this better
    // message("Adding dyn reloc at " + toString(this) + "+0x" +
    // utohexstr(CurrentEntryOffset))
#endif
    assert(CurrentEntryOffset < getSize());
    // Add a dynamic relocation so that RTLD fills in the right base address
    // We only have the offset relative to the load address...
    // Ideally RTLD/crt_init_globals would just add the load address to all
    // cap_relocs entries that have a RELATIVE flag set instead of requiring a
    // full Elf_Rel/Elf_Rela Can't use RealLocation here because that will
    // usually refer to a local symbol
    InX::RelaDyn->addReloc({elf::Target->RelativeRel, this, CurrentEntryOffset,
                            true, nullptr, static_cast<int64_t>(Loc.Offset)});
  }
  if (TargetNeedsDynReloc) {
    // Capability target is the second field -> offset + 8
    uint64_t OffsetInOutSec = CurrentEntryOffset + 8;
    assert(OffsetInOutSec < getSize());
    // message("Adding dyn reloc at " + toString(this) + "+0x" +
    // utohexstr(OffsetInOutSec) + " against " + toString(TargetSym));

    // If the target is not preemptible we can optimize this to a relative
    // relocation agaist the image base
    bool RelativeToLoadAddress = !Target.Symbol->IsPreemptible;
    // The addend is not used as the offset into the capability here, as we
    // have the offset field in the __cap_relocs for that. The Addend
    // will be zero unless we are targetting a string constant as these
    // don't have a symbol and will be like .rodata.str+0x1234

    // TODO: this should not be relative but use R_MIPS_64 for an absolute value
    // This happens to work for FreeBSD rtld  but is fragile
    InX::RelaDyn->addReloc({elf::Target->RelativeRel, this, OffsetInOutSec,
                                 RelativeToLoadAddress, Target.Symbol,
                                 static_cast<int64_t>(Target.Offset)});
  }
}

template<typename ELFT>
static uint64_t getTargetSize(const CheriCapRelocLocation &Location,
                              const CheriCapReloc &Reloc, bool Strict) {
  uint64_t TargetSize = Reloc.Target.Symbol->getSize();
  if (TargetSize > INT_MAX) {
    error("Insanely large symbol size for " + verboseToString<ELFT>(Reloc.Target) +
          "for cap_reloc at" + Location.toString());
    return 0;
  }
  auto TargetSym = Reloc.Target.Symbol;
  if (TargetSize == 0 && !TargetSym->IsPreemptible) {
    StringRef Name = TargetSym->getName();
    // Section end symbols like __preinit_array_end, etc. should actually be
    // zero size symbol since they are just markers for the end of a section
    // and not usable as a valid pointer
    bool IsSpecialEndSymbol = Name.startswith("__stop_") ||
        (Name.startswith("__") && Name.endswith("_end")) ||
        Name == "end" || Name == "_end" || Name == "etext" ||
        Name == "_etext" || Name == "edata" || Name == "_edata";
    if (IsSpecialEndSymbol)
      return TargetSize;

    // Symbols previously declared as weak can have size 0 (if they then resolve
    // to NULL). For example __preinit_array_start, etc. are generated by the
    // linker as ABS symbols with value 0
    bool IsAbsoluteSym = TargetSym->getOutputSection() == nullptr &&
      TargetSym->File == nullptr;
    if (TargetSym->isUndefWeak() || IsAbsoluteSym)  {
      if (TargetSym->getVA(0) != 0) {
        // All previously weak symbols that resolve to an address should have
        // a size > 0.
        error("Zero size weak/ABS symbol did not resolve to NULL but 0x" +
              utohexstr(TargetSym->getVA(0)) + ": " +
              lld::verboseToString<ELFT>(TargetSym));
      }
      return 0;
    }
    bool WarnAboutUnknownSize = true;
    // currently clang doesn't emit the necessary symbol information for local
    // string constants such as: struct config_opt opts[] = { { ..., "foo" },
    // { ..., "bar" } }; As this pattern is quite common don't warn if the
    // target section is .rodata.str
    if (Defined *DefinedSym =
      dyn_cast<Defined>(TargetSym)) {
      if (DefinedSym->isSection() &&
          DefinedSym->Section->Name.startswith(".rodata.str")) {
        WarnAboutUnknownSize = false;
      }
    }
    // TODO: are there any other cases that can be ignored?

    if (WarnAboutUnknownSize || errorHandler().Verbose) {
      std::string Msg = "could not determine size of cap reloc against " +
           verboseToString<ELFT>(Reloc.Target) + "\n>>> referenced by " +
           Location.toString();
      if (Strict)
        warn(Msg);
      else
        nonFatalWarning(Msg);
    }
    if (OutputSection *OS = TargetSym->getOutputSection()) {
      // For negative offsets use 0 instead (we wan the range of the full symbol in that case)
      int64_t Offset = std::max((int64_t)0, Reloc.Target.Offset);
      uint64_t TargetVA = TargetSym->getVA(Offset);
      assert(TargetVA >= OS->Addr);
      uint64_t OffsetInOS = TargetVA - OS->Addr;
      // Use less-or-equal here to account for __end_foo symbols which point 1 past the section
      assert(OffsetInOS <= OS->Size);
      TargetSize = OS->Size - OffsetInOS;
#if 0
      if (Config->VerboseCapRelocs)
          errs() << " OS OFFSET 0x" << utohexstr(OS->Addr) << "SYM OFFSET 0x"
                 << utohexstr(OffsetInOS) << " SECLEN 0x" << utohexstr(OS->Size)
                 << " -> target size 0x" << utohexstr(TargetSize) << "\n";
#endif
    } else {
      warn("Could not find size for symbol " +
           verboseToString<ELFT>(Reloc.Target) +
           " and could not determine section size. Using 0.");
      // TargetSize = std::numeric_limits<uint64_t>::max();
      return 0;
    }
  }
  return TargetSize;
}

template <class ELFT> void CheriCapRelocsSection<ELFT>::writeTo(uint8_t *Buf) {
  constexpr endianness E = ELFT::TargetEndianness;
  static_assert(RelocSize == sizeof(InMemoryCapRelocEntry<E>),
                "cap relocs size mismatch");
  uint64_t Offset = 0;
  for (const auto &I : RelocsMap) {
    const CheriCapRelocLocation &Location = I.first;
    const CheriCapReloc &Reloc = I.second;
    assert(Location.Offset <= Location.Section->getSize());
    // We write the virtual address of the location in in both static and the
    // shared library case:
    // In the static case we can compute the final virtual address and write it
    // In the dynamic case we write the virtual address relative to the load
    // address and the runtime linker will add the load address to that
    uint64_t OutSecOffset = Location.Section->getOffset(Location.Offset);
    uint64_t LocationVA =
        Location.Section->getOutputSection()->Addr + OutSecOffset;

    // For the target the virtual address the addend is always zero so
    // if we need a dynamic reloc we write zero
    // TODO: would it be more efficient for local symbols to write the DSO VA
    // and add a relocation against the load address?
    // Also this would make llvm-objdump -C more useful because it would
    // actually display the symbol that the relocation is against
    uint64_t TargetVA = Reloc.Target.Symbol->getVA(Reloc.Target.Offset);
    bool PreemptibleDynReloc = Reloc.NeedsDynReloc && Reloc.Target.Symbol->IsPreemptible;
    if (PreemptibleDynReloc) {
      // If we have a relocation against a preemptible symbol (even in the
      // current DSO) we can't compute the virtual address here so we only write
      // the addend
      TargetVA = Reloc.Target.Offset;
    }
    uint64_t TargetOffset = Reloc.CapabilityOffset;
    uint64_t Permissions = Reloc.Target.Symbol->isFunc() ? 1ULL << 63 : 0;
    uint64_t TargetSize = getTargetSize<ELFT>(Location, Reloc, /*Strict=*/!ContainsLegacyCapRelocs);
    // TODO: should we warn about symbols that are out-of-bounds?
    // mandoc seems to do it so I guess we need it
    // if (TargetOffset < 0 || TargetOffset > TargetSize) warn(...);
    InMemoryCapRelocEntry<E> Entry{LocationVA, TargetVA, TargetOffset,
                                   TargetSize, Permissions};
    memcpy(Buf + Offset, &Entry, sizeof(Entry));
    //     if (errorHandler().Verbose) {
    //       errs() << "Added capability reloc: loc=" << utohexstr(LocationVA)
    //              << ", object=" << utohexstr(TargetVA)
    //              << ", offset=" << utohexstr(TargetOffset)
    //              << ", size=" << utohexstr(TargetSize)
    //              << ", permissions=" << utohexstr(Permissions) << "\n";
    //     }
    Offset += RelocSize;
  }
  // Sort the cap_relocs by target address for better cache and TLB locality
  // It also makes it much easier to read the llvm-objdump -C output since it
  // is sorted in a sensible order
  std::stable_sort(reinterpret_cast<InMemoryCapRelocEntry<E>*>(Buf),
            reinterpret_cast<InMemoryCapRelocEntry<E>*>(Buf + Offset),
            [](const InMemoryCapRelocEntry<E>& a, const InMemoryCapRelocEntry<E>& b) {
                return a.capability_location < b.capability_location;
            });
  assert(Offset == getSize() && "Not all data written?");
}


CheriCapTableSection::CheriCapTableSection()
  : SyntheticSection(SHF_ALLOC | SHF_WRITE, /* XXX: actually RELRO */
                     SHT_PROGBITS, Config->CapabilitySize, ".cap_table") {
  assert(Config->CapabilitySize > 0);
  this->Entsize = Config->CapabilitySize;
}

void CheriCapTableSection::writeTo(uint8_t* Buf) {
  // Should be filled with all zeros and crt_init_globals fills it in
  // TODO: fill in the raw bits and use csettag
  (void)Buf;
}

void CheriCapTableSection::addEntry(Symbol &Sym, bool SmallImm) {
  // FIXME: can this be called from multiple threads?
  CapTableIndex Idx;
  Idx.NeedsSmallImm = SmallImm;
  auto it = Entries.insert(std::make_pair(&Sym, Idx));
  if (!it.second) {
    // If it is references by a small immediate relocation we need to update
    // the small immediate flag
    if (SmallImm) {
      it.first->second.NeedsSmallImm = true;
    }
  }
#ifdef DEBUG_CAP_TABLE
  llvm::errs() << "Added symbol " << toString(Sym)
               << " to .cap_table. Total count " << Entries.size() << "\n";
#endif
}

uint32_t CheriCapTableSection::getIndex(const Symbol &Sym) const {
  assert(ValuesAssigned && "getIndex called before index assignment");
  auto it = Entries.find(const_cast<Symbol *>(&Sym));
  assert(it != Entries.end());
  return it->second.Index.getValue();
}

template <class ELFT>
void CheriCapTableSection::assignValuesAndAddCapTableSymbols() {
  uint64_t SmallEntryCount = 0;
  for (auto &it : Entries) {
    // TODO: looping twice is inefficient, we could keep track of the number of
    // small entries during insertion
    if (it.second.NeedsSmallImm) {
      SmallEntryCount++;
    }
  }
  uint32_t AssignedSmallIndexes = 0;
  uint32_t AssignedLargeIndexes = 0;
  for (auto &it : Entries) {
    CapTableIndex &CTI = it.second;
    if (CTI.NeedsSmallImm) {
      assert(AssignedSmallIndexes < SmallEntryCount);
      CTI.Index = AssignedSmallIndexes;
      AssignedSmallIndexes++;
    } else {
      CTI.Index = SmallEntryCount + AssignedLargeIndexes;
      AssignedLargeIndexes++;
    }

    uint32_t Index = *CTI.Index;
    auto Body = it.first;
    // FIXME: locking?
    In<ELFT>::CapRelocs->addCapReloc(
        {InX::CheriCapTable, Index * Config->CapabilitySize, Config->Pic},
        {Body, 0u}, Body->IsPreemptible, 0);
    if (Config->Pic) {
      static bool HasWarned = false;
      if (!HasWarned) {
        HasWarned = true;
        // Avoid failing the build due to on by default fatal-warnings
        nonFatalWarning("cannot add capability table entries for PIC code yet, "
                        "file will be broken!");
      }
    }

    StringRef Name = it.first->getName();
    if (Name.empty())
      continue;
    // TODO: don't add for local symbols? or somehow rename?
    // if (it.first->isLocal())
    //   continue;s


    // Avoid duplicate symbol name errors for unnamed string constants:
    // XXXAR: maybe renumber them instead?
    if (Name.startswith(".L.str"))
      continue;
    // XXXAR: for some reason we sometimes create more than one cap table entry
    // for a given global name, for now just rename the symbol
    // Could possibly happen with local symbols?
    std::string RefName = (Name + "@CAPTABLE").str();
    while (Symtab->find(RefName)) {
      RefName += ".duplicate-name";
    }
    uint64_t Off = Index * Config->CapabilitySize;
    Symtab->addRegular(Saver.save(RefName), STV_HIDDEN, STT_OBJECT, Off,
                       Config->CapabilitySize, STB_LOCAL, this, nullptr);
  }
  assert(AssignedSmallIndexes + AssignedLargeIndexes == Entries.size());
  ValuesAssigned = true;
}

CheriCapTableSection *InX::CheriCapTable;

template class elf::CheriCapRelocsSection<ELF32LE>;
template class elf::CheriCapRelocsSection<ELF32BE>;
template class elf::CheriCapRelocsSection<ELF64LE>;
template class elf::CheriCapRelocsSection<ELF64BE>;

template void
CheriCapTableSection::assignValuesAndAddCapTableSymbols<ELF32LE>();
template void
CheriCapTableSection::assignValuesAndAddCapTableSymbols<ELF32BE>();
template void
CheriCapTableSection::assignValuesAndAddCapTableSymbols<ELF64LE>();
template void
CheriCapTableSection::assignValuesAndAddCapTableSymbols<ELF64BE>();

} // namespace elf
} // namespace lld
