//===-- ELFDumper.cpp - ELF-specific dumper ---------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief This file implements the ELF-specific dumper for llvm-readobj.
///
//===----------------------------------------------------------------------===//

#include "llvm-readobj.h"
#include "ARMAttributeParser.h"
#include "ARMEHABIPrinter.h"
#include "Error.h"
#include "ObjDumper.h"
#include "StreamWriter.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/ARMBuildAttributes.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::object;
using namespace ELF;

#define LLVM_READOBJ_ENUM_CASE(ns, enum) \
  case ns::enum: return #enum;

namespace {

template<typename ELFT>
class ELFDumper : public ObjDumper {
public:
  ELFDumper(const ELFFile<ELFT> *Obj, StreamWriter &Writer)
      : ObjDumper(Writer), Obj(Obj) {}

  virtual void printFileHeaders() override;
  virtual void printSections() override;
  virtual void printRelocations() override;
  virtual void printSymbols() override;
  virtual void printDynamicSymbols() override;
  virtual void printUnwindInfo() override;

  virtual void printDynamicTable() override;
  virtual void printNeededLibraries() override;
  virtual void printProgramHeaders() override;

  virtual void printAttributes() override;

private:
  typedef ELFFile<ELFT> ELFO;
  typedef typename ELFO::Elf_Shdr Elf_Shdr;
  typedef typename ELFO::Elf_Sym Elf_Sym;

  void printSymbol(typename ELFO::Elf_Sym_Iter Symbol);

  void printRelocations(const Elf_Shdr *Sec);
  void printRelocation(const Elf_Shdr *Sec, typename ELFO::Elf_Rela Rel);

  const ELFO *Obj;
};

template <class T> T errorOrDefault(ErrorOr<T> Val, T Default = T()) {
  if (!Val) {
    error(Val.getError());
    return Default;
  }

  return *Val;
}
} // namespace

namespace llvm {

template <class ELFT>
static error_code createELFDumper(const ELFFile<ELFT> *Obj,
                                  StreamWriter &Writer,
                                  std::unique_ptr<ObjDumper> &Result) {
  Result.reset(new ELFDumper<ELFT>(Obj, Writer));
  return readobj_error::success;
}

error_code createELFDumper(const object::ObjectFile *Obj, StreamWriter &Writer,
                           std::unique_ptr<ObjDumper> &Result) {
  // Little-endian 32-bit
  if (const ELF32LEObjectFile *ELFObj = dyn_cast<ELF32LEObjectFile>(Obj))
    return createELFDumper(ELFObj->getELFFile(), Writer, Result);

  // Big-endian 32-bit
  if (const ELF32BEObjectFile *ELFObj = dyn_cast<ELF32BEObjectFile>(Obj))
    return createELFDumper(ELFObj->getELFFile(), Writer, Result);

  // Little-endian 64-bit
  if (const ELF64LEObjectFile *ELFObj = dyn_cast<ELF64LEObjectFile>(Obj))
    return createELFDumper(ELFObj->getELFFile(), Writer, Result);

  // Big-endian 64-bit
  if (const ELF64BEObjectFile *ELFObj = dyn_cast<ELF64BEObjectFile>(Obj))
    return createELFDumper(ELFObj->getELFFile(), Writer, Result);

  return readobj_error::unsupported_obj_file_format;
}

} // namespace llvm

static const EnumEntry<unsigned> ElfClass[] = {
  { "None",   ELF::ELFCLASSNONE },
  { "32-bit", ELF::ELFCLASS32   },
  { "64-bit", ELF::ELFCLASS64   },
};

static const EnumEntry<unsigned> ElfDataEncoding[] = {
  { "None",         ELF::ELFDATANONE },
  { "LittleEndian", ELF::ELFDATA2LSB },
  { "BigEndian",    ELF::ELFDATA2MSB },
};

static const EnumEntry<unsigned> ElfObjectFileType[] = {
  { "None",         ELF::ET_NONE },
  { "Relocatable",  ELF::ET_REL  },
  { "Executable",   ELF::ET_EXEC },
  { "SharedObject", ELF::ET_DYN  },
  { "Core",         ELF::ET_CORE },
};

