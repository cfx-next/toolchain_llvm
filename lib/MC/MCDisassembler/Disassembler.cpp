//===-- lib/MC/Disassembler.cpp - Disassembler Public C Interface ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Disassembler.h"
#include "llvm-c/Disassembler.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCRelocationInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbolizer.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/MemoryObject.h"
#include "llvm/Support/TargetRegistry.h"

namespace llvm {
class Target;
} // namespace llvm
using namespace llvm;

// LLVMCreateDisasm() creates a disassembler for the TripleName.  Symbolic
// disassembly is supported by passing a block of information in the DisInfo
// parameter and specifying the TagType and callback functions as described in
// the header llvm-c/Disassembler.h .  The pointer to the block and the 
// functions can all be passed as NULL.  If successful, this returns a
// disassembler context.  If not, it returns NULL.
//
LLVMDisasmContextRef LLVMCreateDisasmCPU(const char *Triple, const char *CPU,
                                         void *DisInfo, int TagType,
                                         LLVMOpInfoCallback GetOpInfo,
                                         LLVMSymbolLookupCallback SymbolLookUp){
  // Get the target.
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget(Triple, Error);
  if (!TheTarget)
    return 0;

  const MCRegisterInfo *MRI = TheTarget->createMCRegInfo(Triple);
  if (!MRI)
    return 0;

  // Get the assembler info needed to setup the MCContext.
  const MCAsmInfo *MAI = TheTarget->createMCAsmInfo(*MRI, Triple);
  if (!MAI)
    return 0;

  const MCInstrInfo *MII = TheTarget->createMCInstrInfo();
  if (!MII)
    return 0;

  // Package up features to be passed to target/subtarget
  std::string FeaturesStr;

  const MCSubtargetInfo *STI = TheTarget->createMCSubtargetInfo(Triple, CPU,
                                                                FeaturesStr);
  if (!STI)
    return 0;

  // Set up the MCContext for creating symbols and MCExpr's.
  MCContext *Ctx = new MCContext(MAI, MRI, 0);
  if (!Ctx)
    return 0;

  // Set up disassembler.
  MCDisassembler *DisAsm = TheTarget->createMCDisassembler(*STI);
  if (!DisAsm)
    return 0;

  std::unique_ptr<MCRelocationInfo> RelInfo(
      TheTarget->createMCRelocationInfo(Triple, *Ctx));
  if (!RelInfo)
    return 0;

  std::unique_ptr<MCSymbolizer> Symbolizer(TheTarget->createMCSymbolizer(
      Triple, GetOpInfo, SymbolLookUp, DisInfo, Ctx, RelInfo.release()));
  DisAsm->setSymbolizer(Symbolizer);
  DisAsm->setupForSymbolicDisassembly(GetOpInfo, SymbolLookUp, DisInfo,
                                      Ctx, RelInfo);
  // Set up the instruction printer.
  int AsmPrinterVariant = MAI->getAssemblerDialect();
  MCInstPrinter *IP = TheTarget->createMCInstPrinter(AsmPrinterVariant,
                                                     *MAI, *MII, *MRI, *STI);
  if (!IP)
    return 0;

  LLVMDisasmContext *DC = new LLVMDisasmContext(Triple, DisInfo, TagType,
                                                GetOpInfo, SymbolLookUp,
                                                TheTarget, MAI, MRI,
                                                STI, MII, Ctx, DisAsm, IP);
  if (!DC)
    return 0;

  DC->setCPU(CPU);
  return DC;
}

LLVMDisasmContextRef LLVMCreateDisasm(const char *Triple, void *DisInfo,
                                      int TagType, LLVMOpInfoCallback GetOpInfo,
                                      LLVMSymbolLookupCallback SymbolLookUp) {
  return LLVMCreateDisasmCPU(Triple, "", DisInfo, TagType, GetOpInfo,
                             SymbolLookUp);
}

//
// LLVMDisasmDispose() disposes of the disassembler specified by the context.
//
void LLVMDisasmDispose(LLVMDisasmContextRef DCR){
  LLVMDisasmContext *DC = (LLVMDisasmContext *)DCR;
  delete DC;
}

namespace {
//
// The memory object created by LLVMDisasmInstruction().
//
class DisasmMemoryObject : public MemoryObject {
  uint8_t *Bytes;
  uint64_t Size;
  uint64_t BasePC;
public:
  DisasmMemoryObject(uint8_t *bytes, uint64_t size, uint64_t basePC) :
                     Bytes(bytes), Size(size), BasePC(basePC) {}
 
  uint64_t getBase() const { return BasePC; }
  uint64_t getExtent() const { return Size; }

  int readByte(uint64_t Addr, uint8_t *Byte) const {
    if (Addr - BasePC >= Size)
      return -1;
    *Byte = Bytes[Addr - BasePC];
    return 0;
  }
};
} // end anonymous namespace

