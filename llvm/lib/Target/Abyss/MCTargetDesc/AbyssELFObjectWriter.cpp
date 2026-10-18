//===-- AbyssELFObjectWriter.cpp - Abyss ELF object writer ----------------===//

#include "AbyssMCFixups.h"
#include "AbyssMCTargetDesc.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"
#include <cstdint>

using namespace llvm;

namespace {

// TODO: assign a real EM_ABYSS value and add ELFRelocs/Abyss.def once the
// target is upstreamed.  These placeholders are enough for local testing.
enum : unsigned { R_ABYSS_NONE = 0, R_ABYSS_32 = 1 };

class AbyssELFObjectWriter : public MCELFObjectTargetWriter {
public:
  explicit AbyssELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI,
                                /*EMachine=*/0 /* EM_NONE placeholder */,
                                /*HasRelocationAddend=*/false) {}

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &,
                        bool IsPCRel) const override {
    switch (Fixup.getKind()) {
    case FK_Data_4:
      return R_ABYSS_32;
    case Abyss::FK_ABYSS_BRANCH:
    case Abyss::FK_ABYSS_JAL:
      return IsPCRel ? R_ABYSS_32 : R_ABYSS_NONE;
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
