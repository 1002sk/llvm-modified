//===-- X86MemLockMap.cpp - .memlock_map section emission -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "X86MemLockMap.h"
#include "MCTargetDesc/X86BaseInfo.h"
#include "MCTargetDesc/X86MCAsmInfo.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "X86InstrInfo.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbolELF.h"
#include "llvm/Target/TargetMachine.h"

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

bool X86MemLockMap::isImplicitStackAccess(unsigned Opcode) {
  switch (Opcode) {
  case X86::PUSH64r:
  case X86::PUSH64i8:
  case X86::PUSH64i32:
  case X86::PUSHP64r:
  case X86::PUSH2:
  case X86::PUSH2P:
  case X86::PUSHF64:
  case X86::POP64r:
  case X86::POPP64r:
  case X86::POP2:
  case X86::POP2P:
  case X86::POPF64:
  case X86::CALL64pcrel32:
  case X86::CALL64r:
  case X86::CALL64r_NT:
  case X86::CALL64r_ImpCall:
  case X86::RET64:
  case X86::RETI64:
  case X86::LEAVE64:
    return true;
  default:
    return false;
  }
}

bool X86MemLockMap::isCaptured(const AllocaInst *AI) {
  auto [It, Inserted] = CaptureCache.try_emplace(AI, true);
  // Reaching the limit of explored uses counts as a capture.
  if (Inserted)
    It->second = PointerMayBeCaptured(AI, /*ReturnCaptures=*/true);
  return It->second;
}

uint8_t X86MemLockMap::classifyMemOperand(const MachineMemOperand &MMO,
                                          const MachineFunction &MF) {
  if (const PseudoSourceValue *PSV = MMO.getPseudoValue()) {
    switch (PSV->kind()) {
    case PseudoSourceValue::Stack:
    case PseudoSourceValue::FixedStack:
      return StackSlot;
    case PseudoSourceValue::GOT:
    case PseudoSourceValue::ConstantPool:
    case PseudoSourceValue::JumpTable:
      return MMO.isStore() ? OtherPseudoSource : ReadOnly;
    default:
      return OtherPseudoSource;
    }
  }

  const Value *V = MMO.getValue();
  if (!V)
    return NotAlloca;
  SmallVector<const Value *, 4> Objects;
  getUnderlyingObjects(V, Objects);
  if (Objects.empty())
    return NotAlloca;
  uint8_t R = LocalNotCaptured;
  for (const Value *O : Objects) {
    const auto *AI = dyn_cast<AllocaInst>(O);
    if (!AI) {
      // getUnderlyingObjects() returns a value it would have looked through
      // only when it gave up.
      if (isa<GEPOperator, PHINode, SelectInst>(O) ||
          Operator::getOpcode(O) == Instruction::BitCast ||
          Operator::getOpcode(O) == Instruction::AddrSpaceCast)
        R = std::min<uint8_t>(R, Unexpected);
      else
        R = std::min<uint8_t>(R, NotAlloca);
      continue;
    }
    if (AI->getFunction() != &MF.getFunction())
      R = std::min<uint8_t>(R, NotAlloca);
    else if (isCaptured(AI))
      R = std::min<uint8_t>(R, Captured);
  }
  return R;
}

uint8_t X86MemLockMap::classify(const MachineInstr &MI) {
  const MachineFunction &MF = *MI.getMF();
  // An outlined function has no IR of its own to reason about.
  if (MF.isOutlined())
    return Unexpected;

  bool HasExplicit = X86::getFirstAddrOperandIdx(MI) >= 0;
  if (!HasExplicit && isImplicitStackAccess(MI.getOpcode()))
    return StackPointerImplicit;

  if (MI.memoperands_empty())
    return NoMemOperand;
  // An X86 instruction has at most one explicit memory operand, which needs
  // at least one MMO. The implicit accesses of the other instructions (string
  // instructions) need an MMO for each direction.
  if (!HasExplicit) {
    bool HasLoad = false, HasStore = false;
    for (const MachineMemOperand *MMO : MI.memoperands()) {
      HasLoad |= MMO->isLoad();
      HasStore |= MMO->isStore();
    }
    if ((MI.mayLoad() && !HasLoad) || (MI.mayStore() && !HasStore))
      return NoMemOperand;
  }

  // The smallest code of the accesses needing a lock, otherwise the smallest
  // code of the others.
  uint8_t Lock = UINT8_MAX, NoLock = UINT8_MAX;
  for (const MachineMemOperand *MMO : MI.memoperands()) {
    uint8_t R = classifyMemOperand(*MMO, MF);
    uint8_t &Min = needsLock(R) ? Lock : NoLock;
    Min = std::min(Min, R);
  }
  return Lock != UINT8_MAX ? Lock : NoLock;
}

void X86MemLockMap::handleInstruction(const MachineInstr &MI, AsmPrinter &AP) {
  CurMI = &MI;
  CurReason.reset();
  PrefixLabel = nullptr;
  if (MI.mayLoadOrStore() || isImplicitStackAccess(MI.getOpcode()))
    CurReason = classify(MI);
}