/// \brief Emits the comments that are stored in \p DC comment stream.
/// Each comment in the comment stream must end with a newline.
static void emitComments(LLVMDisasmContext *DC,
                         formatted_raw_ostream &FormattedOS) {
  // Flush the stream before taking its content.
  DC->CommentStream.flush();
  StringRef Comments = DC->CommentsToEmit.str();
  // Get the default information for printing a comment.
  const MCAsmInfo *MAI = DC->getAsmInfo();
  const char *CommentBegin = MAI->getCommentString();
  unsigned CommentColumn = MAI->getCommentColumn();
  bool IsFirst = true;
  while (!Comments.empty()) {
    if (!IsFirst)
      FormattedOS << '\n';
    // Emit a line of comments.
    FormattedOS.PadToColumn(CommentColumn);
    size_t Position = Comments.find('\n');
    FormattedOS << CommentBegin << ' ' << Comments.substr(0, Position);
    // Move after the newline character.
    Comments = Comments.substr(Position+1);
    IsFirst = false;
  }
  FormattedOS.flush();

  // Tell the comment stream that the vector changed underneath it.
  DC->CommentsToEmit.clear();
  DC->CommentStream.resync();
}

/// \brief Gets latency information for \p Inst form the itinerary
/// scheduling model, based on \p DC information.
/// \return The maximum expected latency over all the operands or -1
/// if no information are available.
static int getItineraryLatency(LLVMDisasmContext *DC, const MCInst &Inst) {
  const int NoInformationAvailable = -1;

  // Check if we have a CPU to get the itinerary information.
  if (DC->getCPU().empty())
    return NoInformationAvailable;

  // Get itinerary information.
  const MCSubtargetInfo *STI = DC->getSubtargetInfo();
  InstrItineraryData IID = STI->getInstrItineraryForCPU(DC->getCPU());
  // Get the scheduling class of the requested instruction.
  const MCInstrDesc& Desc = DC->getInstrInfo()->get(Inst.getOpcode());
  unsigned SCClass = Desc.getSchedClass();

  int Latency = 0;
  for (unsigned OpIdx = 0, OpIdxEnd = Inst.getNumOperands(); OpIdx != OpIdxEnd;
       ++OpIdx)
    Latency = std::max(Latency, IID.getOperandCycle(SCClass, OpIdx));

  return Latency;
}

/// \brief Gets latency information for \p Inst, based on \p DC information.
/// \return The maximum expected latency over all the definitions or -1
/// if no information are available.
static int getLatency(LLVMDisasmContext *DC, const MCInst &Inst) {
  // Try to compute scheduling information.
  const MCSubtargetInfo *STI = DC->getSubtargetInfo();
  const MCSchedModel *SCModel = STI->getSchedModel();
  const int NoInformationAvailable = -1;

  // Check if we have a scheduling model for instructions.
  if (!SCModel || !SCModel->hasInstrSchedModel())
    // Try to fall back to the itinerary model if we do not have a
    // scheduling model.
    return getItineraryLatency(DC, Inst);

  // Get the scheduling class of the requested instruction.
  const MCInstrDesc& Desc = DC->getInstrInfo()->get(Inst.getOpcode());
  unsigned SCClass = Desc.getSchedClass();
  const MCSchedClassDesc *SCDesc = SCModel->getSchedClassDesc(SCClass);
  // Resolving the variant SchedClass requires an MI to pass to
  // SubTargetInfo::resolveSchedClass.
  if (!SCDesc || !SCDesc->isValid() || SCDesc->isVariant())
    return NoInformationAvailable;

  // Compute output latency.
  int Latency = 0;
  for (unsigned DefIdx = 0, DefEnd = SCDesc->NumWriteLatencyEntries;
       DefIdx != DefEnd; ++DefIdx) {
    // Lookup the definition's write latency in SubtargetInfo.
    const MCWriteLatencyEntry *WLEntry = STI->getWriteLatencyEntry(SCDesc,
                                                                   DefIdx);
    Latency = std::max(Latency, WLEntry->Cycles);
  }

  return Latency;
}


/// \brief Emits latency information in DC->CommentStream for \p Inst, based
/// on the information available in \p DC.
static void emitLatency(LLVMDisasmContext *DC, const MCInst &Inst) {
  int Latency = getLatency(DC, Inst);

  // Report only interesting latency.
  if (Latency < 2)
    return;

  DC->CommentStream << "Latency: " << Latency << '\n';
}