static const EnumEntry<unsigned> ElfOSABI[] = {
  { "SystemV",      ELF::ELFOSABI_NONE         },
  { "HPUX",         ELF::ELFOSABI_HPUX         },
  { "NetBSD",       ELF::ELFOSABI_NETBSD       },
  { "GNU/Linux",    ELF::ELFOSABI_LINUX        },
  { "GNU/Hurd",     ELF::ELFOSABI_HURD         },
  { "Solaris",      ELF::ELFOSABI_SOLARIS      },
  { "AIX",          ELF::ELFOSABI_AIX          },
  { "IRIX",         ELF::ELFOSABI_IRIX         },
  { "FreeBSD",      ELF::ELFOSABI_FREEBSD      },
  { "TRU64",        ELF::ELFOSABI_TRU64        },
  { "Modesto",      ELF::ELFOSABI_MODESTO      },
  { "OpenBSD",      ELF::ELFOSABI_OPENBSD      },
  { "OpenVMS",      ELF::ELFOSABI_OPENVMS      },
  { "NSK",          ELF::ELFOSABI_NSK          },
  { "AROS",         ELF::ELFOSABI_AROS         },
  { "FenixOS",      ELF::ELFOSABI_FENIXOS      },
  { "C6000_ELFABI", ELF::ELFOSABI_C6000_ELFABI },
  { "C6000_LINUX" , ELF::ELFOSABI_C6000_LINUX  },
  { "ARM",          ELF::ELFOSABI_ARM          },
  { "Standalone"  , ELF::ELFOSABI_STANDALONE   }
};

static const EnumEntry<unsigned> ElfMachineType[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, EM_NONE         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_M32          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SPARC        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_386          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_68K          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_88K          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_486          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_860          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MIPS         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_S370         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MIPS_RS3_LE  ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_PARISC       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_VPP500       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SPARC32PLUS  ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_960          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_PPC          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_PPC64        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_S390         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SPU          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_V800         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_FR20         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_RH32         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_RCE          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ARM          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ALPHA        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SH           ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SPARCV9      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_TRICORE      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ARC          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_H8_300       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_H8_300H      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_H8S          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_H8_500       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_IA_64        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MIPS_X       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_COLDFIRE     ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_68HC12       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MMA          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_PCP          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_NCPU         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_NDR1         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_STARCORE     ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ME16         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ST100        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_TINYJ        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_X86_64       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_PDSP         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_PDP10        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_PDP11        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_FX66         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ST9PLUS      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ST7          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_68HC16       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_68HC11       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_68HC08       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_68HC05       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SVX          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ST19         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_VAX          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_CRIS         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_JAVELIN      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_FIREPATH     ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ZSP          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MMIX         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_HUANY        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_PRISM        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_AVR          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_FR30         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_D10V         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_D30V         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_V850         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_M32R         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MN10300      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MN10200      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_PJ           ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_OPENRISC     ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ARC_COMPACT  ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_XTENSA       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_VIDEOCORE    ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_TMM_GPP      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_NS32K        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_TPC          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SNP1K        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ST200        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_IP2K         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MAX          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_CR           ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_F2MC16       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MSP430       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_BLACKFIN     ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SE_C33       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SEP          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ARCA         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_UNICORE      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_EXCESS       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_DXP          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ALTERA_NIOS2 ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_CRX          ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_XGATE        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_C166         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_M16C         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_DSPIC30F     ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_CE           ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_M32C         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_TSK3000      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_RS08         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SHARC        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ECOG2        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SCORE7       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_DSP24        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_VIDEOCORE3   ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_LATTICEMICO32),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SE_C17       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_TI_C6000     ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_TI_C2000     ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_TI_C5500     ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MMDSP_PLUS   ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_CYPRESS_M8C  ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_R32C         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_TRIMEDIA     ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_HEXAGON      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_8051         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_STXP7X       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_NDS32        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ECOG1        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ECOG1X       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MAXQ30       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_XIMO16       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MANIK        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_CRAYNV2      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_RX           ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_METAG        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_MCST_ELBRUS  ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ECOG16       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_CR16         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ETPU         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_SLE9X        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_L10M         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_K10M         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_AARCH64      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_AVR32        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_STM8         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_TILE64       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_TILEPRO      ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_CUDA         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_TILEGX       ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_CLOUDSHIELD  ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_COREA_1ST    ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_COREA_2ND    ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_ARC_COMPACT2 ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_OPEN8        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_RL78         ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_VIDEOCORE5   ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_78KOR        ),
  LLVM_READOBJ_ENUM_ENT(ELF, EM_56800EX      )
};

