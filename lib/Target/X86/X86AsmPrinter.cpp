//===-- X86AsmPrinter.cpp - Convert X86 LLVM code to AT&T assembly --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to X86 machine code.
//
//===----------------------------------------------------------------------===//

#include "X86AsmPrinter.h"
#include "InstPrinter/X86ATTInstPrinter.h"
#include "X86.h"
#include "X86COFFMachineModuleInfo.h"
#include "X86MachineFunctionInfo.h"
#include "X86TargetMachine.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetOptions.h"
using namespace llvm;

//===----------------------------------------------------------------------===//
// Primitive Helper Functions.
//===----------------------------------------------------------------------===//

/// runOnMachineFunction - Emit the function body.
///
bool X86AsmPrinter::runOnMachineFunction(MachineFunction &MF) {
  SetupMachineFunction(MF);

  if (Subtarget->isTargetCOFF()) {
    bool Intrn = MF.getFunction()->hasInternalLinkage();
    OutStreamer.BeginCOFFSymbolDef(CurrentFnSym);
    OutStreamer.EmitCOFFSymbolStorageClass(Intrn ? COFF::IMAGE_SYM_CLASS_STATIC
                                              : COFF::IMAGE_SYM_CLASS_EXTERNAL);
    OutStreamer.EmitCOFFSymbolType(COFF::IMAGE_SYM_DTYPE_FUNCTION
                                               << COFF::SCT_COMPLEX_TYPE_SHIFT);
    OutStreamer.EndCOFFSymbolDef();
  }

  // Have common code print out the function header with linkage info etc.
  EmitFunctionHeader();

  // Emit the rest of the function body.
  EmitFunctionBody();

  // We didn't modify anything.
  return false;
}

