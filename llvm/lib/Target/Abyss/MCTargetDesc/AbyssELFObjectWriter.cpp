//===-- AbyssELFObjectWriter.cpp - Abyss ELF object writer ----------------===//

#include "AbyssMCFixups.h"
#include "AbyssMCTargetDesc.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"
#include <cstdint>

using namespace llvm;

namespace {

// Abyss uses RISC-V-compatible encodings and relocation layout.  Reuse the
// RISC-V ELF machine/relocation types so that stock lld can link Abyss objects.
class AbyssELFObjectWriter : public MCELFObjectTargetWriter {
public:
  explicit AbyssELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI, ELF::EM_RISCV,
                                /*HasRelocationAddend=*/true) {}

  bool needsRelocateWithSymbol(const MCValue &, unsigned Type) const override {
    return true;
  }

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &,
                        bool IsPCRel) const override {
    (void)IsPCRel;
    switch (Fixup.getKind()) {
    case FK_Data_4:
      return ELF::R_RISCV_32;
    case Abyss::FK_ABYSS_BRANCH:
      return ELF::R_RISCV_BRANCH;
    case Abyss::FK_ABYSS_JAL:
      return ELF::R_RISCV_JAL;
    default:
      llvm_unreachable("invalid fixup kind!");
    }
  }
};

} // namespace

std::unique_ptr<MCObjectTargetWriter>
llvm::createAbyssELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<AbyssELFObjectWriter>(OSABI);
}