static const EnumEntry<unsigned> ElfSymbolBindings[] = {
  { "Local",  ELF::STB_LOCAL  },
  { "Global", ELF::STB_GLOBAL },
  { "Weak",   ELF::STB_WEAK   }
};

static const EnumEntry<unsigned> ElfSymbolTypes[] = {
  { "None",      ELF::STT_NOTYPE    },
  { "Object",    ELF::STT_OBJECT    },
  { "Function",  ELF::STT_FUNC      },
  { "Section",   ELF::STT_SECTION   },
  { "File",      ELF::STT_FILE      },
  { "Common",    ELF::STT_COMMON    },
  { "TLS",       ELF::STT_TLS       },
  { "GNU_IFunc", ELF::STT_GNU_IFUNC }
};

static const char *getElfSectionType(unsigned Arch, unsigned Type) {
  switch (Arch) {
  case ELF::EM_ARM:
    switch (Type) {
    LLVM_READOBJ_ENUM_CASE(ELF, SHT_ARM_EXIDX);
    LLVM_READOBJ_ENUM_CASE(ELF, SHT_ARM_PREEMPTMAP);
    LLVM_READOBJ_ENUM_CASE(ELF, SHT_ARM_ATTRIBUTES);
    LLVM_READOBJ_ENUM_CASE(ELF, SHT_ARM_DEBUGOVERLAY);
    LLVM_READOBJ_ENUM_CASE(ELF, SHT_ARM_OVERLAYSECTION);
    }
  case ELF::EM_HEXAGON:
    switch (Type) { LLVM_READOBJ_ENUM_CASE(ELF, SHT_HEX_ORDERED); }
  case ELF::EM_X86_64:
    switch (Type) { LLVM_READOBJ_ENUM_CASE(ELF, SHT_X86_64_UNWIND); }
  case ELF::EM_MIPS:
  case ELF::EM_MIPS_RS3_LE:
    switch (Type) {
    LLVM_READOBJ_ENUM_CASE(ELF, SHT_MIPS_REGINFO);
    LLVM_READOBJ_ENUM_CASE(ELF, SHT_MIPS_OPTIONS);
    }
  }

  switch (Type) {
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_NULL              );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_PROGBITS          );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_SYMTAB            );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_STRTAB            );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_RELA              );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_HASH              );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_DYNAMIC           );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_NOTE              );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_NOBITS            );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_REL               );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_SHLIB             );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_DYNSYM            );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_INIT_ARRAY        );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_FINI_ARRAY        );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_PREINIT_ARRAY     );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_GROUP             );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_SYMTAB_SHNDX      );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_GNU_ATTRIBUTES    );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_GNU_HASH          );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_GNU_verdef        );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_GNU_verneed       );
  LLVM_READOBJ_ENUM_CASE(ELF, SHT_GNU_versym        );
  default: return "";
  }
}

static const EnumEntry<unsigned> ElfSectionFlags[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_WRITE           ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_ALLOC           ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_EXCLUDE         ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_EXECINSTR       ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_MERGE           ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_STRINGS         ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_INFO_LINK       ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_LINK_ORDER      ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_OS_NONCONFORMING),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_GROUP           ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_TLS             ),
  LLVM_READOBJ_ENUM_ENT(ELF, XCORE_SHF_CP_SECTION),
  LLVM_READOBJ_ENUM_ENT(ELF, XCORE_SHF_DP_SECTION),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_MIPS_NOSTRIP    )
};

