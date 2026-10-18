

let Namespace = "Abyss" in {

// Register::HWEncoding is bits<16>; our encodings only use the low 5 bits.
class AbyssReg<string n, bits<16> enc> : Register<n> {
  let HWEncoding = enc;
}

// x0: hard-wired zero (RISC-V x0 semantics).
def X0 : AbyssReg<"x0", 0>, DwarfRegNum<[0]> {
  let isConstant = true;
  let AltNames = ["zero", "R0"];
}

// R1..R28: general-purpose registers.
foreach I = 1-28 in {
  def X#I : AbyssReg<"R"#I, I>, DwarfRegNum<[I]>;
}

// R29..R31: read-only SIMD special registers.
// Assembler accepts %blockIdx / %blockDim / %threadIdx as canonical names.
def X29 : AbyssReg<"blockIdx", 29>, DwarfRegNum<[29]> {
  let AltNames = ["R29", "%blockIdx"];
}
def X30 : AbyssReg<"blockDim", 30>, DwarfRegNum<[30]> {
  let AltNames = ["R30", "%blockDim"];
}
def X31 : AbyssReg<"threadIdx", 31>, DwarfRegNum<[31]> {
  let AltNames = ["R31", "%threadIdx"];
}

// All 32 registers are legal assembly operands.  Codegen must reserve
// X0 and X29..X31 in AbyssRegisterInfo::getReservedRegs().
def XRegs : RegisterClass<"Abyss", [i32], 32,
    (add (sequence "X%u", 1, 28), X0, X29, X30, X31)>;

} // end Namespace "Abyss"