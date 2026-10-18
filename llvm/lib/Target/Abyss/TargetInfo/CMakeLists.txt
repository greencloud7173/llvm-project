add_llvm_component_library(LLVMAbyssInfo
  # The implementation currently lives one level up.
  ../AbyssTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support

  ADD_TO_COMPONENT
  Abyss
  )