static const char *getElfSegmentType(unsigned Arch, unsigned Type) {
  // Check potentially overlapped processor-specific
  // program header type.
  switch (Arch) {
  case ELF::EM_ARM:
    switch (Type) {
    LLVM_READOBJ_ENUM_CASE(ELF, PT_ARM_EXIDX);
    }
  case ELF::EM_MIPS:
  case ELF::EM_MIPS_RS3_LE:
    switch (Type) {
    LLVM_READOBJ_ENUM_CASE(ELF, PT_MIPS_REGINFO);
    LLVM_READOBJ_ENUM_CASE(ELF, PT_MIPS_RTPROC);
    LLVM_READOBJ_ENUM_CASE(ELF, PT_MIPS_OPTIONS);
    }
  }

  switch (Type) {
  LLVM_READOBJ_ENUM_CASE(ELF, PT_NULL   );
  LLVM_READOBJ_ENUM_CASE(ELF, PT_LOAD   );
  LLVM_READOBJ_ENUM_CASE(ELF, PT_DYNAMIC);
  LLVM_READOBJ_ENUM_CASE(ELF, PT_INTERP );
  LLVM_READOBJ_ENUM_CASE(ELF, PT_NOTE   );
  LLVM_READOBJ_ENUM_CASE(ELF, PT_SHLIB  );
  LLVM_READOBJ_ENUM_CASE(ELF, PT_PHDR   );
  LLVM_READOBJ_ENUM_CASE(ELF, PT_TLS    );

  LLVM_READOBJ_ENUM_CASE(ELF, PT_GNU_EH_FRAME);
  LLVM_READOBJ_ENUM_CASE(ELF, PT_SUNW_UNWIND);

  LLVM_READOBJ_ENUM_CASE(ELF, PT_GNU_STACK);
  LLVM_READOBJ_ENUM_CASE(ELF, PT_GNU_RELRO);
  default: return "";
  }
}

static const EnumEntry<unsigned> ElfSegmentFlags[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, PF_X),
  LLVM_READOBJ_ENUM_ENT(ELF, PF_W),
  LLVM_READOBJ_ENUM_ENT(ELF, PF_R)
};

template<class ELFT>
void ELFDumper<ELFT>::printFileHeaders() {
  const typename ELFO::Elf_Ehdr *Header = Obj->getHeader();

  {
    DictScope D(W, "ElfHeader");
    {
      DictScope D(W, "Ident");
      W.printBinary("Magic", makeArrayRef(Header->e_ident).slice(ELF::EI_MAG0,
                                                                 4));
      W.printEnum  ("Class", Header->e_ident[ELF::EI_CLASS],
                      makeArrayRef(ElfClass));
      W.printEnum  ("DataEncoding", Header->e_ident[ELF::EI_DATA],
                      makeArrayRef(ElfDataEncoding));
      W.printNumber("FileVersion", Header->e_ident[ELF::EI_VERSION]);
      W.printEnum  ("OS/ABI", Header->e_ident[ELF::EI_OSABI],
                      makeArrayRef(ElfOSABI));
      W.printNumber("ABIVersion", Header->e_ident[ELF::EI_ABIVERSION]);
      W.printBinary("Unused", makeArrayRef(Header->e_ident).slice(ELF::EI_PAD));
    }

    W.printEnum  ("Type", Header->e_type, makeArrayRef(ElfObjectFileType));
    W.printEnum  ("Machine", Header->e_machine, makeArrayRef(ElfMachineType));
    W.printNumber("Version", Header->e_version);
    W.printHex   ("Entry", Header->e_entry);
    W.printHex   ("ProgramHeaderOffset", Header->e_phoff);
    W.printHex   ("SectionHeaderOffset", Header->e_shoff);
    W.printFlags ("Flags", Header->e_flags);
    W.printNumber("HeaderSize", Header->e_ehsize);
    W.printNumber("ProgramHeaderEntrySize", Header->e_phentsize);
    W.printNumber("ProgramHeaderCount", Header->e_phnum);
    W.printNumber("SectionHeaderEntrySize", Header->e_shentsize);
    W.printNumber("SectionHeaderCount", Header->e_shnum);
    W.printNumber("StringTableSectionIndex", Header->e_shstrndx);
  }
}

