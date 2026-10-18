//===-- AbyssMCTargetDesc.h - Abyss MC target descriptions ------*- C++ -*-===//

#ifndef LLVM_LIB_TARGET_ABYSS_MCTARGETDESC_ABYSSMCTARGETDESC_H
#define LLVM_LIB_TARGET_ABYSS_MCTARGETDESC_ABYSSMCTARGETDESC_H

#include <cstdint>
#include <memory>

namespace llvm {

class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCInstrInfo;
class MCObjectTargetWriter;
class MCRegisterInfo;
class MCSubtargetInfo;
class MCTargetOptions;
class Target;

MCCodeEmitter *createAbyssMCCodeEmitter(const MCInstrInfo &MCII, MCContext &Ctx);

MCAsmBackend *createAbyssAsmBackend(const Target &T, const MCSubtargetInfo &STI,
                                    const MCRegisterInfo &MRI,
                                    const MCTargetOptions &Options);

std::unique_ptr<MCObjectTargetWriter> createAbyssELFObjectWriter(uint8_t OSABI);

} // namespace llvm

// Generated register, instruction and subtarget enums.
#define GET_REGINFO_ENUM
#include "AbyssGenRegisterInfo.inc"

#define GET_INSTRINFO_ENUM
#define GET_INSTRINFO_MC_HELPER_DECLS
#include "AbyssGenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "AbyssGenSubtargetInfo.inc"

#endif