/// printSymbolOperand - Print a raw symbol reference operand.  This handles
/// jump tables, constant pools, global address and external symbols, all of
/// which print to a label with various suffixes for relocation types etc.
static void printSymbolOperand(X86AsmPrinter &P, const MachineOperand &MO,
                               raw_ostream &O) {
  switch (MO.getType()) {
  default: llvm_unreachable("unknown symbol type!");
  case MachineOperand::MO_ConstantPoolIndex:
    O << *P.GetCPISymbol(MO.getIndex());
    P.printOffset(MO.getOffset(), O);
    break;
  case MachineOperand::MO_GlobalAddress: {
    const GlobalValue *GV = MO.getGlobal();

    MCSymbol *GVSym;
    if (MO.getTargetFlags() == X86II::MO_DARWIN_STUB)
      GVSym = P.getSymbolWithGlobalValueBase(GV, "$stub");
    else if (MO.getTargetFlags() == X86II::MO_DARWIN_NONLAZY ||
             MO.getTargetFlags() == X86II::MO_DARWIN_NONLAZY_PIC_BASE ||
             MO.getTargetFlags() == X86II::MO_DARWIN_HIDDEN_NONLAZY_PIC_BASE)
      GVSym = P.getSymbolWithGlobalValueBase(GV, "$non_lazy_ptr");
    else
      GVSym = P.getSymbol(GV);

    // Handle dllimport linkage.
    if (MO.getTargetFlags() == X86II::MO_DLLIMPORT)
      GVSym =
          P.OutContext.GetOrCreateSymbol(Twine("__imp_") + GVSym->getName());

    if (MO.getTargetFlags() == X86II::MO_DARWIN_NONLAZY ||
        MO.getTargetFlags() == X86II::MO_DARWIN_NONLAZY_PIC_BASE) {
      MCSymbol *Sym = P.getSymbolWithGlobalValueBase(GV, "$non_lazy_ptr");
      MachineModuleInfoImpl::StubValueTy &StubSym =
          P.MMI->getObjFileInfo<MachineModuleInfoMachO>().getGVStubEntry(Sym);
      if (StubSym.getPointer() == 0)
        StubSym = MachineModuleInfoImpl::
          StubValueTy(P.getSymbol(GV), !GV->hasInternalLinkage());
    } else if (MO.getTargetFlags() == X86II::MO_DARWIN_HIDDEN_NONLAZY_PIC_BASE){
      MCSymbol *Sym = P.getSymbolWithGlobalValueBase(GV, "$non_lazy_ptr");
      MachineModuleInfoImpl::StubValueTy &StubSym =
          P.MMI->getObjFileInfo<MachineModuleInfoMachO>().getHiddenGVStubEntry(
              Sym);
      if (StubSym.getPointer() == 0)
        StubSym = MachineModuleInfoImpl::
          StubValueTy(P.getSymbol(GV), !GV->hasInternalLinkage());
    } else if (MO.getTargetFlags() == X86II::MO_DARWIN_STUB) {
      MCSymbol *Sym = P.getSymbolWithGlobalValueBase(GV, "$stub");
      MachineModuleInfoImpl::StubValueTy &StubSym =
          P.MMI->getObjFileInfo<MachineModuleInfoMachO>().getFnStubEntry(Sym);
      if (StubSym.getPointer() == 0)
        StubSym = MachineModuleInfoImpl::
          StubValueTy(P.getSymbol(GV), !GV->hasInternalLinkage());
    }

    // If the name begins with a dollar-sign, enclose it in parens.  We do this
    // to avoid having it look like an integer immediate to the assembler.
    if (GVSym->getName()[0] != '$')
      O << *GVSym;
    else
      O << '(' << *GVSym << ')';
    P.printOffset(MO.getOffset(), O);
    break;
  }
  }

  switch (MO.getTargetFlags()) {
  default:
    llvm_unreachable("Unknown target flag on GV operand");
  case X86II::MO_NO_FLAG:    // No flag.
    break;
  case X86II::MO_DARWIN_NONLAZY:
  case X86II::MO_DLLIMPORT:
  case X86II::MO_DARWIN_STUB:
    // These affect the name of the symbol, not any suffix.
    break;
  case X86II::MO_GOT_ABSOLUTE_ADDRESS:
    O << " + [.-" << *P.MF->getPICBaseSymbol() << ']';
    break;
  case X86II::MO_PIC_BASE_OFFSET:
  case X86II::MO_DARWIN_NONLAZY_PIC_BASE:
  case X86II::MO_DARWIN_HIDDEN_NONLAZY_PIC_BASE:
    O << '-' << *P.MF->getPICBaseSymbol();
    break;
  case X86II::MO_TLSGD:     O << "@TLSGD";     break;
  case X86II::MO_TLSLD:     O << "@TLSLD";     break;
  case X86II::MO_TLSLDM:    O << "@TLSLDM";    break;
  case X86II::MO_GOTTPOFF:  O << "@GOTTPOFF";  break;
  case X86II::MO_INDNTPOFF: O << "@INDNTPOFF"; break;
  case X86II::MO_TPOFF:     O << "@TPOFF";     break;
  case X86II::MO_DTPOFF:    O << "@DTPOFF";    break;
  case X86II::MO_NTPOFF:    O << "@NTPOFF";    break;
  case X86II::MO_GOTNTPOFF: O << "@GOTNTPOFF"; break;
  case X86II::MO_GOTPCREL:  O << "@GOTPCREL";  break;
  case X86II::MO_GOT:       O << "@GOT";       break;
  case X86II::MO_GOTOFF:    O << "@GOTOFF";    break;
  case X86II::MO_PLT:       O << "@PLT";       break;
  case X86II::MO_TLVP:      O << "@TLVP";      break;
  case X86II::MO_TLVP_PIC_BASE:
    O << "@TLVP" << '-' << *P.MF->getPICBaseSymbol();
    break;
  case X86II::MO_SECREL:    O << "@SECREL32";  break;
  }
}

static void printOperand(X86AsmPrinter &P, const MachineInstr *MI,
                         unsigned OpNo, raw_ostream &O,
                         const char *Modifier = 0, unsigned AsmVariant = 0);

/// printPCRelImm - This is used to print an immediate value that ends up
/// being encoded as a pc-relative value.  These print slightly differently, for
/// example, a $ is not emitted.
static void printPCRelImm(X86AsmPrinter &P, const MachineInstr *MI,
                          unsigned OpNo, raw_ostream &O) {
  const MachineOperand &MO = MI->getOperand(OpNo);
  switch (MO.getType()) {
  default: llvm_unreachable("Unknown pcrel immediate operand");
  case MachineOperand::MO_Register:
    // pc-relativeness was handled when computing the value in the reg.
    printOperand(P, MI, OpNo, O);
    return;
  case MachineOperand::MO_Immediate:
    O << MO.getImm();
    return;
  case MachineOperand::MO_GlobalAddress:
    printSymbolOperand(P, MO, O);
    return;
  }
}