template<class ELFT>
void ELFDumper<ELFT>::printSections() {
  ListScope SectionsD(W, "Sections");

  int SectionIndex = -1;
  for (typename ELFO::Elf_Shdr_Iter SecI = Obj->begin_sections(),
                                    SecE = Obj->end_sections();
       SecI != SecE; ++SecI) {
    ++SectionIndex;

    const Elf_Shdr *Section = &*SecI;
    StringRef Name = errorOrDefault(Obj->getSectionName(Section));

    DictScope SectionD(W, "Section");
    W.printNumber("Index", SectionIndex);
    W.printNumber("Name", Name, Section->sh_name);
    W.printHex("Type",
               getElfSectionType(Obj->getHeader()->e_machine, Section->sh_type),
               Section->sh_type);
    W.printFlags ("Flags", Section->sh_flags, makeArrayRef(ElfSectionFlags));
    W.printHex   ("Address", Section->sh_addr);
    W.printHex   ("Offset", Section->sh_offset);
    W.printNumber("Size", Section->sh_size);
    W.printNumber("Link", Section->sh_link);
    W.printNumber("Info", Section->sh_info);
    W.printNumber("AddressAlignment", Section->sh_addralign);
    W.printNumber("EntrySize", Section->sh_entsize);

    if (opts::SectionRelocations) {
      ListScope D(W, "Relocations");
      printRelocations(Section);
    }

    if (opts::SectionSymbols) {
      ListScope D(W, "Symbols");
      for (typename ELFO::Elf_Sym_Iter SymI = Obj->begin_symbols(),
                                       SymE = Obj->end_symbols();
           SymI != SymE; ++SymI) {
        if (Obj->getSection(&*SymI) == Section)
          printSymbol(SymI);
      }
    }

    if (opts::SectionData) {
      ArrayRef<uint8_t> Data = errorOrDefault(Obj->getSectionContents(Section));
      W.printBinaryBlock("SectionData",
                         StringRef((const char *)Data.data(), Data.size()));
    }
  }
}

template<class ELFT>
void ELFDumper<ELFT>::printRelocations() {
  ListScope D(W, "Relocations");

  int SectionNumber = -1;
  for (typename ELFO::Elf_Shdr_Iter SecI = Obj->begin_sections(),
                                    SecE = Obj->end_sections();
       SecI != SecE; ++SecI) {
    ++SectionNumber;

    if (SecI->sh_type != ELF::SHT_REL && SecI->sh_type != ELF::SHT_RELA)
      continue;

    StringRef Name = errorOrDefault(Obj->getSectionName(&*SecI));

    W.startLine() << "Section (" << SectionNumber << ") " << Name << " {\n";
    W.indent();

    printRelocations(&*SecI);

    W.unindent();
    W.startLine() << "}\n";
  }
}

template <class ELFT>
void ELFDumper<ELFT>::printRelocations(const Elf_Shdr *Sec) {
  switch (Sec->sh_type) {
  case ELF::SHT_REL:
    for (typename ELFO::Elf_Rel_Iter RI = Obj->begin_rel(Sec),
                                     RE = Obj->end_rel(Sec);
         RI != RE; ++RI) {
      typename ELFO::Elf_Rela Rela;
      Rela.r_offset = RI->r_offset;
      Rela.r_info = RI->r_info;
      Rela.r_addend = 0;
      printRelocation(Sec, Rela);
    }
    break;
  case ELF::SHT_RELA:
    for (typename ELFO::Elf_Rela_Iter RI = Obj->begin_rela(Sec),
                                      RE = Obj->end_rela(Sec);
         RI != RE; ++RI) {
      printRelocation(Sec, *RI);
    }
    break;
  }
}

template <class ELFT>
void ELFDumper<ELFT>::printRelocation(const Elf_Shdr *Sec,
                                      typename ELFO::Elf_Rela Rel) {
  SmallString<32> RelocName;
  Obj->getRelocationTypeName(Rel.getType(Obj->isMips64EL()), RelocName);
  StringRef SymbolName;
  std::pair<const Elf_Shdr *, const Elf_Sym *> Sym =
      Obj->getRelocationSymbol(Sec, &Rel);
  if (Sym.first)
    SymbolName = errorOrDefault(Obj->getSymbolName(Sym.first, Sym.second));

  if (opts::ExpandRelocs) {
    DictScope Group(W, "Relocation");
    W.printHex("Offset", Rel.r_offset);
    W.printNumber("Type", RelocName, (int)Rel.getType(Obj->isMips64EL()));
    W.printString("Symbol", SymbolName.size() > 0 ? SymbolName : "-");
    W.printHex("Addend", Rel.r_addend);
  } else {
    raw_ostream& OS = W.startLine();
    OS << W.hex(Rel.r_offset)
       << " " << RelocName
       << " " << (SymbolName.size() > 0 ? SymbolName : "-")
       << " " << W.hex(Rel.r_addend)
       << "\n";
  }
}

