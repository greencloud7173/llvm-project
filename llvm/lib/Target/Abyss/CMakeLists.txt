add_llvm_component_group(Abyss)

set(LLVM_TARGET_DEFINITIONS Abyss.td)

tablegen(LLVM AbyssGenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM AbyssGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM AbyssGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM AbyssGenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM AbyssGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM AbyssGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(AbyssCommonTableGen)

# Minimal direct-IR backend: the Abyss MC layer already describes every
# instruction, so AbyssCodeGen.cpp emits MCInsts directly and avoids a large
# SelectionDAG/register-allocator implementation.
add_llvm_target(AbyssCodeGen
  AbyssCodeGen.cpp
  AbyssTargetMachineImpl.cpp

  PARTIAL_SOURCES_INTENDED

  LINK_COMPONENTS
  Analysis
  CodeGen
  Core
  MC
  AbyssDesc
  AbyssInfo
  Target
  Support

  ADD_TO_COMPONENT
  Abyss
  )

# The historical Sparc-derived TargetMachine/stub files were removed; this
# target now uses AbyssCodeGen.cpp + AbyssTargetMachineImpl.cpp exclusively.

add_subdirectory(AsmParser)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