static void printOperand(X86AsmPrinter &P, const MachineInstr *MI,
                         unsigned OpNo, raw_ostream &O, const char *Modifier,
                         unsigned AsmVariant) {
  const MachineOperand &MO = MI->getOperand(OpNo);
  switch (MO.getType()) {
  default: llvm_unreachable("unknown operand type!");
  case MachineOperand::MO_Register: {
    // FIXME: Enumerating AsmVariant, so we can remove magic number.
    if (AsmVariant == 0) O << '%';
    unsigned Reg = MO.getReg();
    if (Modifier && strncmp(Modifier, "subreg", strlen("subreg")) == 0) {
      MVT::SimpleValueType VT = (strcmp(Modifier+6,"64") == 0) ?
        MVT::i64 : ((strcmp(Modifier+6, "32") == 0) ? MVT::i32 :
                    ((strcmp(Modifier+6,"16") == 0) ? MVT::i16 : MVT::i8));
      Reg = getX86SubSuperRegister(Reg, VT);
    }
    O << X86ATTInstPrinter::getRegisterName(Reg);
    return;
  }

  case MachineOperand::MO_Immediate:
    if (AsmVariant == 0) O << '$';
    O << MO.getImm();
    return;

  case MachineOperand::MO_GlobalAddress: {
    if (AsmVariant == 0) O << '$';
    printSymbolOperand(P, MO, O);
    break;
  }
  }
}

static void printLeaMemReference(X86AsmPrinter &P, const MachineInstr *MI,
                                 unsigned Op, raw_ostream &O,
                                 const char *Modifier = NULL) {
  const MachineOperand &BaseReg  = MI->getOperand(Op);
  const MachineOperand &IndexReg = MI->getOperand(Op+2);
  const MachineOperand &DispSpec = MI->getOperand(Op+3);

  // If we really don't want to print out (rip), don't.
  bool HasBaseReg = BaseReg.getReg() != 0;
  if (HasBaseReg && Modifier && !strcmp(Modifier, "no-rip") &&
      BaseReg.getReg() == X86::RIP)
    HasBaseReg = false;

  // HasParenPart - True if we will print out the () part of the mem ref.
  bool HasParenPart = IndexReg.getReg() || HasBaseReg;

  switch (DispSpec.getType()) {
  default:
    llvm_unreachable("unknown operand type!");
  case MachineOperand::MO_Immediate: {
    int DispVal = DispSpec.getImm();
    if (DispVal || !HasParenPart)
      O << DispVal;
    break;
  }
  case MachineOperand::MO_GlobalAddress:
  case MachineOperand::MO_ConstantPoolIndex:
    printSymbolOperand(P, MI->getOperand(Op + 3), O);
  }

  if (Modifier && strcmp(Modifier, "H") == 0)
    O << "+8";

  if (HasParenPart) {
    assert(IndexReg.getReg() != X86::ESP &&
           "X86 doesn't allow scaling by ESP");

    O << '(';
    if (HasBaseReg)
      printOperand(P, MI, Op, O, Modifier);

    if (IndexReg.getReg()) {
      O << ',';
      printOperand(P, MI, Op+2, O, Modifier);
      unsigned ScaleVal = MI->getOperand(Op+1).getImm();
      if (ScaleVal != 1)
        O << ',' << ScaleVal;
    }
    O << ')';
  }
}

static void printMemReference(X86AsmPrinter &P, const MachineInstr *MI,
                              unsigned Op, raw_ostream &O,
                              const char *Modifier = NULL) {
  assert(isMem(MI, Op) && "Invalid memory reference!");
  const MachineOperand &Segment = MI->getOperand(Op+4);
  if (Segment.getReg()) {
    printOperand(P, MI, Op+4, O, Modifier);
    O << ':';
  }
  printLeaMemReference(P, MI, Op, O, Modifier);
}