template<class ELFT>
void ELFDumper<ELFT>::printSymbols() {
  ListScope Group(W, "Symbols");
  for (typename ELFO::Elf_Sym_Iter SymI = Obj->begin_symbols(),
                                   SymE = Obj->end_symbols();
       SymI != SymE; ++SymI) {
    printSymbol(SymI);
  }
}

template<class ELFT>
void ELFDumper<ELFT>::printDynamicSymbols() {
  ListScope Group(W, "DynamicSymbols");

  for (typename ELFO::Elf_Sym_Iter SymI = Obj->begin_dynamic_symbols(),
                                   SymE = Obj->end_dynamic_symbols();
       SymI != SymE; ++SymI) {
    printSymbol(SymI);
  }
}

template <class ELFT>
void ELFDumper<ELFT>::printSymbol(typename ELFO::Elf_Sym_Iter Symbol) {
  StringRef SymbolName = errorOrDefault(Obj->getSymbolName(Symbol));
  const Elf_Shdr *Sec = Obj->getSection(&*Symbol);
  StringRef SectionName = Sec ? errorOrDefault(Obj->getSectionName(Sec)) : "";
  std::string FullSymbolName(SymbolName);
  if (Symbol.isDynamic()) {
    bool IsDefault;
    ErrorOr<StringRef> Version = Obj->getSymbolVersion(0, &*Symbol, IsDefault);
    if (Version) {
      FullSymbolName += (IsDefault ? "@@" : "@");
      FullSymbolName += *Version;
    } else
      error(Version.getError());
  }

  DictScope D(W, "Symbol");
  W.printNumber("Name", FullSymbolName, Symbol->st_name);
  W.printHex   ("Value", Symbol->st_value);
  W.printNumber("Size", Symbol->st_size);
  W.printEnum  ("Binding", Symbol->getBinding(),
                  makeArrayRef(ElfSymbolBindings));
  W.printEnum  ("Type", Symbol->getType(), makeArrayRef(ElfSymbolTypes));
  W.printNumber("Other", Symbol->st_other);
  W.printHex   ("Section", SectionName, Symbol->st_shndx);
}

#define LLVM_READOBJ_TYPE_CASE(name) \
  case DT_##name: return #name

static const char *getTypeString(uint64_t Type) {
  switch (Type) {
  LLVM_READOBJ_TYPE_CASE(BIND_NOW);
  LLVM_READOBJ_TYPE_CASE(DEBUG);
  LLVM_READOBJ_TYPE_CASE(FINI);
  LLVM_READOBJ_TYPE_CASE(FINI_ARRAY);
  LLVM_READOBJ_TYPE_CASE(FINI_ARRAYSZ);
  LLVM_READOBJ_TYPE_CASE(FLAGS);
  LLVM_READOBJ_TYPE_CASE(HASH);
  LLVM_READOBJ_TYPE_CASE(INIT);
  LLVM_READOBJ_TYPE_CASE(INIT_ARRAY);
  LLVM_READOBJ_TYPE_CASE(INIT_ARRAYSZ);
  LLVM_READOBJ_TYPE_CASE(PREINIT_ARRAY);
  LLVM_READOBJ_TYPE_CASE(PREINIT_ARRAYSZ);
  LLVM_READOBJ_TYPE_CASE(JMPREL);
  LLVM_READOBJ_TYPE_CASE(NEEDED);
  LLVM_READOBJ_TYPE_CASE(NULL);
  LLVM_READOBJ_TYPE_CASE(PLTGOT);
  LLVM_READOBJ_TYPE_CASE(PLTREL);
  LLVM_READOBJ_TYPE_CASE(PLTRELSZ);
  LLVM_READOBJ_TYPE_CASE(REL);
  LLVM_READOBJ_TYPE_CASE(RELA);
  LLVM_READOBJ_TYPE_CASE(RELENT);
  LLVM_READOBJ_TYPE_CASE(RELSZ);
  LLVM_READOBJ_TYPE_CASE(RELAENT);
  LLVM_READOBJ_TYPE_CASE(RELASZ);
  LLVM_READOBJ_TYPE_CASE(RPATH);
  LLVM_READOBJ_TYPE_CASE(RUNPATH);
  LLVM_READOBJ_TYPE_CASE(SONAME);
  LLVM_READOBJ_TYPE_CASE(STRSZ);
  LLVM_READOBJ_TYPE_CASE(STRTAB);
  LLVM_READOBJ_TYPE_CASE(SYMBOLIC);
  LLVM_READOBJ_TYPE_CASE(SYMENT);
  LLVM_READOBJ_TYPE_CASE(SYMTAB);
  LLVM_READOBJ_TYPE_CASE(TEXTREL);
  LLVM_READOBJ_TYPE_CASE(VERNEED);
  LLVM_READOBJ_TYPE_CASE(VERNEEDNUM);
  LLVM_READOBJ_TYPE_CASE(VERSYM);
  LLVM_READOBJ_TYPE_CASE(MIPS_RLD_VERSION);
  LLVM_READOBJ_TYPE_CASE(MIPS_FLAGS);
  LLVM_READOBJ_TYPE_CASE(MIPS_BASE_ADDRESS);
  LLVM_READOBJ_TYPE_CASE(MIPS_LOCAL_GOTNO);
  LLVM_READOBJ_TYPE_CASE(MIPS_SYMTABNO);
  LLVM_READOBJ_TYPE_CASE(MIPS_UNREFEXTNO);
  LLVM_READOBJ_TYPE_CASE(MIPS_GOTSYM);
  LLVM_READOBJ_TYPE_CASE(MIPS_RLD_MAP);
  LLVM_READOBJ_TYPE_CASE(MIPS_PLTGOT);
  default: return "unknown";
  }
}

