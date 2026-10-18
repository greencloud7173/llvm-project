//===-- AbyssMCCodeEmitter.cpp - Abyss instruction encoder ----------------===//

#include "AbyssMCFixups.h"
#include "AbyssMCTargetDesc.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>

using namespace llvm;

namespace {

class AbyssMCCodeEmitter : public MCCodeEmitter {
  const MCRegisterInfo &MRI;
  MCContext &Ctx;

public:
  AbyssMCCodeEmitter(const MCInstrInfo &, const MCRegisterInfo &MRI,
                     MCContext &Ctx)
      : MRI(MRI), Ctx(Ctx) {}

  uint64_t getBinaryCodeForInstr(const MCInst &MI,
                                 SmallVectorImpl<MCFixup> &Fixups,
                                 const MCSubtargetInfo &STI) const;

  unsigned getMachineOpValue(const MCInst &MI, const MCOperand &MO,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI) const;

  void encodeInstruction(const MCInst &MI, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const override;
};

} // namespace

MCCodeEmitter *llvm::createAbyssMCCodeEmitter(const MCInstrInfo &MCII,
                                              MCContext &Ctx) {
  return new AbyssMCCodeEmitter(MCII, *Ctx.getRegisterInfo(), Ctx);
}

unsigned AbyssMCCodeEmitter::getMachineOpValue(
    const MCInst &MI, const MCOperand &MO,
    SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  if (MO.isReg())
    return MRI.getEncodingValue(MO.getReg());

  if (MO.isImm())
    return static_cast<unsigned>(MO.getImm());

  assert(MO.isExpr() && "expected register, immediate or expression");

  // Only branch/jump operands may be symbolic; this keeps the first
  // assembler implementation simple and correct.
  switch (MI.getOpcode()) {
  case Abyss::BEQ:
  case Abyss::BNE:
  case Abyss::BLT:
  case Abyss::BGE:
  case Abyss::BLTU:
  case Abyss::BGEU:
    Fixups.push_back(
        MCFixup::create(0, MO.getExpr(), Abyss::FK_ABYSS_BRANCH, true));
    break;
  case Abyss::JAL:
    Fixups.push_back(
        MCFixup::create(0, MO.getExpr(), Abyss::FK_ABYSS_JAL, true));
    break;
  default:
    Ctx.reportError(MI.getLoc(),
                    "symbolic immediate is only supported for branch/jump");
    break;
  }
  return 0;
}

void AbyssMCCodeEmitter::encodeInstruction(
    const MCInst &MI, SmallVectorImpl<char> &CB,
    SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  uint64_t Value = getBinaryCodeForInstr(MI, Fixups, STI);
  raw_svector_ostream OS(CB);
  support::endian::write<uint32_t>(OS, Value, endianness::little);
}

#include "AbyssGenMCCodeEmitter.inc"