static void printIntelMemReference(X86AsmPrinter &P, const MachineInstr *MI,
                                   unsigned Op, raw_ostream &O,
                                   const char *Modifier = NULL,
                                   unsigned AsmVariant = 1) {
  const MachineOperand &BaseReg  = MI->getOperand(Op);
  unsigned ScaleVal = MI->getOperand(Op+1).getImm();
  const MachineOperand &IndexReg = MI->getOperand(Op+2);
  const MachineOperand &DispSpec = MI->getOperand(Op+3);
  const MachineOperand &SegReg   = MI->getOperand(Op+4);

  // If this has a segment register, print it.
  if (SegReg.getReg()) {
    printOperand(P, MI, Op+4, O, Modifier, AsmVariant);
    O << ':';
  }

  O << '[';

  bool NeedPlus = false;
  if (BaseReg.getReg()) {
    printOperand(P, MI, Op, O, Modifier, AsmVariant);
    NeedPlus = true;
  }

  if (IndexReg.getReg()) {
    if (NeedPlus) O << " + ";
    if (ScaleVal != 1)
      O << ScaleVal << '*';
    printOperand(P, MI, Op+2, O, Modifier, AsmVariant);
    NeedPlus = true;
  }

  if (!DispSpec.isImm()) {
    if (NeedPlus) O << " + ";
    printOperand(P, MI, Op+3, O, Modifier, AsmVariant);
  } else {
    int64_t DispVal = DispSpec.getImm();
    if (DispVal || (!IndexReg.getReg() && !BaseReg.getReg())) {
      if (NeedPlus) {
        if (DispVal > 0)
          O << " + ";
        else {
          O << " - ";
          DispVal = -DispVal;
        }
      }
      O << DispVal;
    }
  }
  O << ']';
}

static bool printAsmMRegister(X86AsmPrinter &P, const MachineOperand &MO,
                              char Mode, raw_ostream &O) {
  unsigned Reg = MO.getReg();
  switch (Mode) {
  default: return true;  // Unknown mode.
  case 'b': // Print QImode register
    Reg = getX86SubSuperRegister(Reg, MVT::i8);
    break;
  case 'h': // Print QImode high register
    Reg = getX86SubSuperRegister(Reg, MVT::i8, true);
    break;
  case 'w': // Print HImode register
    Reg = getX86SubSuperRegister(Reg, MVT::i16);
    break;
  case 'k': // Print SImode register
    Reg = getX86SubSuperRegister(Reg, MVT::i32);
    break;
  case 'q': // Print DImode register
    // FIXME: gcc will actually print e instead of r for 32-bit.
    Reg = getX86SubSuperRegister(Reg, MVT::i64);
    break;
  }

  O << '%' << X86ATTInstPrinter::getRegisterName(Reg);
  return false;
}

/// PrintAsmOperand - Print out an operand for an inline asm expression.
///
bool X86AsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                    unsigned AsmVariant,
                                    const char *ExtraCode, raw_ostream &O) {
  // Does this asm operand have a single letter operand modifier?
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0) return true; // Unknown modifier.

    const MachineOperand &MO = MI->getOperand(OpNo);

    switch (ExtraCode[0]) {
    default:
      // See if this is a generic print operand
      return AsmPrinter::PrintAsmOperand(MI, OpNo, AsmVariant, ExtraCode, O);
    case 'a': // This is an address.  Currently only 'i' and 'r' are expected.
      switch (MO.getType()) {
      default:
        return true;
      case MachineOperand::MO_Immediate:
        O << MO.getImm();
        return false;
      case MachineOperand::MO_ConstantPoolIndex:
      case MachineOperand::MO_JumpTableIndex:
      case MachineOperand::MO_ExternalSymbol:
        llvm_unreachable("unexpected operand type!");
      case MachineOperand::MO_GlobalAddress:
        printSymbolOperand(*this, MO, O);
        if (Subtarget->isPICStyleRIPRel())
          O << "(%rip)";
        return false;
      case MachineOperand::MO_Register:
        O << '(';
        printOperand(*this, MI, OpNo, O);
        O << ')';
        return false;
      }

    case 'c': // Don't print "$" before a global var name or constant.
      switch (MO.getType()) {
      default:
        printOperand(*this, MI, OpNo, O);
        break;
      case MachineOperand::MO_Immediate:
        O << MO.getImm();
        break;
      case MachineOperand::MO_ConstantPoolIndex:
      case MachineOperand::MO_JumpTableIndex:
      case MachineOperand::MO_ExternalSymbol:
        llvm_unreachable("unexpected operand type!");
      case MachineOperand::MO_GlobalAddress:
        printSymbolOperand(*this, MO, O);
        break;
      }
      return false;

    case 'A': // Print '*' before a register (it must be a register)
      if (MO.isReg()) {
        O << '*';
        printOperand(*this, MI, OpNo, O);
        return false;
      }
      return true;

    case 'b': // Print QImode register
    case 'h': // Print QImode high register
    case 'w': // Print HImode register
    case 'k': // Print SImode register
    case 'q': // Print DImode register
      if (MO.isReg())
        return printAsmMRegister(*this, MO, ExtraCode[0], O);
      printOperand(*this, MI, OpNo, O);
      return false;

    case 'P': // This is the operand of a call, treat specially.
      printPCRelImm(*this, MI, OpNo, O);
      return false;

    case 'n':  // Negate the immediate or print a '-' before the operand.
      // Note: this is a temporary solution. It should be handled target
      // independently as part of the 'MC' work.
      if (MO.isImm()) {
        O << -MO.getImm();
        return false;
      }
      O << '-';
    }
  }

  printOperand(*this, MI, OpNo, O, /*Modifier*/ 0, AsmVariant);
  return false;
}