#undef LLVM_READOBJ_TYPE_CASE

template <class ELFT>
static void printValue(const ELFFile<ELFT> *O, uint64_t Type, uint64_t Value,
                       bool Is64, raw_ostream &OS) {
  switch (Type) {
  case DT_PLTREL:
    if (Value == DT_REL) {
      OS << "REL";
      break;
    } else if (Value == DT_RELA) {
      OS << "RELA";
      break;
    }
  // Fallthrough.
  case DT_PLTGOT:
  case DT_HASH:
  case DT_STRTAB:
  case DT_SYMTAB:
  case DT_RELA:
  case DT_INIT:
  case DT_FINI:
  case DT_REL:
  case DT_JMPREL:
  case DT_INIT_ARRAY:
  case DT_FINI_ARRAY:
  case DT_PREINIT_ARRAY:
  case DT_DEBUG:
  case DT_VERNEED:
  case DT_VERSYM:
  case DT_NULL:
  case DT_MIPS_FLAGS:
  case DT_MIPS_BASE_ADDRESS:
  case DT_MIPS_GOTSYM:
  case DT_MIPS_RLD_MAP:
  case DT_MIPS_PLTGOT:
    OS << format("0x%" PRIX64, Value);
    break;
  case DT_VERNEEDNUM:
  case DT_MIPS_RLD_VERSION:
  case DT_MIPS_LOCAL_GOTNO:
  case DT_MIPS_SYMTABNO:
  case DT_MIPS_UNREFEXTNO:
    OS << Value;
    break;
  case DT_PLTRELSZ:
  case DT_RELASZ:
  case DT_RELAENT:
  case DT_STRSZ:
  case DT_SYMENT:
  case DT_RELSZ:
  case DT_RELENT:
  case DT_INIT_ARRAYSZ:
  case DT_FINI_ARRAYSZ:
  case DT_PREINIT_ARRAYSZ:
    OS << Value << " (bytes)";
    break;
  case DT_NEEDED:
    OS << "SharedLibrary (" << O->getDynamicString(Value) << ")";
    break;
  case DT_SONAME:
    OS << "LibrarySoname (" << O->getDynamicString(Value) << ")";
    break;
  case DT_RPATH:
  case DT_RUNPATH:
    OS << O->getDynamicString(Value);
    break;
  }
}

template<class ELFT>
void ELFDumper<ELFT>::printUnwindInfo() {
  W.startLine() << "UnwindInfo not implemented.\n";
}

namespace {
template <>
void ELFDumper<ELFType<support::little, 2, false> >::printUnwindInfo() {
  const unsigned Machine = Obj->getHeader()->e_machine;
  if (Machine == EM_ARM) {
    ARM::EHABI::PrinterContext<ELFType<support::little, 2, false> > Ctx(W, Obj);
    return Ctx.PrintUnwindInformation();
  }
  W.startLine() << "UnwindInfo not implemented.\n";
}
}

