//===-- AbyssAsmBackend.cpp - Abyss assembler backend ---------------------===//

#include "AbyssMCFixups.h"
#include "AbyssMCTargetDesc.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/ErrorHandling.h"
#include <cassert>
#include <cstdint>

using namespace llvm;

namespace {

class AbyssAsmBackend : public MCAsmBackend {
public:
  AbyssAsmBackend() : MCAsmBackend(endianness::little) {}

  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data, uint64_t Value,
                  bool IsResolved) override;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override;

  MCFixupKindInfo getFixupKindInfo(MCFixupKind Kind) const override;

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override;
};

// Patch the scattered B-type immediate.  Data points at the instruction.
static void patchBranchImmediate(uint8_t *Data, int64_t Value) {
  uint32_t Inst = support::endian::read32le(Data);
  uint32_t Off = static_cast<uint32_t>(Value);
  Inst &= ~((1u << 31) | (0x3fu << 25) | (0xfu << 8) | (1u << 7));
  Inst |= ((Off >> 12) & 1u) << 31;
  Inst |= ((Off >> 5) & 0x3fu) << 25;
  Inst |= ((Off >> 1) & 0xfu) << 8;
  Inst |= ((Off >> 11) & 1u) << 7;
  support::endian::write32le(Data, Inst);
}

// Patch the scattered J-type immediate.  Data points at the instruction.
static void patchJalImmediate(uint8_t *Data, int64_t Value) {
  uint32_t Inst = support::endian::read32le(Data);
  uint32_t Off = static_cast<uint32_t>(Value);
  Inst &= ~((1u << 31) | (0x3ffu << 21) | (1u << 20) | (0xffu << 12));
  Inst |= ((Off >> 20) & 1u) << 31;
  Inst |= ((Off >> 1) & 0x3ffu) << 21;
  Inst |= ((Off >> 11) & 1u) << 20;
  Inst |= ((Off >> 12) & 0xffu) << 12;
  support::endian::write32le(Data, Inst);
}

} // namespace

void AbyssAsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                                 const MCValue &Target, uint8_t *Data,
                                 uint64_t Value, bool IsResolved) {
  maybeAddReloc(F, Fixup, Target, Value, IsResolved);

  switch (Fixup.getKind()) {
  case Abyss::FK_ABYSS_BRANCH:
    if (Value & 3)
      report_fatal_error("branch target must be 4-byte aligned");
    patchBranchImmediate(Data, static_cast<int64_t>(Value));
    break;
  case Abyss::FK_ABYSS_JAL:
    if (Value & 1)
      report_fatal_error("JAL target must be 2-byte aligned");
    patchJalImmediate(Data, static_cast<int64_t>(Value));
    break;
  case FK_Data_4:
    support::endian::write32le(Data, static_cast<uint32_t>(Value));
    break;
  default:
    llvm_unreachable("unexpected fixup kind");
  }
}

std::unique_ptr<MCObjectTargetWriter>
AbyssAsmBackend::createObjectTargetWriter() const {
  return createAbyssELFObjectWriter(0);
}

MCFixupKindInfo AbyssAsmBackend::getFixupKindInfo(MCFixupKind Kind) const {
  static const MCFixupKindInfo Infos[Abyss::NumTargetFixupKinds] = {
      {"FK_ABYSS_BRANCH", 0, 32, 0},
      {"FK_ABYSS_JAL", 0, 32, 0},
  };

  if (Kind < FirstTargetFixupKind)
    return MCAsmBackend::getFixupKindInfo(Kind);

  assert(unsigned(Kind - FirstTargetFixupKind) < Abyss::NumTargetFixupKinds &&
         "invalid target fixup kind");
  return Infos[Kind - FirstTargetFixupKind];
}

bool AbyssAsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                   const MCSubtargetInfo *STI) const {
  if (Count % 4 != 0)
    return false;

  // NOP = ADDI x0, x0, #0
  for (uint64_t I = 0; I < Count; I += 4)
    support::endian::write<uint32_t>(OS, 0x00000013, endianness::little);
  return true;
}

MCAsmBackend *llvm::createAbyssAsmBackend(const Target &,
                                          const MCSubtargetInfo &,
                                          const MCRegisterInfo &,
                                          const MCTargetOptions &) {
  return new AbyssAsmBackend();
}