bool X86AsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                          unsigned OpNo, unsigned AsmVariant,
                                          const char *ExtraCode,
                                          raw_ostream &O) {
  if (AsmVariant) {
    printIntelMemReference(*this, MI, OpNo, O);
    return false;
  }

  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0) return true; // Unknown modifier.

    switch (ExtraCode[0]) {
    default: return true;  // Unknown modifier.
    case 'b': // Print QImode register
    case 'h': // Print QImode high register
    case 'w': // Print HImode register
    case 'k': // Print SImode register
    case 'q': // Print SImode register
      // These only apply to registers, ignore on mem.
      break;
    case 'H':
      printMemReference(*this, MI, OpNo, O, "H");
      return false;
    case 'P': // Don't print @PLT, but do print as memory.
      printMemReference(*this, MI, OpNo, O, "no-rip");
      return false;
    }
  }
  printMemReference(*this, MI, OpNo, O);
  return false;
}

void X86AsmPrinter::EmitStartOfAsmFile(Module &M) {
  if (Subtarget->isTargetMacho())
    OutStreamer.SwitchSection(getObjFileLowering().getTextSection());

  if (Subtarget->isTargetCOFF()) {
    // Emit an absolute @feat.00 symbol.  This appears to be some kind of
    // compiler features bitfield read by link.exe.
    if (!Subtarget->is64Bit()) {
      MCSymbol *S = MMI->getContext().GetOrCreateSymbol(StringRef("@feat.00"));
      OutStreamer.BeginCOFFSymbolDef(S);
      OutStreamer.EmitCOFFSymbolStorageClass(COFF::IMAGE_SYM_CLASS_STATIC);
      OutStreamer.EmitCOFFSymbolType(COFF::IMAGE_SYM_DTYPE_NULL);
      OutStreamer.EndCOFFSymbolDef();
      // According to the PE-COFF spec, the LSB of this value marks the object
      // for "registered SEH".  This means that all SEH handler entry points
      // must be registered in .sxdata.  Use of any unregistered handlers will
      // cause the process to terminate immediately.  LLVM does not know how to
      // register any SEH handlers, so its object files should be safe.
      S->setAbsolute();
      OutStreamer.EmitSymbolAttribute(S, MCSA_Global);
      OutStreamer.EmitAssignment(
          S, MCConstantExpr::Create(int64_t(1), MMI->getContext()));
    }
  }
}


