add_llvm_component_library(LLVMAbyssAsmParser
  AbyssAsmParser.cpp

  LINK_COMPONENTS
  MC
  MCParser
  AbyssDesc
  AbyssInfo
  Support

  ADD_TO_COMPONENT
  Abyss
  )
