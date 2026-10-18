add_llvm_component_library(LLVMAbyssDesc
  AbyssMCTargetDesc.cpp
  AbyssInstPrinter.cpp
  AbyssMCCodeEmitter.cpp
  AbyssAsmBackend.cpp
  AbyssELFObjectWriter.cpp

  LINK_COMPONENTS
  MC
  AbyssInfo
  Support
  TargetParser

  ADD_TO_COMPONENT
  Abyss
  )
