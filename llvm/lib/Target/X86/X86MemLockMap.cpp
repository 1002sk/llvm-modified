//===-- X86MemLockMap.cpp - .memlock_map section emission -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "X86MemLockMap.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbolELF.h"

using namespace llvm;

/// The .memlock_map section associated with \p TextSec.
static MCSection *getMemLockMapSection(MCContext &Ctx,
                                       const MCSection &TextSec) {
  const auto &ElfSec = static_cast<const MCSectionELF &>(TextSec);
  unsigned Flags = ELF::SHF_LINK_ORDER;
  StringRef GroupName;
  if (const MCSymbol *Group = ElfSec.getGroup()) {
    GroupName = Group->getName();
    Flags |= ELF::SHF_GROUP;
  }
  return Ctx.getELFSection(
      ".memlock_map", ELF::SHT_PROGBITS, Flags, 0, GroupName, true,
      ElfSec.getUniqueID(),
      static_cast<const MCSymbolELF *>(TextSec.getBeginSymbol()));
}

// FIXME: Placeholder until the sharing analysis exists. Every access needs a
// lock.
static uint8_t classify(const MachineInstr &MI) {
  if (MI.memoperands_empty())
    return X86MemLockMap::NoMemOperand;
  return X86MemLockMap::Unexpected;
}

void X86MemLockMap::handleInstruction(const MachineInstr &MI, AsmPrinter &AP) {
  if (!MI.mayLoadOrStore())
    return;
  uint8_t R = classify(MI);
  if (Mode != MemLockMapMode::All && !needsLock(R))
    return;
  MCSymbol *Label = AP.OutContext.createTempSymbol("memlock");
  AP.OutStreamer->emitLabel(Label);
  Entries[MI.getParent()->getSectionID()].push_back(
      {Label, KindInstruction, nullptr, R});
}

void X86MemLockMap::endFunction(AsmPrinter &AP) {
  MCStreamer &OS = *AP.OutStreamer;
  StringRef Name = AP.MF->getFunction().getName();
  uint8_t Flags = Mode == MemLockMapMode::All ? FlagAll : 0;

  OS.pushSection();
  // A record for every fragment, also those without memory accesses.
  for (const auto &[ID, Range] : AP.MBBSectionRanges) {
    MCSymbol *Begin = Range.BeginLabel;
    OS.switchSection(
        getMemLockMapSection(AP.OutContext, Begin->getSection()));
    OS.emitInt8(Version);
    OS.emitInt8(Flags);
    OS.emitSymbolValue(Begin, 8);
    OS.emitAbsoluteSymbolDiffAsULEB128(Range.EndLabel, Begin);
    OS.emitULEB128IntValue(Name.size());
    OS.emitBytes(Name);
    auto It = Entries.find(ID);
    ArrayRef<Entry> Es;
    if (It != Entries.end())
      Es = It->second;
    OS.emitULEB128IntValue(Es.size());
    for (const Entry &E : Es) {
      OS.emitAbsoluteSymbolDiffAsULEB128(E.Label, Begin);
      OS.emitInt8(E.Kind);
      if (E.Kind == KindRange)
        OS.emitAbsoluteSymbolDiffAsULEB128(E.End, E.Label);
      OS.emitInt8(E.Reason);
    }
  }
  OS.popSection();
  Entries.clear();
}