/// Whether \p Inst is the TLSDESC call, which reads a descriptor that may be
/// resolved lazily.
static bool isTLSDescCall(const MCInst &Inst) {
  for (const MCOperand &Op : Inst)
    if (Op.isExpr())
      if (const auto *SRE = dyn_cast<MCSymbolRefExpr>(Op.getExpr()))
        if (SRE->getSpecifier() == X86::S_TLSCALL)
          return true;
  return false;
}

bool X86MemLockMap::mayBePadded(const MCInst &Inst, const MCInstrDesc &Desc,
                                const MCInstrInfo &MCII) const {
  // Keep in sync with X86AsmBackend::emitInstructionBegin(). Prefixes added
  // for padding are part of the instruction, so only the nops matter, which
  // go in front of an aligned branch or of the first instruction of a fused
  // pair.
  uint8_t K = AlignedBranchKinds;
  return ((K & X86::AlignBranchJcc) && Desc.isConditionalBranch()) ||
         ((K & X86::AlignBranchJmp) && Desc.isUnconditionalBranch()) ||
         ((K & X86::AlignBranchCall) && Desc.isCall()) ||
         ((K & X86::AlignBranchRet) && Desc.isReturn()) ||
         ((K & X86::AlignBranchIndirect) && Desc.isIndirectBranch()) ||
         ((K & X86::AlignBranchFused) &&
          X86_MC::isFirstMacroFusibleInst(Inst, MCII));
}

void X86MemLockMap::handleMCInst(const MCInst &Inst, AsmPrinter &AP) {
  // Code outside function bodies (e.g. the ASan check functions emitted at the
  // end of the file) has no record, so a lock is taken for all of it.
  if (!CurMI)
    return;
  const MCInstrDesc &Desc = AP.TM.getMCInstrInfo()->get(Inst.getOpcode());
  // The offset of an instruction includes its prefixes, so the label goes in
  // front of the first one.
  if (X86II::isPrefix(Desc.TSFlags)) {
    if (!PrefixLabel) {
      PrefixLabel = AP.OutContext.createTempSymbol("memlock");
      AP.OutStreamer->emitLabel(PrefixLabel);
    }
    return;
  }
  MCSymbol *Label = std::exchange(PrefixLabel, nullptr);

  std::optional<uint8_t> R;
  if (Direct && CurReason)
    // Listed even if the MCInst does not seem to access memory.
    R = std::exchange(CurReason, std::nullopt);
  else if (Desc.mayLoad() || Desc.mayStore() ||
           isImplicitStackAccess(Inst.getOpcode()))
    R = isTLSDescCall(Inst) ? Unexpected : MCLayer;
  if (!R)
    return;
  // The assembler may insert nops between the label and the instruction. Drop
  // the records of the function then, so that a lock is taken for all of it.
  // This does not depend on the mode, so both modes list the same accesses.
  if (mayBePadded(Inst, Desc, *AP.TM.getMCInstrInfo()))
    DropFunction = true;
  if (Mode != MemLockMapMode::All && !needsLock(*R))
    return;

  if (!Label) {
    Label = AP.OutContext.createTempSymbol("memlock");
    AP.OutStreamer->emitLabel(Label);
  }
  Entries[CurMI->getParent()->getSectionID()].push_back(
      {Label, KindInstruction, nullptr, *R});
}

void X86MemLockMap::beginInlineAsm(AsmPrinter &AP) {
  InlineAsmBegin = AP.OutContext.createTempSymbol("memlock_asm");
  AP.OutStreamer->emitLabel(InlineAsmBegin);
}

void X86MemLockMap::endInlineAsm(const MachineInstr *MI, AsmPrinter &AP) {
  MCSymbol *Begin = std::exchange(InlineAsmBegin, nullptr);
  if (!MI || !Begin)
    return;
  MCStreamer &OS = *AP.OutStreamer;
  if (&Begin->getSection() != OS.getCurrentSectionOnly()) {
    AP.OutContext.reportError(
        {}, "-memlock-map: inline assembly must end in the section it starts "
            "in");
    return;
  }
  MCSymbol *End = AP.OutContext.createTempSymbol("memlock_asm_end");
  OS.emitLabel(End);
  Entries[MI->getParent()->getSectionID()].push_back(
      {Begin, KindRange, End, InlineAsm});
}

void X86MemLockMap::endFunction(AsmPrinter &AP) {
  MCStreamer &OS = *AP.OutStreamer;
  StringRef Name = AP.MF->getFunction().getName();
  uint8_t Flags = Mode == MemLockMapMode::All ? FlagAll : 0;

  OS.pushSection();
  // A record for every fragment, also those without memory accesses.
  for (const auto &[ID, Range] : AP.MBBSectionRanges) {
    if (DropFunction)
      break;
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
  CaptureCache.clear();
  CurMI = nullptr;
  CurReason.reset();
  PrefixLabel = nullptr;
  DropFunction = false;
}
