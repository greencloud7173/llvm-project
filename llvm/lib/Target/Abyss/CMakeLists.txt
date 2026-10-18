add_llvm_component_group(Abyss)

set(LLVM_TARGET_DEFINITIONS Abyss.td)

tablegen(LLVM AbyssGenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM AbyssGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM AbyssGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM AbyssGenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM AbyssGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM AbyssGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(AbyssCommonTableGen)

# Assembler-only milestone: only the target registration entry point is needed
# here.  The real TargetMachine will replace this stub when codegen is added.
add_llvm_target(AbyssCodeGen
  AbyssCodeGenStub.cpp

  # AbyssTargetMachine.cpp is still a work-in-progress Sparc copy and is
  # intentionally excluded until the real TargetMachine is ported.
  PARTIAL_SOURCES_INTENDED

  LINK_COMPONENTS
  Support

  ADD_TO_COMPONENT
  Abyss
  )

add_subdirectory(AsmParser)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