void X86AsmPrinter::EmitEndOfAsmFile(Module &M) {
  if (Subtarget->isTargetMacho()) {
    // All darwin targets use mach-o.
    MachineModuleInfoMachO &MMIMacho =
      MMI->getObjFileInfo<MachineModuleInfoMachO>();

    // Output stubs for dynamically-linked functions.
    MachineModuleInfoMachO::SymbolListTy Stubs;

    Stubs = MMIMacho.GetFnStubList();
    if (!Stubs.empty()) {
      const MCSection *TheSection =
        OutContext.getMachOSection("__IMPORT", "__jump_table",
                                   MCSectionMachO::S_SYMBOL_STUBS |
                                   MCSectionMachO::S_ATTR_SELF_MODIFYING_CODE |
                                   MCSectionMachO::S_ATTR_PURE_INSTRUCTIONS,
                                   5, SectionKind::getMetadata());
      OutStreamer.SwitchSection(TheSection);

      for (unsigned i = 0, e = Stubs.size(); i != e; ++i) {
        // L_foo$stub:
        OutStreamer.EmitLabel(Stubs[i].first);
        //   .indirect_symbol _foo
        OutStreamer.EmitSymbolAttribute(Stubs[i].second.getPointer(),
                                        MCSA_IndirectSymbol);
        // hlt; hlt; hlt; hlt; hlt     hlt = 0xf4.
        const char HltInsts[] = "\xf4\xf4\xf4\xf4\xf4";
        OutStreamer.EmitBytes(StringRef(HltInsts, 5));
      }

      Stubs.clear();
      OutStreamer.AddBlankLine();
    }

    // Output stubs for external and common global variables.
    Stubs = MMIMacho.GetGVStubList();
    if (!Stubs.empty()) {
      const MCSection *TheSection =
        OutContext.getMachOSection("__IMPORT", "__pointers",
                                   MCSectionMachO::S_NON_LAZY_SYMBOL_POINTERS,
                                   SectionKind::getMetadata());
      OutStreamer.SwitchSection(TheSection);

      for (unsigned i = 0, e = Stubs.size(); i != e; ++i) {
        // L_foo$non_lazy_ptr:
        OutStreamer.EmitLabel(Stubs[i].first);
        // .indirect_symbol _foo
        MachineModuleInfoImpl::StubValueTy &MCSym = Stubs[i].second;
        OutStreamer.EmitSymbolAttribute(MCSym.getPointer(),
                                        MCSA_IndirectSymbol);
        // .long 0
        if (MCSym.getInt())
          // External to current translation unit.
          OutStreamer.EmitIntValue(0, 4/*size*/);
        else
          // Internal to current translation unit.
          //
          // When we place the LSDA into the TEXT section, the type info
          // pointers need to be indirect and pc-rel. We accomplish this by
          // using NLPs.  However, sometimes the types are local to the file. So
          // we need to fill in the value for the NLP in those cases.
          OutStreamer.EmitValue(MCSymbolRefExpr::Create(MCSym.getPointer(),
                                                        OutContext), 4/*size*/);
      }
      Stubs.clear();
      OutStreamer.AddBlankLine();
    }

    Stubs = MMIMacho.GetHiddenGVStubList();
    if (!Stubs.empty()) {
      OutStreamer.SwitchSection(getObjFileLowering().getDataSection());
      EmitAlignment(2);

      for (unsigned i = 0, e = Stubs.size(); i != e; ++i) {
        // L_foo$non_lazy_ptr:
        OutStreamer.EmitLabel(Stubs[i].first);
        // .long _foo
        OutStreamer.EmitValue(MCSymbolRefExpr::
                              Create(Stubs[i].second.getPointer(),
                                     OutContext), 4/*size*/);
      }
      Stubs.clear();
      OutStreamer.AddBlankLine();
    }

    SM.serializeToStackMapSection();

    // Funny Darwin hack: This flag tells the linker that no global symbols
    // contain code that falls through to other global symbols (e.g. the obvious
    // implementation of multiple entry points).  If this doesn't occur, the
    // linker can safely perform dead code stripping.  Since LLVM never
    // generates code that does this, it is always safe to set.
    OutStreamer.EmitAssemblerFlag(MCAF_SubsectionsViaSymbols);
  }

  if (Subtarget->isTargetWindows() && !Subtarget->isTargetCygMing() &&
      MMI->usesVAFloatArgument()) {
    StringRef SymbolName = Subtarget->is64Bit() ? "_fltused" : "__fltused";
    MCSymbol *S = MMI->getContext().GetOrCreateSymbol(SymbolName);
    OutStreamer.EmitSymbolAttribute(S, MCSA_Global);
  }

  if (Subtarget->isTargetCOFF()) {
    X86COFFMachineModuleInfo &COFFMMI =
      MMI->getObjFileInfo<X86COFFMachineModuleInfo>();

    // Emit type information for external functions
    typedef X86COFFMachineModuleInfo::externals_iterator externals_iterator;
    for (externals_iterator I = COFFMMI.externals_begin(),
                            E = COFFMMI.externals_end();
                            I != E; ++I) {
      OutStreamer.BeginCOFFSymbolDef(CurrentFnSym);
      OutStreamer.EmitCOFFSymbolStorageClass(COFF::IMAGE_SYM_CLASS_EXTERNAL);
      OutStreamer.EmitCOFFSymbolType(COFF::IMAGE_SYM_DTYPE_FUNCTION
                                               << COFF::SCT_COMPLEX_TYPE_SHIFT);
      OutStreamer.EndCOFFSymbolDef();
    }

    // Necessary for dllexport support
    std::vector<const MCSymbol*> DLLExportedFns, DLLExportedGlobals;

    for (Module::const_iterator I = M.begin(), E = M.end(); I != E; ++I)
      if (I->hasDLLExportStorageClass())
        DLLExportedFns.push_back(getSymbol(I));

    for (Module::const_global_iterator I = M.global_begin(),
           E = M.global_end(); I != E; ++I)
      if (I->hasDLLExportStorageClass())
        DLLExportedGlobals.push_back(getSymbol(I));

    for (Module::const_alias_iterator I = M.alias_begin(), E = M.alias_end();
                                      I != E; ++I) {
      const GlobalValue *GV = I;
      if (!GV->hasDLLExportStorageClass())
        continue;

      while (const GlobalAlias *A = dyn_cast<GlobalAlias>(GV))
        GV = A->getAliasedGlobal();

      if (isa<Function>(GV))
        DLLExportedFns.push_back(getSymbol(I));
      else if (isa<GlobalVariable>(GV))
        DLLExportedGlobals.push_back(getSymbol(I));
    }

    // Output linker support code for dllexported globals on windows.
    if (!DLLExportedGlobals.empty() || !DLLExportedFns.empty()) {
      const TargetLoweringObjectFileCOFF &TLOFCOFF =
        static_cast<const TargetLoweringObjectFileCOFF&>(getObjFileLowering());

      OutStreamer.SwitchSection(TLOFCOFF.getDrectveSection());
      SmallString<128> name;
      for (unsigned i = 0, e = DLLExportedGlobals.size(); i != e; ++i) {
        if (Subtarget->isTargetWindows())
          name = " /EXPORT:";
        else
          name = " -export:";
        name += DLLExportedGlobals[i]->getName();
        if (Subtarget->isTargetWindows())
          name += ",DATA";
        else
        name += ",data";
        OutStreamer.EmitBytes(name);
      }

      for (unsigned i = 0, e = DLLExportedFns.size(); i != e; ++i) {
        if (Subtarget->isTargetWindows())
          name = " /EXPORT:";
        else
          name = " -export:";
        name += DLLExportedFns[i]->getName();
        OutStreamer.EmitBytes(name);
      }
    }
  }

  if (Subtarget->isTargetELF()) {
    const TargetLoweringObjectFileELF &TLOFELF =
      static_cast<const TargetLoweringObjectFileELF &>(getObjFileLowering());

    MachineModuleInfoELF &MMIELF = MMI->getObjFileInfo<MachineModuleInfoELF>();

    // Output stubs for external and common global variables.
    MachineModuleInfoELF::SymbolListTy Stubs = MMIELF.GetGVStubList();
    if (!Stubs.empty()) {
      OutStreamer.SwitchSection(TLOFELF.getDataRelSection());
      const DataLayout *TD = TM.getDataLayout();

      for (unsigned i = 0, e = Stubs.size(); i != e; ++i) {
        OutStreamer.EmitLabel(Stubs[i].first);
        OutStreamer.EmitSymbolValue(Stubs[i].second.getPointer(),
                                    TD->getPointerSize());
      }
      Stubs.clear();
    }
  }
}

//===----------------------------------------------------------------------===//
// Target Registry Stuff
//===----------------------------------------------------------------------===//

// Force static initialization.
extern "C" void LLVMInitializeX86AsmPrinter() {
  RegisterAsmPrinter<X86AsmPrinter> X(TheX86_32Target);
  RegisterAsmPrinter<X86AsmPrinter> Y(TheX86_64Target);
}