//
// LLVMDisasmInstruction() disassembles a single instruction using the
// disassembler context specified in the parameter DC.  The bytes of the
// instruction are specified in the parameter Bytes, and contains at least
// BytesSize number of bytes.  The instruction is at the address specified by
// the PC parameter.  If a valid instruction can be disassembled its string is
// returned indirectly in OutString which whos size is specified in the
// parameter OutStringSize.  This function returns the number of bytes in the
// instruction or zero if there was no valid instruction.  If this function
// returns zero the caller will have to pick how many bytes they want to step
// over by printing a .byte, .long etc. to continue.
//
size_t LLVMDisasmInstruction(LLVMDisasmContextRef DCR, uint8_t *Bytes,
                             uint64_t BytesSize, uint64_t PC, char *OutString,
                             size_t OutStringSize){
  LLVMDisasmContext *DC = (LLVMDisasmContext *)DCR;
  // Wrap the pointer to the Bytes, BytesSize and PC in a MemoryObject.
  DisasmMemoryObject MemoryObject(Bytes, BytesSize, PC);

  uint64_t Size;
  MCInst Inst;
  const MCDisassembler *DisAsm = DC->getDisAsm();
  MCInstPrinter *IP = DC->getIP();
  MCDisassembler::DecodeStatus S;
  SmallVector<char, 64> InsnStr;
  raw_svector_ostream Annotations(InsnStr);
  S = DisAsm->getInstruction(Inst, Size, MemoryObject, PC,
                             /*REMOVE*/ nulls(), Annotations);
  switch (S) {
  case MCDisassembler::Fail:
  case MCDisassembler::SoftFail:
    // FIXME: Do something different for soft failure modes?
    return 0;

  case MCDisassembler::Success: {
    Annotations.flush();
    StringRef AnnotationsStr = Annotations.str();

    SmallVector<char, 64> InsnStr;
    raw_svector_ostream OS(InsnStr);
    formatted_raw_ostream FormattedOS(OS);
    IP->printInst(&Inst, FormattedOS, AnnotationsStr);

    if (DC->getOptions() & LLVMDisassembler_Option_PrintLatency)
      emitLatency(DC, Inst);

    emitComments(DC, FormattedOS);
    OS.flush();

    assert(OutStringSize != 0 && "Output buffer cannot be zero size");
    size_t OutputSize = std::min(OutStringSize-1, InsnStr.size());
    std::memcpy(OutString, InsnStr.data(), OutputSize);
    OutString[OutputSize] = '\0'; // Terminate string.

    return Size;
  }
  }
  llvm_unreachable("Invalid DecodeStatus!");
}

//
// LLVMSetDisasmOptions() sets the disassembler's options.  It returns 1 if it
// can set all the Options and 0 otherwise.
//
int LLVMSetDisasmOptions(LLVMDisasmContextRef DCR, uint64_t Options){
  if (Options & LLVMDisassembler_Option_UseMarkup){
      LLVMDisasmContext *DC = (LLVMDisasmContext *)DCR;
      MCInstPrinter *IP = DC->getIP();
      IP->setUseMarkup(1);
      DC->addOptions(LLVMDisassembler_Option_UseMarkup);
      Options &= ~LLVMDisassembler_Option_UseMarkup;
  }
  if (Options & LLVMDisassembler_Option_PrintImmHex){
      LLVMDisasmContext *DC = (LLVMDisasmContext *)DCR;
      MCInstPrinter *IP = DC->getIP();
      IP->setPrintImmHex(1);
      DC->addOptions(LLVMDisassembler_Option_PrintImmHex);
      Options &= ~LLVMDisassembler_Option_PrintImmHex;
  }
  if (Options & LLVMDisassembler_Option_AsmPrinterVariant){
      LLVMDisasmContext *DC = (LLVMDisasmContext *)DCR;
      // Try to set up the new instruction printer.
      const MCAsmInfo *MAI = DC->getAsmInfo();
      const MCInstrInfo *MII = DC->getInstrInfo();
      const MCRegisterInfo *MRI = DC->getRegisterInfo();
      const MCSubtargetInfo *STI = DC->getSubtargetInfo();
      int AsmPrinterVariant = MAI->getAssemblerDialect();
      AsmPrinterVariant = AsmPrinterVariant == 0 ? 1 : 0;
      MCInstPrinter *IP = DC->getTarget()->createMCInstPrinter(
          AsmPrinterVariant, *MAI, *MII, *MRI, *STI);
      if (IP) {
        DC->setIP(IP);
        DC->addOptions(LLVMDisassembler_Option_AsmPrinterVariant);
        Options &= ~LLVMDisassembler_Option_AsmPrinterVariant;
      }
  }
  if (Options & LLVMDisassembler_Option_SetInstrComments) {
    LLVMDisasmContext *DC = (LLVMDisasmContext *)DCR;
    MCInstPrinter *IP = DC->getIP();
    IP->setCommentStream(DC->CommentStream);
    DC->addOptions(LLVMDisassembler_Option_SetInstrComments);
    Options &= ~LLVMDisassembler_Option_SetInstrComments;
  }
  if (Options & LLVMDisassembler_Option_PrintLatency) {
    LLVMDisasmContext *DC = (LLVMDisasmContext *)DCR;
    DC->addOptions(LLVMDisassembler_Option_PrintLatency);
    Options &= ~LLVMDisassembler_Option_PrintLatency;
  }
  return (Options == 0);
}