template<class ELFT>
void ELFDumper<ELFT>::printDynamicTable() {
  typedef typename ELFO::Elf_Dyn_Iter EDI;
  EDI Start = Obj->begin_dynamic_table(), End = Obj->end_dynamic_table(true);

  if (Start == End)
    return;

  ptrdiff_t Total = std::distance(Start, End);
  raw_ostream &OS = W.getOStream();
  W.startLine() << "DynamicSection [ (" << Total << " entries)\n";

  bool Is64 = ELFT::Is64Bits;

  W.startLine()
     << "  Tag" << (Is64 ? "                " : "        ") << "Type"
     << "                 " << "Name/Value\n";
  for (; Start != End; ++Start) {
    W.startLine()
       << "  "
       << format(Is64 ? "0x%016" PRIX64 : "0x%08" PRIX64, Start->getTag())
       << " " << format("%-21s", getTypeString(Start->getTag()));
    printValue(Obj, Start->getTag(), Start->getVal(), Is64, OS);
    OS << "\n";
  }

  W.startLine() << "]\n";
}

template<class ELFT>
void ELFDumper<ELFT>::printNeededLibraries() {
  ListScope D(W, "NeededLibraries");

  typedef std::vector<StringRef> LibsTy;
  LibsTy Libs;

  for (typename ELFO::Elf_Dyn_Iter DynI = Obj->begin_dynamic_table(),
                                   DynE = Obj->end_dynamic_table();
       DynI != DynE; ++DynI)
    if (DynI->d_tag == ELF::DT_NEEDED)
      Libs.push_back(Obj->getDynamicString(DynI->d_un.d_val));

  std::stable_sort(Libs.begin(), Libs.end());

  for (LibsTy::const_iterator I = Libs.begin(), E = Libs.end(); I != E; ++I) {
    outs() << "  " << *I << "\n";
  }
}

template<class ELFT>
void ELFDumper<ELFT>::printProgramHeaders() {
  ListScope L(W, "ProgramHeaders");

  for (typename ELFO::Elf_Phdr_Iter PI = Obj->begin_program_headers(),
                                    PE = Obj->end_program_headers();
                                    PI != PE; ++PI) {
    DictScope P(W, "ProgramHeader");
    W.printHex   ("Type",
                  getElfSegmentType(Obj->getHeader()->e_machine, PI->p_type),
                  PI->p_type);
    W.printHex   ("Offset", PI->p_offset);
    W.printHex   ("VirtualAddress", PI->p_vaddr);
    W.printHex   ("PhysicalAddress", PI->p_paddr);
    W.printNumber("FileSize", PI->p_filesz);
    W.printNumber("MemSize", PI->p_memsz);
    W.printFlags ("Flags", PI->p_flags, makeArrayRef(ElfSegmentFlags));
    W.printNumber("Alignment", PI->p_align);
  }
}

template <class ELFT>
void ELFDumper<ELFT>::printAttributes() {
  W.startLine() << "Attributes not implemented.\n";
}

namespace {
template <>
void ELFDumper<ELFType<support::little, 2, false> >::printAttributes() {
  if (Obj->getHeader()->e_machine != EM_ARM) {
    W.startLine() << "Attributes not implemented.\n";
    return;
  }

  DictScope BA(W, "BuildAttributes");
  for (ELFO::Elf_Shdr_Iter SI = Obj->begin_sections(), SE = Obj->end_sections();
       SI != SE; ++SI) {
    if (SI->sh_type != ELF::SHT_ARM_ATTRIBUTES)
      continue;

    ErrorOr<ArrayRef<uint8_t> > Contents = Obj->getSectionContents(&(*SI));
    if (!Contents)
      continue;

    if ((*Contents)[0] != ARMBuildAttrs::Format_Version) {
      errs() << "unrecognised FormatVersion: 0x" << utohexstr((*Contents)[0])
             << '\n';
      continue;
    }

    W.printHex("FormatVersion", (*Contents)[0]);
    if (Contents->size() == 1)
      continue;

    ARMAttributeParser(W).Parse(*Contents);
  }
}
}

