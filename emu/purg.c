/*
 * purg.c - RISC-V (RV32IMC + Zicsr/Zifencei) emulator: implementation (generated).
 *
 * Derived from atoomnetmarc/RISC-V-emulator (Apache-2.0, (c) Marc Ketel).
 * The only public surface is purg.h.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "purg.h"

typedef struct __attribute__((packed)) { uint32_t hartid : 32; } RiscvCSRmhartid_t;

typedef struct __attribute__((packed)) {
    uint8_t pmp0cfg;
    uint8_t pmp1cfg;
    uint8_t pmp2cfg;
    uint8_t pmp3cfg;
} RiscvCSRpmpcfg0_t;

typedef struct __attribute__((packed)) { uint32_t address; } RiscvCSRpmpaddr0_t;

typedef struct __attribute__((packed)) {
    uint8_t : 3;
    uint8_t nmie : 1;
    uint8_t : 3;
    uint8_t mnpv : 1;
    uint8_t : 3;
    uint8_t mnpp : 2;
    uint32_t : 19;
} RiscvCSRmnstatus_t;

typedef struct __attribute__((packed)) { uint32_t mscratch; } RiscvCSRmscratch_t;

typedef struct __attribute__((packed)) {
    uint32_t exceptioncode : 31;
    uint8_t interrupt : 1;
} RiscvCSRmcause_t;

typedef struct __attribute__((packed)) { uint32_t mip; } RiscvCSRmip_t;

typedef struct __attribute__((packed)) {
    uint8_t : 1;
    uint8_t sie : 1;
    uint8_t : 1;
    uint8_t mie : 1;
    uint8_t : 1;
    uint8_t spie : 1;
    uint8_t ube : 1;
    uint8_t mpie : 1;
    uint8_t spp : 1;
    uint8_t vs : 2;
    uint8_t mpp : 2;
    uint8_t fs : 2;
    uint8_t xs : 2;
    uint8_t mprv : 1;
    uint8_t sum : 1;
    uint8_t mxr : 1;
    uint8_t tvm : 1;
    uint8_t tw : 1;
    uint8_t tsr : 1;
    uint8_t : 8;
    uint8_t sd : 1;
} RiscvCSRmstatus_t;

typedef struct __attribute__((packed)) {
    uint8_t : 4;
    uint8_t sbe : 1;
    uint8_t mbe : 1;
    uint8_t gva : 1;
    uint8_t mpv : 1;
    uint32_t : 24;
} RiscvCSRmstatush_t;

typedef union {
    struct __attribute__((packed)) {
        uint32_t extensions : 26;
        uint8_t mxlen : 4;
        uint8_t mxl : 2;
    };

    struct __attribute__((packed)) {
        uint8_t a : 1;
        uint8_t b : 1;
        uint8_t c : 1;
        uint8_t d : 1;
        uint8_t e : 1;
        uint8_t f : 1;
        uint8_t g : 1;
        uint8_t h : 1;
        uint8_t i : 1;
        uint8_t j : 1;
        uint8_t k : 1;
        uint8_t l : 1;
        uint8_t m : 1;
        uint8_t n : 1;
        uint8_t o : 1;
        uint8_t p : 1;
        uint8_t q : 1;
        uint8_t r : 1;
        uint8_t s : 1;
        uint8_t t : 1;
        uint8_t u : 1;
        uint8_t v : 1;
        uint8_t w : 1;
        uint8_t x : 1;
        uint8_t y : 1;
        uint8_t z : 1;
    };
} RiscvCSRmisa_u;

typedef struct __attribute__((packed)) { uint32_t synchronousexceptions; } RiscvCSRmedeleg_t;
typedef struct __attribute__((packed)) { uint32_t interrupts; } RiscvCSRmideleg_t;

typedef union {
    struct __attribute__((packed)) {
        uint32_t mie;
    };

    struct __attribute__((packed)) {
        uint8_t : 1;
        uint8_t ssie : 1;
        uint8_t : 1;
        uint8_t msie : 1;
        uint8_t : 1;
        uint8_t stie : 1;
        uint8_t : 1;
        uint8_t mtie : 1;
        uint8_t : 1;
        uint8_t seie : 1;
        uint8_t : 1;
        uint8_t meie : 1;
        uint8_t : 4;
        uint16_t : 16;
    };
} RiscvCSRmie_u;

typedef struct __attribute__((packed)) {
    uint8_t mode : 2;
    uint32_t base : 30;
} RiscvCSRmtvec_t;

typedef struct __attribute__((packed)) {
    uint32_t ppn : 22;
    uint16_t asid : 9;
    uint8_t mode : 1;
} RiscvCSRsatp_t;

typedef struct __attribute__((packed)) {
    // Machine Information Registers
    RiscvCSRmhartid_t mhartid;

    // Machine Trap Setup
    RiscvCSRmstatus_t mstatus;
    RiscvCSRmisa_u misa;
    RiscvCSRmedeleg_t medeleg;
    RiscvCSRmideleg_t mideleg;
    RiscvCSRmie_u mie;
    RiscvCSRmtvec_t mtvec;
    RiscvCSRmstatush_t mstatush;

    // Machine Trap Handling
    RiscvCSRmscratch_t mscratch;
    uint32_t mepc; // Machine exception program counter.
    RiscvCSRmcause_t mcause;
    uint32_t mtval; // Machine bad address or instruction.
    RiscvCSRmip_t mip;

    // Machine Memory Protection
    RiscvCSRpmpcfg0_t pmpcfg0;
    RiscvCSRpmpaddr0_t pmpaddr0;

    // Machine Non-Maskable Interrupt Handling
    RiscvCSRmnstatus_t mnstatus;

    // Supervisor Protection and Translation
    RiscvCSRsatp_t satp;
} RiscvCSR_t;

typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t imm11 : 1;
    uint8_t imm4_1 : 4;
    uint8_t funct3 : 3;
    uint8_t rs1 : 5;
    uint8_t rs2 : 5;
    uint8_t imm10_5 : 6;
    uint8_t imm12 : 1;
} RiscvInstructionTypeB_t;

typedef union {
    struct __attribute__((packed)) {
        uint8_t imm0 : 1;
        uint8_t imm4_1 : 4;
        uint8_t imm10_5 : 6;
        uint8_t imm11 : 1;
        uint8_t imm12 : 1;
    } bit;

    struct __attribute__((packed)) {
        int16_t imm : 13;
    };
} RiscvInstructionTypeBDecoderImm_u;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rs2 : 5;
    uint8_t rd : 5;
    uint8_t funct4 : 4;
} RiscvInstructionTypeCR_t;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t imm4_0 : 5;
    uint8_t rd : 5;
    uint8_t imm5 : 1;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCI_t;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t imm5 : 1;
    uint8_t imm8_7 : 2;
    uint8_t imm6 : 1;
    uint8_t imm4 : 1;
    uint8_t rd : 5;
    uint8_t imm9 : 1;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCIAddi16sp_t;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t imm16_12 : 5;
    uint8_t rd : 5;
    uint8_t imm17 : 1;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCILui_t;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t imm7_6 : 2;
    uint8_t imm4_2 : 3;
    uint8_t rd : 5;
    uint8_t imm5 : 1;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCILwsp_t;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rs2 : 5;
    uint8_t imm7_6 : 2;
    uint8_t imm5_2 : 4;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCSS_t;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rdp : 3;
    uint8_t imm3 : 1;
    uint8_t imm2 : 1;
    uint8_t imm9_6 : 4;
    uint8_t imm5_4 : 2;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCIW_t;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rdp : 3;
    uint8_t imm6 : 1;
    uint8_t imm2 : 1;
    uint8_t rs1p : 3;
    uint8_t imm5_3 : 3;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCL_t;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rs2p : 3;
    uint8_t imm6 : 1;
    uint8_t imm2 : 1;
    uint8_t rs1p : 3;
    uint8_t imm5_3 : 3;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCS_t;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rs2p : 3;
    uint8_t funct2 : 2;
    uint8_t rdp : 3;
    uint8_t funct6 : 6;
} RiscvInstructionTypeCA_t;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t imm5 : 1;
    uint8_t imm2_1 : 2;
    uint8_t imm7_6 : 2;
    uint8_t rs1p : 3;
    uint8_t imm4_3 : 2;
    uint8_t imm8 : 1;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCB_t;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t imm4_0 : 5;
    uint8_t rdp : 3;
    uint8_t funct2 : 2;
    uint8_t imm5 : 1;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCBImm_t;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t imm5 : 1;
    uint8_t imm3_1 : 3;
    uint8_t imm7 : 1;
    uint8_t imm6 : 1;
    uint8_t imm10 : 1;
    uint8_t imm9_8 : 2;
    uint8_t imm4 : 1;
    uint8_t imm11 : 1;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCJ_t;

typedef union {
    struct __attribute__((packed)) {
        uint8_t imm4_0 : 5;
        uint8_t imm5 : 1;
    } bit;

    struct __attribute__((packed)) {
        int32_t imm : 6;
    };
} RiscvInstructionTypeCIDecoderImm_u;

typedef union {
    struct __attribute__((packed)) {
        uint8_t : 4;
        uint8_t imm4 : 1;
        uint8_t imm5 : 1;
        uint8_t imm6 : 1;
        uint8_t imm8_7 : 2;
        uint8_t imm9 : 1;
    } bit;

    struct __attribute__((packed)) {
        int16_t imm : 10;
    };
} RiscvInstructionTypeCIAddi16spDecoderImm_u;

typedef union {
    struct __attribute__((packed)) {
        uint16_t : 12;
        uint8_t imm16_12 : 5;
        uint8_t imm17 : 1;
    } bit;

    struct __attribute__((packed)) {
        int32_t imm : 18;
    };
} RiscvInstructionTypeCILuiDecoderImm_u;

typedef union {
    struct __attribute__((packed)) {
        uint8_t : 2;
        uint8_t imm4_2 : 3;
        uint8_t imm5 : 1;
        uint8_t imm7_6 : 2;
    } bit;

    struct __attribute__((packed)) {
        uint8_t imm : 8;
    };
} RiscvInstructionTypeCILwspDecoderImm_u;

typedef union {
    struct __attribute__((packed)) {
        uint8_t imm1_0 : 2;
        uint8_t imm5_2 : 4;
        uint8_t imm7_6 : 2;
    } bit;

    struct __attribute__((packed)) {
        uint8_t imm : 8;
    };
} RiscvInstructionTypeCSSDecoderImm_u;

typedef union {
    struct __attribute__((packed)) {
        uint8_t imm1_0 : 2;
        uint8_t imm2 : 1;
        uint8_t imm3 : 1;
        uint8_t imm5_4 : 2;
        uint8_t imm9_6 : 4;
    } bit;

    struct __attribute__((packed)) {
        uint16_t imm : 10;
    };
} RiscvInstructionTypeCIWDecoderImm_u;

typedef union {
    struct __attribute__((packed)) {
        uint8_t : 2;
        uint8_t imm2 : 1;
        uint8_t imm5_3 : 3;
        uint8_t imm6 : 1;
    } bit;

    struct __attribute__((packed)) {
        uint8_t imm : 7;
    };
} RiscvInstructionTypeCLDecoderImm_u;

typedef union {
    struct __attribute__((packed)) {
        uint8_t : 2;
        uint8_t imm2 : 1;
        uint8_t imm5_3 : 3;
        uint8_t imm6 : 1;
    } bit;

    struct __attribute__((packed)) {
        uint8_t imm : 7;
    };
} RiscvInstructionTypeCSDecoderImm_u;

typedef union {
    struct __attribute__((packed)) {
        uint8_t funct2 : 2;
        uint8_t funct6 : 6;
    };

    struct __attribute__((packed)) {
        uint8_t funct6_funct2 : 8;
    };
} RiscvInstructionTypeCADecoderFunct6Funct2_u;

typedef union {
    struct __attribute__((packed)) {
        uint8_t : 1;
        uint8_t imm2_1 : 2;
        uint8_t imm4_3 : 2;
        uint8_t imm5 : 1;
        uint8_t imm7_6 : 2;
        uint8_t imm8 : 1;
    } bit;

    struct __attribute__((packed)) {
        int16_t imm : 9;
    };
} RiscvInstructionTypeCBDecoderImm_u;

typedef union {
    struct __attribute__((packed)) {
        uint8_t imm4_0 : 5;
        uint8_t imm5 : 1;
    } bit;

    struct __attribute__((packed)) {
        int16_t imm : 6;
    };
} RiscvInstructionTypeCBImmDecoderImm_u;

typedef union {
    struct __attribute__((packed)) {
        uint8_t funct2 : 2;
        uint8_t funct3 : 3;
    };

    struct __attribute__((packed)) {
        uint8_t funct3_funct2 : 5;
    };
} RiscvInstructionTypeCBDecoderFunct3Funct2_u;

typedef union {
    struct __attribute__((packed)) {
        uint8_t : 1;
        uint8_t imm3_1 : 3;
        uint8_t imm4 : 1;
        uint8_t imm5 : 1;
        uint8_t imm6 : 1;
        uint8_t imm7 : 1;
        uint8_t imm9_8 : 2;
        uint8_t imm10 : 1;
        uint8_t imm11 : 1;
    } bit;

    struct __attribute__((packed)) {
        int16_t imm : 12;
    };
} RiscvInstructionTypeCJDecoderImm_u;

typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint16_t : 11;
    uint8_t funct3 : 3;
} RiscvInstructionOpcodeC_t;

typedef union {
    struct __attribute__((packed)) {
        uint8_t op : 2;
        uint8_t funct3 : 3;
    };

    struct __attribute__((packed)) {
        uint8_t opfunct3 : 5;
    };
} RiscvInstructionTypeCDecoderOpcode_u;

typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t rd : 5;
    uint8_t funct3 : 3;
    uint8_t rs1 : 5;
    int16_t imm : 12;
} RiscvInstructionTypeI_t;

typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t rd : 5;
    uint8_t funct3 : 3;
    uint8_t rs1 : 5;
    uint16_t funct12 : 12;
} RiscvInstructionTypeIStystem_t;

typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t rd : 5;
    uint8_t funct3 : 3;
    uint8_t rs1 : 5;
    uint16_t csr : 12;
} RiscvInstructionTypeICSR_t;

typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t rd : 5;
    uint8_t funct3 : 3;
    uint8_t imm : 5;
    uint16_t csr : 12;
} RiscvInstructionTypeICSRImm_t;

typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t rd : 5;
    uint8_t funct3 : 3;
    uint8_t rs1 : 5;
    uint8_t shamt : 5;
    uint8_t imm11_5 : 7;
} RiscvInstructionTypeIShiftByConstant_t;

typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t rd : 5;
    uint8_t funct3 : 3;
    uint8_t rs1 : 5;
    uint8_t sw : 1;
    uint8_t sr : 1;
    uint8_t so : 1;
    uint8_t si : 1;
    uint8_t pw : 1;
    uint8_t pr : 1;
    uint8_t po : 1;
    uint8_t pi : 1;
    uint8_t fm : 4;
} RiscvInstructionTypeIMiscMemt_t;

typedef union {
    struct __attribute__((packed)) {
        uint8_t funct3 : 3;
        int16_t imm11_5 : 7;
    };

    struct __attribute__((packed)) {
        uint16_t imm11_5funct3 : 10;
    };
} RiscvInstructionTypeIDecoderImm11_7Funct3Imm11_7Funct3_u;

typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t rd : 5;
    uint8_t imm19_12 : 8;
    uint8_t imm11 : 1;
    uint16_t imm10_1 : 10;
    uint8_t imm20 : 1;
} RiscvInstructionTypeJ_t;

typedef union {
    struct __attribute__((packed)) {
        uint8_t : 1;
        uint16_t imm10_1 : 10;
        uint8_t imm11 : 1;
        uint8_t imm19_12 : 8;
        uint8_t imm20 : 1;
    } bit;

    struct __attribute__((packed)) {
        int32_t imm : 21;
    };
} RiscvInstructionTypeJDecoderImm_u;

typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t rd : 5;
    uint8_t funct3 : 3;
    uint8_t rs1 : 5;
    uint8_t rs2 : 5;
    uint8_t funct7 : 7;
} RiscvInstructionTypeR_t;

typedef union {
    struct __attribute__((packed)) {
        uint8_t funct3 : 3;
        uint8_t funct7 : 7;
    };

    struct __attribute__((packed)) {
        uint16_t funct7_3 : 10;
    };
} RiscvInstructionTypeRDecoderFunct7Funct3_u;

typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t imm4_0 : 5;
    uint8_t funct3 : 3;
    uint8_t rs1 : 5;
    uint8_t rs2 : 5;
    uint8_t imm11_5 : 7;
} RiscvInstructionTypeS_t;

typedef union {
    struct __attribute__((packed)) {
        uint8_t imm4_0 : 5;
        uint8_t imm11_5 : 7;
    } bit;

    struct __attribute__((packed)) {
        int16_t imm : 12;
    };
} RiscvInstructionTypeSDecoderImm_u;

typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t rd : 5;
    uint32_t imm31_12 : 20;
} RiscvInstructionTypeU_t;

typedef union {
    struct __attribute__((packed)) {
        uint16_t imm11_0 : 12;
        uint32_t imm31_12 : 20;
    } bit;

    struct __attribute__((packed)) {
        uint32_t imm : 32;
    };
} RiscvInstructionTypeUDecoderImm_u;

typedef union {
    uint32_t value;

    struct __attribute__((packed)) {
        uint8_t opcode : 7;
    };

    struct __attribute__((packed)) {
        uint16_t L;
        uint16_t H;
    };
    RiscvInstructionOpcodeC_t copcode;
    RiscvInstructionTypeCR_t crtype;
    RiscvInstructionTypeCI_t citype;
    RiscvInstructionTypeCIAddi16sp_t ciaddi16sp;
    RiscvInstructionTypeCILui_t cilui;
    RiscvInstructionTypeCILwsp_t cilwsp;
    RiscvInstructionTypeCSS_t csstype;
    RiscvInstructionTypeCIW_t ciwtype;
    RiscvInstructionTypeCL_t cltype;
    RiscvInstructionTypeCS_t cstype;
    RiscvInstructionTypeCA_t catype;
    RiscvInstructionTypeCB_t cbtype;
    RiscvInstructionTypeCBImm_t cbimm;
    RiscvInstructionTypeCJ_t cjtype;
    RiscvInstructionTypeR_t rtype;
    RiscvInstructionTypeI_t itype;
    RiscvInstructionTypeICSR_t itypecsr;
    RiscvInstructionTypeICSRImm_t itypecsrimm;
    RiscvInstructionTypeIMiscMemt_t itypemiscmem;
    RiscvInstructionTypeIShiftByConstant_t itypeshiftbyconstant;
    RiscvInstructionTypeIStystem_t itypesystem;
    RiscvInstructionTypeS_t stype;
    RiscvInstructionTypeB_t btype;
    RiscvInstructionTypeU_t utype;
    RiscvInstructionTypeJ_t jtype;
} RiscvInstruction_u;

typedef union {
    struct {
        uint32_t Zero;
        uint32_t ra;
        uint32_t sp;
        uint32_t gp;
        uint32_t tp;
        uint32_t t0;
        uint32_t t1;
        uint32_t t2;
        uint32_t s0_fp;
        uint32_t s1;
        uint32_t a0;
        uint32_t a1;
        uint32_t a2;
        uint32_t a3;
        uint32_t a4;
        uint32_t a5;
        uint32_t a6;
        uint32_t a7;
        uint32_t s2;
        uint32_t s3;
        uint32_t s4;
        uint32_t s5;
        uint32_t s6;
        uint32_t s7;
        uint32_t s8;
        uint32_t s9;
        uint32_t s10;
        uint32_t s11;
        uint32_t t3;
        uint32_t t4;
        uint32_t t5;
        uint32_t t6;
    };
    uint32_t x[32];
} RiscvRegister_u;

typedef union {
    struct {
        uint8_t illegalinstruction : 1;
        uint8_t instructionaddressmisaligned : 1;
        uint8_t breakpoint : 1;
        uint8_t loadaddressmisaligned : 1;
        uint8_t storeaddressmisaligned : 1;
        uint8_t environmentcallfrommmode : 1;
    };
    uint8_t value;
} RiscvEmulatorTrapFlag_u;

struct RiscvEmulatorState {
    RiscvEmulatorTrapFlag_u trapflag;
    uint32_t programcounter;
    uint32_t programcounternext;
    RiscvInstruction_u instruction;
    RiscvRegister_u reg;
    RiscvCSR_t csr;
};

#define ROM_ORIGIN 0x20000000

// B-type, branch.

#define FUNCT3_BRANCH_BEQ 0b000
#define FUNCT3_BRANCH_BNE 0b001
#define FUNCT3_BRANCH_BLT 0b100
#define FUNCT3_BRANCH_BGE 0b101
#define FUNCT3_BRANCH_BLTU 0b110
#define FUNCT3_BRANCH_BGEU 0b111

#define BRANCH_YES 0
#define BRANCH_NO 1

// Machine cause exception code.

#define MCAUSE_EXCEPTION_CODE_INSTRUCTION_ADDRESS_MISALIGNED 0
#define MCAUSE_EXCEPTION_CODE_ILLEGAL_INSTRUCTION 2
#define MCAUSE_EXCEPTION_CODE_BREAKPOINT 3
#define MCAUSE_EXCEPTION_CODE_LOAD_ADDRESS_MISALIGNED 4
#define MCAUSE_EXCEPTION_CODE_STORE_ADDRESS_MISALIGNED 6
#define MCAUSE_EXCEPTION_CODE_ENVIRONMENT_CALL_FROM_MMODE 11

// Compressed Register.

#define FUNCT4_MV 0b1000
#define FUNCT4_ADD 0b1001

// Compressed Arithmetic.

#define FUNCT6_FUNCT2_SUB 0b10001100
#define FUNCT6_FUNCT2_XOR 0b10001101
#define FUNCT6_FUNCT2_OR 0b10001110
#define FUNCT6_FUNCT2_AND 0b10001111

// Compressed Branch.

#define FUNCT3_FUNCT2_SRLI 0b10000
#define FUNCT3_FUNCT2_SRAI 0b10001
#define FUNCT3_FUNCT2_ANDI 0b10010

// I-type, register immediate.

#define FUNCT3_IMMEDIATE_ADDI 0b000
#define FUNCT3_IMMEDIATE_SLTI 0b010
#define FUNCT3_IMMEDIATE_SLTIU 0b011
#define FUNCT3_IMMEDIATE_XORI 0b100
#define FUNCT3_IMMEDIATE_ORI 0b110
#define FUNCT3_IMMEDIATE_ANDI 0b111

#define FUNCT3_IMMEDIATE_FUNCTIONS_1 0b001
#define FUNCT3_IMMEDIATE_FUNCTIONS_5 0b101

#define IMM11_5_FUNCT3_IMMEDIATE_SLLI 0b0000000001
#define IMM11_5_FUNCT3_IMMEDIATE_SRLI 0b0000000101
#define IMM11_5_FUNCT3_IMMEDIATE_SRAI 0b0100000101

#define FUNCT3_JUMPANDLINKREGISTER_JALR 0b000

#define FUNCT3_LOAD_LB 0b000
#define FUNCT3_LOAD_LH 0b001
#define FUNCT3_LOAD_LW 0b010
#define FUNCT3_LOAD_LBU 0b100
#define FUNCT3_LOAD_LHU 0b101

#define FUNCT3_CSR_CSRRW 0b001
#define FUNCT3_CSR_CSRRS 0b010
#define FUNCT3_CSR_CSRRC 0b011
#define FUNCT3_CSR_CSRRWI 0b101
#define FUNCT3_CSR_CSRRSI 0b110
#define FUNCT3_CSR_CSRRCI 0b111

#define FUNCT12_ECALL 0b000000000000
#define FUNCT12_EBREAK 0b000000000001
#define FUNCT12_MRET 0b001100000010

#define FUNCT3_FENCE 0b000

#define FUNCT3_FENCEI 0b001

#define OPCODE16_QUADRANT_INVALID 0b11

// 16-bit opcodes when RV32. Bits [15:13][1:0].

#define OPCODE16_ADDI4SPN 0b00000
#define OPCODE16_LW 0b01000
#define OPCODE16_SW 0b11000
#define OPCODE16_ADDI 0b00001
#define OPCODE16_JAL 0b00101
#define OPCODE16_LI 0b01001
#define OPCODE16_LUI_ADDI16SP 0b01101
#define OPCODE16_MISCALU 0b10001
#define OPCODE16_J 0b10101
#define OPCODE16_BEQZ 0b11001
#define OPCODE16_BNEZ 0b11101
#define OPCODE16_SLLI 0b00010
#define OPCODE16_LWSP 0b01010
#define OPCODE16_JALR_MV_ADD 0b10010
#define OPCODE16_SWSP 0b11010

// 32-bit opcodes. Bits [6:0].

#define OPCODE32_LOAD 0b0000011
#define OPCODE32_MISCMEM 0b0001111
#define OPCODE32_IMMEDIATE 0b0010011
#define OPCODE32_ADDUPPERIMMEDIATE2PC 0b0010111
#define OPCODE32_STORE 0b0100011
#define OPCODE32_OPERATION 0b0110011
#define OPCODE32_LOADUPPERIMMEDIATE 0b0110111
#define OPCODE32_BRANCH 0b1100011
#define OPCODE32_JUMPANDLINKREGISTER 0b1100111
#define OPCODE32_JUMPANDLINK 0b1101111
#define OPCODE32_SYSTEM 0b1110011

// R-type, register register.

#define FUNCT7_FUNCT3_OPERATION_ADD 0b0000000000
#define FUNCT7_FUNCT3_OPERATION_SUB 0b0100000000
#define FUNCT7_FUNCT3_OPERATION_SLL 0b0000000001
#define FUNCT7_FUNCT3_OPERATION_SLT 0b0000000010
#define FUNCT7_FUNCT3_OPERATION_SLTU 0b0000000011
#define FUNCT7_FUNCT3_OPERATION_XOR 0b0000000100
#define FUNCT7_FUNCT3_OPERATION_SRL 0b0000000101
#define FUNCT7_FUNCT3_OPERATION_SRA 0b0100000101
#define FUNCT7_FUNCT3_OPERATION_OR 0b0000000110
#define FUNCT7_FUNCT3_OPERATION_AND 0b0000000111

#define FUNCT7_FUNCT3_OPERATION_MUL 0b0000001000
#define FUNCT7_FUNCT3_OPERATION_MULH 0b0000001001
#define FUNCT7_FUNCT3_OPERATION_MULHSU 0b0000001010
#define FUNCT7_FUNCT3_OPERATION_MULHU 0b0000001011
#define FUNCT7_FUNCT3_OPERATION_DIV 0b0000001100
#define FUNCT7_FUNCT3_OPERATION_DIVU 0b0000001101
#define FUNCT7_FUNCT3_OPERATION_REM 0b0000001110
#define FUNCT7_FUNCT3_OPERATION_REMU 0b0000001111

// S-type, store.

#define FUNCT3_STORE_SB 0b000
#define FUNCT3_STORE_SH 0b001
#define FUNCT3_STORE_SW 0b010

static void *RiscvEmulatorGetCSRAddress(RiscvEmulatorState_t *state, const uint16_t csr) {
    void *address = 0;
    switch (csr) {
    case 0xF14: address = &state->csr.mhartid; break;
    case 0x300: address = &state->csr.mstatus; break;
    case 0x302: address = &state->csr.medeleg; break;
    case 0x303: address = &state->csr.mideleg; break;
    case 0x304: address = &state->csr.mie; break;
    case 0x305: address = &state->csr.mtvec; break;
    case 0x310: address = &state->csr.mstatush; break;
    case 0x340: address = &state->csr.mscratch; break;
    case 0x341: address = &state->csr.mepc; break;
    case 0x342: address = &state->csr.mcause; break;
    case 0x343: address = &state->csr.mtval; break;
    case 0x344: address = &state->csr.mip; break;
    case 0x3A0: address = &state->csr.pmpcfg0; break;
    case 0x3B0: address = &state->csr.pmpaddr0; break;
    case 0x744: address = &state->csr.mnstatus; break;
    case 0x180: address = &state->csr.satp; break;
    default:
        address = RiscvEmulatorGetUnknownCSR(state, csr);
        if (address == 0) {
            state->trapflag.illegalinstruction = 1;
            RiscvEmulatorUnknownCSR(state);
        }
    }
    return address;
}

void RiscvEmulatorInit(RiscvEmulatorState_t *state, uint32_t initial_sp) {
    // Initialize stack pointer.
    state->reg.sp = initial_sp;

    // Initialize program counter.
    state->programcounter = ROM_ORIGIN;
    state->programcounternext = ROM_ORIGIN;

    // Initialize X0.
    state->reg.Zero = 0;

    // Initialize CSR.
    memset(&state->csr, 0, sizeof(state->csr));

    // Initialize trap flags.
    state->trapflag.value = 0;
}

void RiscvEmulatorLoop(RiscvEmulatorState_t *state) {
    state->programcounter = state->programcounternext;
    uint8_t instructionlength = 32;

    // Read 16 bits.
    state->instruction.H = 0;
    RiscvEmulatorLoad(state->programcounter, &state->instruction.L, sizeof(state->instruction.L));
    state->programcounternext += sizeof(state->instruction.L);

    // Read another 16 bits when this is a 32-bit instruction.
    if (state->instruction.copcode.op == OPCODE16_QUADRANT_INVALID) {
        RiscvEmulatorLoad(state->programcounternext, &state->instruction.H,
                          sizeof(state->instruction.L));
        state->programcounternext += sizeof(state->instruction.L);
    } else {
        instructionlength = 16;
    }

    if (instructionlength == 16) {
        {
            RiscvInstructionTypeCDecoderOpcode_u decoderOpcode16 = {0};
            decoderOpcode16.funct3 = state->instruction.copcode.funct3;
            decoderOpcode16.op = state->instruction.copcode.op;
            uint8_t opfunct3 = decoderOpcode16.opfunct3;
            uint8_t funct3_funct2 = 0;
            uint8_t funct6_funct2 = 0;
            int8_t rdnum = -1;
            int8_t rs1num = -1;
            int8_t rs2num = -1;
            uint32_t imm = 0;
            void *rd = 0;
            void *rs1 = 0;
            void *rs2 = 0;
            void *sp = &state->reg.sp;
            void *ra = &state->reg.ra;

            switch (opfunct3) {
            case OPCODE16_ADDI4SPN: {
                RiscvInstructionTypeCIWDecoderImm_u immdecoder = {0};
                immdecoder.bit.imm2 = state->instruction.ciwtype.imm2;
                immdecoder.bit.imm3 = state->instruction.ciwtype.imm3;
                immdecoder.bit.imm5_4 = state->instruction.ciwtype.imm5_4;
                immdecoder.bit.imm9_6 = state->instruction.ciwtype.imm9_6;
                imm = immdecoder.imm;
                rdnum = state->instruction.ciwtype.rdp + 8;
                break;
            }
            case OPCODE16_MISCALU: {
                RiscvInstructionTypeCBDecoderFunct3Funct2_u funct = {0};
                funct.funct3 = state->instruction.cbimm.funct3;
                funct.funct2 = state->instruction.cbimm.funct2;
                funct3_funct2 = funct.funct3_funct2;

                if (funct3_funct2 == FUNCT3_FUNCT2_SRLI || funct3_funct2 == FUNCT3_FUNCT2_SRAI ||
                    funct3_funct2 == FUNCT3_FUNCT2_ANDI) {
                    RiscvInstructionTypeCBImmDecoderImm_u immdecoder = {0};
                    immdecoder.bit.imm4_0 = state->instruction.cbimm.imm4_0;
                    immdecoder.bit.imm5 = state->instruction.cbimm.imm5;
                    imm = immdecoder.imm;
                    rdnum = state->instruction.cbimm.rdp + 8;
                    break;
                }

                RiscvInstructionTypeCADecoderFunct6Funct2_u catfunct = {0};
                catfunct.funct6 = state->instruction.catype.funct6;
                catfunct.funct2 = state->instruction.catype.funct2;
                funct6_funct2 = catfunct.funct6_funct2;

                if (funct6_funct2 == FUNCT6_FUNCT2_SUB || funct6_funct2 == FUNCT6_FUNCT2_XOR ||
                    funct6_funct2 == FUNCT6_FUNCT2_OR || funct6_funct2 == FUNCT6_FUNCT2_AND) {
                    rdnum = state->instruction.catype.rdp + 8;
                    rs2num = state->instruction.catype.rs2p + 8;
                    break;
                }
                break;
            }
            case OPCODE16_BEQZ:
            case OPCODE16_BNEZ: {
                RiscvInstructionTypeCBDecoderImm_u immdecoder = {0};
                immdecoder.bit.imm2_1 = state->instruction.cbtype.imm2_1;
                immdecoder.bit.imm4_3 = state->instruction.cbtype.imm4_3;
                immdecoder.bit.imm5 = state->instruction.cbtype.imm5;
                immdecoder.bit.imm7_6 = state->instruction.cbtype.imm7_6;
                immdecoder.bit.imm8 = state->instruction.cbtype.imm8;
                imm = immdecoder.imm;
                rs1num = state->instruction.cbtype.rs1p + 8;
                break;
            }
            case OPCODE16_ADDI:
            case OPCODE16_LI:
            case OPCODE16_SLLI: {
                RiscvInstructionTypeCIDecoderImm_u immdecoder = {0};
                immdecoder.bit.imm4_0 = state->instruction.citype.imm4_0;
                immdecoder.bit.imm5 = state->instruction.citype.imm5;
                imm = immdecoder.imm;
                rdnum = state->instruction.citype.rd;
                break;
            }
            case OPCODE16_JAL:
            case OPCODE16_J: {
                RiscvInstructionTypeCJDecoderImm_u immdecoder = {0};
                immdecoder.bit.imm3_1 = state->instruction.cjtype.imm3_1;
                immdecoder.bit.imm4 = state->instruction.cjtype.imm4;
                immdecoder.bit.imm5 = state->instruction.cjtype.imm5;
                immdecoder.bit.imm6 = state->instruction.cjtype.imm6;
                immdecoder.bit.imm7 = state->instruction.cjtype.imm7;
                immdecoder.bit.imm9_8 = state->instruction.cjtype.imm9_8;
                immdecoder.bit.imm10 = state->instruction.cjtype.imm10;
                immdecoder.bit.imm11 = state->instruction.cjtype.imm11;
                imm = immdecoder.imm;
                break;
            }
            case OPCODE16_LW: {
                RiscvInstructionTypeCLDecoderImm_u immdecoder = {0};
                immdecoder.bit.imm2 = state->instruction.cltype.imm2;
                immdecoder.bit.imm5_3 = state->instruction.cltype.imm5_3;
                immdecoder.bit.imm6 = state->instruction.cltype.imm6;
                imm = immdecoder.imm;
                rs1num = state->instruction.cltype.rs1p + 8;
                rdnum = state->instruction.cltype.rdp + 8;
                break;
            }
            case OPCODE16_SW: {
                RiscvInstructionTypeCSDecoderImm_u immdecoder = {0};
                immdecoder.bit.imm2 = state->instruction.cstype.imm2;
                immdecoder.bit.imm5_3 = state->instruction.cstype.imm5_3;
                immdecoder.bit.imm6 = state->instruction.cstype.imm6;
                imm = immdecoder.imm;
                rs1num = state->instruction.cstype.rs1p + 8;
                rs2num = state->instruction.cstype.rs2p + 8;
                break;
            }
            case OPCODE16_LWSP: {
                RiscvInstructionTypeCILwspDecoderImm_u immdecoder = {0};
                immdecoder.bit.imm4_2 = state->instruction.cilwsp.imm4_2;
                immdecoder.bit.imm5 = state->instruction.cilwsp.imm5;
                immdecoder.bit.imm7_6 = state->instruction.cilwsp.imm7_6;
                imm = immdecoder.imm;
                rdnum = state->instruction.cilwsp.rd;
                break;
            }
            case OPCODE16_SWSP: {
                RiscvInstructionTypeCSSDecoderImm_u immdecoder = {0};
                immdecoder.bit.imm5_2 = state->instruction.csstype.imm5_2;
                immdecoder.bit.imm7_6 = state->instruction.csstype.imm7_6;
                imm = immdecoder.imm;
                rs2num = state->instruction.csstype.rs2;
                break;
            }
            }

            if (rdnum >= 0) rd = &state->reg.x[rdnum];
            if (rs1num >= 0) rs1 = &state->reg.x[rs1num];
            if (rs2num >= 0) rs2 = &state->reg.x[rs2num];

            switch (opfunct3) {
            case OPCODE16_ADDI4SPN:
                if (imm != 0) {
                    if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)sp + (uint16_t)imm;
                } else {
                    state->trapflag.illegalinstruction = 1;
                }
                break;
            case OPCODE16_LW: {
                uint32_t memorylocation = *(int32_t *)rs1 + (uint8_t)imm;
                if (state->trapflag.storeaddressmisaligned == 1) goto __inl_1;
                RiscvEmulatorLoad(memorylocation, rd, sizeof(uint32_t));

            __inl_1:;
            } break;
            case OPCODE16_SW: {
                uint32_t memorylocation = *(int32_t *)rs1 + (uint8_t)imm;
                if (state->trapflag.storeaddressmisaligned == 1) goto __inl_2;
                RiscvEmulatorStore(memorylocation, rs2, sizeof(uint32_t));

            __inl_2:;
            } break;
            case OPCODE16_ADDI:
                if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rd + (int8_t)imm;
                break;
            case OPCODE16_JAL: {
                *(uint32_t *)ra = state->programcounter + 2;
                state->programcounternext = state->programcounter + (int16_t)imm;
            } break;
            case OPCODE16_LI:
                if ((uint8_t)rdnum != 0) *(int32_t *)rd = (int8_t)imm;
                break;
            case OPCODE16_LUI_ADDI16SP: {
                rdnum = state->instruction.cilui.rd;
                rd = &state->reg.x[rdnum];
                if (rdnum == 2) {
                    {
                        RiscvInstructionTypeCIAddi16spDecoderImm_u immdecoder = {0};
                        immdecoder.bit.imm4 = state->instruction.ciaddi16sp.imm4;
                        immdecoder.bit.imm5 = state->instruction.ciaddi16sp.imm5;
                        immdecoder.bit.imm6 = state->instruction.ciaddi16sp.imm6;
                        immdecoder.bit.imm8_7 = state->instruction.ciaddi16sp.imm8_7;
                        immdecoder.bit.imm9 = state->instruction.ciaddi16sp.imm9;
                        int16_t nzimm = immdecoder.imm;
                        if (nzimm == 0) goto __inl_3;
                        *(int32_t *)rd += nzimm;

                    __inl_3:;
                    }
                } else {
                    {
                        RiscvInstructionTypeCILuiDecoderImm_u immdecoder = {0};
                        immdecoder.bit.imm16_12 = state->instruction.cilui.imm16_12;
                        immdecoder.bit.imm17 = state->instruction.cilui.imm17;
                        int32_t nzimm = immdecoder.imm;

                        if ((uint8_t)rdnum == 0) goto __inl_4;
                        *(int32_t *)rd = nzimm;

                    __inl_4:;
                    }
                }
                break;
            }
            case OPCODE16_MISCALU:
                if (funct3_funct2 == FUNCT3_FUNCT2_SRLI) {
                    if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)rd >> (uint8_t)imm;
                } else if (funct3_funct2 == FUNCT3_FUNCT2_SRAI) {
                    if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rd >> (uint8_t)imm;
                } else if (funct3_funct2 == FUNCT3_FUNCT2_ANDI) {
                    if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rd & (int8_t)imm;
                } else if (funct6_funct2 == FUNCT6_FUNCT2_SUB) {
                    if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rd - *(int32_t *)rs2;
                } else if (funct6_funct2 == FUNCT6_FUNCT2_XOR) {
                    if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)rd ^ *(uint32_t *)rs2;
                } else if (funct6_funct2 == FUNCT6_FUNCT2_OR) {
                    if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)rd | *(uint32_t *)rs2;
                } else if (funct6_funct2 == FUNCT6_FUNCT2_AND) {
                    if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)rd & *(uint32_t *)rs2;
                } else {
                    state->trapflag.illegalinstruction = 1;
                }
                break;
            case OPCODE16_J: {
                state->programcounternext = state->programcounter + (int16_t)imm;
            } break;
            case OPCODE16_BEQZ: {
                if (*(int32_t *)rs1 == 0)
                    state->programcounternext = state->programcounter + (int16_t)imm;
            } break;
            case OPCODE16_BNEZ: {
                if (*(int32_t *)rs1 != 0)
                    state->programcounternext = state->programcounter + (int16_t)imm;
            } break;
            case OPCODE16_SLLI:
                if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)rd << (uint8_t)imm;
                break;
            case OPCODE16_LWSP: {
                uint32_t memorylocation = *(int32_t *)sp + (uint8_t)imm;
                if ((uint8_t)rdnum == 0) goto __inl_5;
                RiscvEmulatorLoad(memorylocation, rd, sizeof(uint32_t));

            __inl_5:;
            } break;
            case OPCODE16_JALR_MV_ADD:
                rdnum = state->instruction.crtype.rd;
                rd = &state->reg.x[rdnum];
                rs2num = state->instruction.crtype.rs2;
                rs2 = &state->reg.x[rs2num];

                if (state->instruction.crtype.funct4 == FUNCT4_MV) {
                    if (rs2num == 0) {
                        { state->programcounternext = *(int32_t *)rd & (UINT32_MAX - 1); }
                    } else {
                        if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rs2;
                    }
                } else if (rdnum == 0 && rs2num == 0) {
                    {
                        state->trapflag.breakpoint = 1;
                        state->csr.mtval = state->programcounter;
                        RiscvEmulatorHandleEBREAK(state);
                    }
                } else if (rs2num == 0) {
                    {
                        uint32_t target = *(int32_t *)rd;
                        *(uint32_t *)ra = state->programcounter + 2;
                        state->programcounternext = target & (UINT32_MAX - 1);
                    }
                } else {
                    if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rd + *(int32_t *)rs2;
                }
                break;
            case OPCODE16_SWSP: {
                uint32_t memorylocation = *(int32_t *)sp + (uint8_t)imm;
                RiscvEmulatorStore(memorylocation, rs2, sizeof(uint32_t));
            } break;
            default: state->trapflag.illegalinstruction = 1; break;
            }
        }
    }

    if (instructionlength == 32) {
        switch (state->instruction.opcode) {
        case OPCODE32_JUMPANDLINKREGISTER: {
            if (state->instruction.itype.funct3 == FUNCT3_JUMPANDLINKREGISTER_JALR) {
                {
                    uint8_t rdnum = state->instruction.itype.rd;
                    void *rd = &state->reg.x[rdnum];
                    uint8_t rs1num = state->instruction.itype.rs1;
                    void *rs1 = &state->reg.x[rs1num];
                    int16_t imm = state->instruction.itype.imm;
                    uint32_t jumptoprogramcounter = (*(uint32_t *)rs1 + imm) & (UINT32_MAX - 1);
                    if (rdnum != 0) *(uint32_t *)rd = state->programcounternext;
                    state->programcounternext = jumptoprogramcounter;
                }
            } else {
                state->trapflag.illegalinstruction = 1;
            }
        } break;
        case OPCODE32_OPERATION: {
            uint8_t rdnum = state->instruction.rtype.rd;
            void *rd = &state->reg.x[rdnum];
            uint8_t rs1num = state->instruction.rtype.rs1;
            void *rs1 = &state->reg.x[rs1num];
            uint8_t rs2num = state->instruction.rtype.rs2;
            void *rs2 = &state->reg.x[rs2num];
            RiscvInstructionTypeRDecoderFunct7Funct3_u decoder = {0};
            decoder.funct3 = state->instruction.rtype.funct3;
            decoder.funct7 = state->instruction.rtype.funct7;

            switch (decoder.funct7_3) {
            case FUNCT7_FUNCT3_OPERATION_ADD:
                if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rs1 + *(int32_t *)rs2;
                break;
            case FUNCT7_FUNCT3_OPERATION_SUB:
                if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rs1 - *(int32_t *)rs2;
                break;
            case FUNCT7_FUNCT3_OPERATION_SLL:
                if ((uint8_t)rdnum != 0)
                    *(uint32_t *)rd = *(uint32_t *)rs1 << (*(uint32_t *)rs2 & 0b11111);
                break;
            case FUNCT7_FUNCT3_OPERATION_SLT:
                if ((uint8_t)rdnum != 0) *(int32_t *)rd = (*(int32_t *)rs1 < *(int32_t *)rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_SLTU:
                if ((uint8_t)rdnum != 0) *(uint32_t *)rd = (*(uint32_t *)rs1 < *(uint32_t *)rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_XOR:
                if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)rs1 ^ *(uint32_t *)rs2;
                break;
            case FUNCT7_FUNCT3_OPERATION_SRL:
                if ((uint8_t)rdnum != 0)
                    *(uint32_t *)rd = *(uint32_t *)rs1 >> (*(uint32_t *)rs2 & 0b11111);
                break;
            case FUNCT7_FUNCT3_OPERATION_SRA:
                if ((uint8_t)rdnum != 0)
                    *(int32_t *)rd = *(int32_t *)rs1 >> (*(uint32_t *)rs2 & 0b11111);
                break;
            case FUNCT7_FUNCT3_OPERATION_OR:
                if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rs1 | *(int32_t *)rs2;
                break;
            case FUNCT7_FUNCT3_OPERATION_AND:
                if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rs1 & *(int32_t *)rs2;
                break;
            case FUNCT7_FUNCT3_OPERATION_MUL:
                if ((uint8_t)rdnum != 0) *(uint32_t *)rd = (*(uint32_t *)rs1 * *(uint32_t *)rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_MULH:
                if ((uint8_t)rdnum != 0) {
                    int64_t result = (int64_t)(*(int32_t *)rs1 * (int64_t) * (int32_t *)rs2);
                    *(int32_t *)rd = (result >> 32);
                }
                break;
            case FUNCT7_FUNCT3_OPERATION_MULHSU:
                if ((uint8_t)rdnum != 0) {
                    int64_t result = (int64_t)(*(int32_t *)rs1 * (uint64_t) * (uint32_t *)rs2);
                    *(int32_t *)rd = (result >> 32);
                }
                break;
            case FUNCT7_FUNCT3_OPERATION_MULHU:
                if ((uint8_t)rdnum != 0) {
                    uint64_t result = (uint64_t)(*(uint32_t *)rs1 * (uint64_t) * (uint32_t *)rs2);
                    *(uint32_t *)rd = (result >> 32);
                }
                break;
            case FUNCT7_FUNCT3_OPERATION_DIV:
                if ((uint8_t)rdnum != 0) {
                    if (*(int32_t *)rs2 == 0) *(int32_t *)rd = -1;
                    else if (*(int32_t *)rs1 == INT32_MIN && *(int32_t *)rs2 == -1)
                        *(int32_t *)rd = INT32_MIN;
                    else *(int32_t *)rd = (*(int32_t *)rs1 / *(int32_t *)rs2);
                }
                break;
            case FUNCT7_FUNCT3_OPERATION_DIVU:
                if ((uint8_t)rdnum != 0) {
                    if (*(uint32_t *)rs2 == 0) *(uint32_t *)rd = UINT32_MAX;
                    else *(uint32_t *)rd = (*(uint32_t *)rs1 / *(uint32_t *)rs2);
                }
                break;
            case FUNCT7_FUNCT3_OPERATION_REM:
                if ((uint8_t)rdnum != 0) {
                    if (*(int32_t *)rs2 == 0) *(int32_t *)rd = *(int32_t *)rs1;
                    else if (*(int32_t *)rs1 == INT32_MIN && *(int32_t *)rs2 == -1)
                        *(int32_t *)rd = 0;
                    else *(int32_t *)rd = (*(int32_t *)rs1 % *(int32_t *)rs2);
                }
                break;
            case FUNCT7_FUNCT3_OPERATION_REMU:
                if ((uint8_t)rdnum != 0) {
                    if (*(uint32_t *)rs2 == 0) *(uint32_t *)rd = *(uint32_t *)rs1;
                    else *(uint32_t *)rd = (*(uint32_t *)rs1 % *(uint32_t *)rs2);
                }
                break;
            default: state->trapflag.illegalinstruction = 1; break;
            }
        } break;
        case OPCODE32_IMMEDIATE: {
            uint8_t rdnum = state->instruction.itype.rd;
            void *rd = &state->reg.x[rdnum];
            uint8_t rs1num = state->instruction.itype.rs1;
            void *rs1 = &state->reg.x[rs1num];
            uint8_t funct3 = state->instruction.itype.funct3;

            if (funct3 == FUNCT3_IMMEDIATE_FUNCTIONS_1 || funct3 == FUNCT3_IMMEDIATE_FUNCTIONS_5) {
                uint8_t shamt = state->instruction.itypeshiftbyconstant.shamt;
                RiscvInstructionTypeIDecoderImm11_7Funct3Imm11_7Funct3_u decoder = {0};
                decoder.funct3 = funct3;
                decoder.imm11_5 = state->instruction.itypeshiftbyconstant.imm11_5;

                switch (decoder.imm11_5funct3) {
                case IMM11_5_FUNCT3_IMMEDIATE_SLLI:
                    if ((uint8_t)rdnum != 0)
                        *(uint32_t *)rd = *(uint32_t *)rs1 << ((uint8_t)shamt & 0b11111);
                    goto __inl_6;
                case IMM11_5_FUNCT3_IMMEDIATE_SRLI:
                    if ((uint8_t)rdnum != 0)
                        *(uint32_t *)rd = *(uint32_t *)rs1 >> ((uint8_t)shamt & 0b11111);
                    goto __inl_6;
                case IMM11_5_FUNCT3_IMMEDIATE_SRAI:
                    if ((uint8_t)rdnum != 0)
                        *(int32_t *)rd = *(int32_t *)rs1 >> ((uint8_t)shamt & 0b11111);
                    goto __inl_6;
                }
            }

            int16_t imm = state->instruction.itype.imm;
            switch (funct3) {
            case FUNCT3_IMMEDIATE_ADDI:
                if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rs1 + (int16_t)imm;
                break;
            case FUNCT3_IMMEDIATE_SLTI:
                if ((uint8_t)rdnum != 0) *(int32_t *)rd = (*(int32_t *)rs1 < (int16_t)imm);
                break;
            case FUNCT3_IMMEDIATE_SLTIU:
                if ((uint8_t)rdnum != 0) *(uint32_t *)rd = (*(uint32_t *)rs1 < (uint32_t)imm);
                break;
            case FUNCT3_IMMEDIATE_XORI:
                if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)rs1 ^ (uint32_t)imm;
                break;
            case FUNCT3_IMMEDIATE_ORI:
                if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rs1 | (int16_t)imm;
                break;
            case FUNCT3_IMMEDIATE_ANDI:
                if ((uint8_t)rdnum != 0) *(int32_t *)rd = *(int32_t *)rs1 & (int16_t)imm;
                break;
            default: state->trapflag.illegalinstruction = 1; break;
            }

        __inl_6:;
        } break;
        case OPCODE32_LOAD: {
            uint8_t rdnum = state->instruction.itype.rd;
            void *rd = &state->reg.x[rdnum];
            uint8_t rs1num = state->instruction.stype.rs1;
            void *rs1 = &state->reg.x[rs1num];
            int16_t imm = state->instruction.itype.imm;
            uint32_t memorylocation = imm + *(uint32_t *)rs1;
            uint8_t length = 0;
            switch (state->instruction.itype.funct3) {
            case FUNCT3_LOAD_LB:
            case FUNCT3_LOAD_LBU: length = sizeof(uint8_t); break;
            case FUNCT3_LOAD_LH:
            case FUNCT3_LOAD_LHU: length = sizeof(uint16_t); break;
            case FUNCT3_LOAD_LW: length = sizeof(uint32_t); break;
            default: state->trapflag.illegalinstruction = 1; goto __inl_7;
            }

            if (length > 1) {
                uint8_t memorylocation8 = memorylocation & 0xFF;
                if ((memorylocation8 % length) != 0) {
                    state->trapflag.loadaddressmisaligned = 1;
                    state->csr.mtval = memorylocation;
                }
            }

            if (rdnum == 0) goto __inl_7;

            if (state->trapflag.loadaddressmisaligned == 1) goto __inl_7;

            uint32_t value = 0;
            RiscvEmulatorLoad(memorylocation, &value, length);

            switch (state->instruction.itype.funct3) {
            case FUNCT3_LOAD_LB: *(int32_t *)rd = (int8_t)value; break;
            case FUNCT3_LOAD_LBU: *(uint32_t *)rd = (uint8_t)value; break;
            case FUNCT3_LOAD_LH: *(int32_t *)rd = (int16_t)value; break;
            case FUNCT3_LOAD_LHU: *(uint32_t *)rd = (uint16_t)value; break;
            case FUNCT3_LOAD_LW: *(uint32_t *)rd = (uint32_t)value; break;
            }

        __inl_7:;
        } break;
        case OPCODE32_STORE: {
            RiscvInstructionTypeSDecoderImm_u immdecoder = {0};
            immdecoder.bit.imm4_0 = state->instruction.stype.imm4_0;
            immdecoder.bit.imm11_5 = state->instruction.stype.imm11_5;
            int16_t offset = immdecoder.imm;
            uint8_t rs1num = state->instruction.stype.rs1;
            void *rs1 = &state->reg.x[rs1num];
            uint8_t rs2num = state->instruction.stype.rs2;
            void *rs2 = &state->reg.x[rs2num];
            uint32_t memorylocation = offset + *(uint32_t *)rs1;
            uint8_t length = 0;
            switch (state->instruction.stype.funct3) {
            case FUNCT3_STORE_SW: length = sizeof(uint32_t); break;
            case FUNCT3_STORE_SH: length = sizeof(uint16_t); break;
            case FUNCT3_STORE_SB: length = sizeof(uint8_t); break;
            default: state->trapflag.illegalinstruction = 1; goto __inl_8;
            }

            if (length > 1) {
                uint8_t memorylocation8 = memorylocation & 0xFF;
                if ((memorylocation8 % length) != 0) {
                    state->trapflag.storeaddressmisaligned = 1;
                    state->csr.mtval = memorylocation;
                }
            }

            if (state->trapflag.storeaddressmisaligned == 1) goto __inl_8;

            RiscvEmulatorStore(memorylocation, rs2, length);

        __inl_8:;
        } break;
        case OPCODE32_BRANCH: {
            uint8_t rs1num = state->instruction.btype.rs1;
            void *rs1 = &state->reg.x[rs1num];
            uint8_t rs2num = state->instruction.btype.rs2;
            void *rs2 = &state->reg.x[rs2num];
            RiscvInstructionTypeBDecoderImm_u immdecoder = {0};
            immdecoder.bit.imm4_1 = state->instruction.btype.imm4_1;
            immdecoder.bit.imm10_5 = state->instruction.btype.imm10_5;
            immdecoder.bit.imm11 = state->instruction.btype.imm11;
            immdecoder.bit.imm12 = state->instruction.btype.imm12;
            int16_t imm = immdecoder.imm;
            uint8_t executebranch = BRANCH_NO;
            switch (state->instruction.btype.funct3) {
            case FUNCT3_BRANCH_BEQ:
                if (*(int32_t *)rs1 == *(int32_t *)rs2) executebranch = BRANCH_YES;
                break;
            case FUNCT3_BRANCH_BNE:
                if (*(int32_t *)rs1 != *(int32_t *)rs2) executebranch = BRANCH_YES;
                break;
            case FUNCT3_BRANCH_BGE:
                if (*(int32_t *)rs1 >= *(int32_t *)rs2) executebranch = BRANCH_YES;
                break;
            case FUNCT3_BRANCH_BGEU:
                if (*(uint32_t *)rs1 >= *(uint32_t *)rs2) executebranch = BRANCH_YES;
                break;
            case FUNCT3_BRANCH_BLT:
                if (*(int32_t *)rs1 < *(int32_t *)rs2) executebranch = BRANCH_YES;
                break;
            case FUNCT3_BRANCH_BLTU:
                if (*(uint32_t *)rs1 < *(uint32_t *)rs2) executebranch = BRANCH_YES;
                break;
            default: state->trapflag.illegalinstruction = 1; goto __inl_9;
            }

            if (executebranch == BRANCH_YES)
                state->programcounternext = state->programcounter + imm;

        __inl_9:;
        } break;
        case OPCODE32_ADDUPPERIMMEDIATE2PC: {
            RiscvInstructionTypeUDecoderImm_u immdecoder = {0};
            immdecoder.bit.imm31_12 = state->instruction.utype.imm31_12;
            int32_t imm = immdecoder.imm;
            uint8_t rdnum = state->instruction.utype.rd;
            if (rdnum != 0) state->reg.x[rdnum] = state->programcounter + imm;
        } break;
        case OPCODE32_LOADUPPERIMMEDIATE: {
            RiscvInstructionTypeUDecoderImm_u immdecoder = {0};
            immdecoder.bit.imm11_0 = 0;
            immdecoder.bit.imm31_12 = state->instruction.utype.imm31_12;
            uint8_t rdnum = state->instruction.utype.rd;
            if (rdnum != 0) state->reg.x[rdnum] = immdecoder.imm;
        } break;
        case OPCODE32_JUMPANDLINK: {
            uint8_t rdnum = state->instruction.jtype.rd;
            void *rd = &state->reg.x[rdnum];
            RiscvInstructionTypeJDecoderImm_u immdecoder = {0};
            immdecoder.bit.imm10_1 = state->instruction.jtype.imm10_1;
            immdecoder.bit.imm11 = state->instruction.jtype.imm11;
            immdecoder.bit.imm19_12 = state->instruction.jtype.imm19_12;
            immdecoder.bit.imm20 = state->instruction.jtype.imm20;
            uint32_t jumptoprogramcounter = state->programcounter + immdecoder.imm;
            if (rdnum != 0) *(uint32_t *)rd = state->programcounternext;
            state->programcounternext = jumptoprogramcounter;
        } break;
        case OPCODE32_SYSTEM: {
            if (state->instruction.itypesystem.rd == 0 &&
                state->instruction.itypesystem.funct3 == 0 &&
                state->instruction.itypesystem.rs1 == 0) {
                switch (state->instruction.itypesystem.funct12) {
                case FUNCT12_MRET: {
                    state->csr.mstatush.mpv = 0;
                    state->csr.mstatus.mpp = 0;
                    state->csr.mstatus.mie = state->csr.mstatus.mpie;
                    state->csr.mstatus.mpie = 1;
                    state->programcounternext = state->csr.mepc;
                }
                    goto __inl_10;
                case FUNCT12_ECALL: {
                    state->trapflag.environmentcallfrommmode = 1;
                    RiscvEmulatorHandleECALL(state);
                }
                    goto __inl_10;
                case FUNCT12_EBREAK: {
                    state->trapflag.breakpoint = 1;
                    state->csr.mtval = state->programcounter;
                    RiscvEmulatorHandleEBREAK(state);
                }
                    goto __inl_10;
                }
            }

            uint8_t rdnum = state->instruction.itypecsr.rd;
            void *rd = &state->reg.x[rdnum];
            uint8_t rs1num = state->instruction.itypecsr.rs1;
            void *rs1 = &state->reg.x[rs1num];
            uint8_t imm = state->instruction.itypecsrimm.imm;
            uint16_t csrnum = state->instruction.itypecsr.csr;
            void *csr = RiscvEmulatorGetCSRAddress(state, csrnum);

            if (state->trapflag.value > 0) goto __inl_10;

            switch (state->instruction.itypecsr.funct3) {
            case FUNCT3_CSR_CSRRW: {
                uint32_t originalvaluers1 = *(uint32_t *)rs1;
                if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)csr;
                *(uint32_t *)csr = originalvaluers1;
            } break;
            case FUNCT3_CSR_CSRRWI: {
                if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)csr;
                *(uint32_t *)csr = (uint8_t)imm;
            } break;
            case FUNCT3_CSR_CSRRS: {
                int32_t initialrs1value = *(uint32_t *)rs1;
                if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)csr;
                if (initialrs1value != 0) *(uint32_t *)csr |= initialrs1value;
            } break;
            case FUNCT3_CSR_CSRRSI: {
                if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)csr;
                if ((uint8_t)imm != 0) *(uint32_t *)csr |= (uint8_t)imm;
            } break;
            case FUNCT3_CSR_CSRRC: {
                int32_t initialrs1value = *(uint32_t *)rs1;
                if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)csr;
                if (initialrs1value != 0) *(uint32_t *)csr &= ~initialrs1value;
            } break;
            case FUNCT3_CSR_CSRRCI: {
                if ((uint8_t)rdnum != 0) *(uint32_t *)rd = *(uint32_t *)csr;
                if ((uint8_t)imm != 0) *(uint32_t *)csr &= ~(uint8_t)imm;
            } break;
            default: state->trapflag.illegalinstruction = 1; break;
            }

        __inl_10:;
        } break;
        case OPCODE32_MISCMEM: {
            uint8_t rd = state->instruction.itypemiscmem.rd;
            uint8_t funct3 = state->instruction.itypemiscmem.funct3;
            uint8_t rs1 = state->instruction.itypemiscmem.rs1;

            if (rd == 0 && rs1 == 0 && (funct3 == FUNCT3_FENCE || funct3 == FUNCT3_FENCEI))
                goto __inl_11;

            state->trapflag.illegalinstruction = 1;

        __inl_11:;
        } break;
        default: state->trapflag.illegalinstruction = 1; break;
        }
    }

    if (state->trapflag.value > 0) {
        {
            // Instruction address misaligned
            if (state->trapflag.instructionaddressmisaligned == 1) {
                state->csr.mcause.exceptioncode =
                    MCAUSE_EXCEPTION_CODE_INSTRUCTION_ADDRESS_MISALIGNED;
            }

            // Breakpoint
            if (state->trapflag.breakpoint == 1)
                state->csr.mcause.exceptioncode = MCAUSE_EXCEPTION_CODE_BREAKPOINT;

            // Load address misaligned
            if (state->trapflag.loadaddressmisaligned == 1)
                state->csr.mcause.exceptioncode = MCAUSE_EXCEPTION_CODE_LOAD_ADDRESS_MISALIGNED;

            // Store/AMO address misaligned
            if (state->trapflag.storeaddressmisaligned == 1)
                state->csr.mcause.exceptioncode = MCAUSE_EXCEPTION_CODE_STORE_ADDRESS_MISALIGNED;

            //  Environment call from M-mode
            if (state->trapflag.environmentcallfrommmode == 1)
                state->csr.mcause.exceptioncode = MCAUSE_EXCEPTION_CODE_ENVIRONMENT_CALL_FROM_MMODE;

            // Illegal instruction
            if (state->trapflag.illegalinstruction == 1) {
                state->csr.mcause.exceptioncode = MCAUSE_EXCEPTION_CODE_ILLEGAL_INSTRUCTION;
                state->csr.mtval = state->instruction.value;
            }

            state->csr.mstatus.mpp = 3; // Previous privilege mode: M
            state->csr.mstatus.mpie = state->csr.mstatus.mie;
            state->csr.mstatus.mie = 0;
            state->csr.mepc = state->programcounter;

            // Jump to trap handler.
            state->programcounternext = state->csr.mtvec.base << 2;
            // For mode 1, add some offset based on exceptioncode.
            if (state->csr.mtvec.mode == 1)
                state->programcounternext = 4 * state->csr.mcause.exceptioncode;

            if (state->trapflag.illegalinstruction == 1) RiscvEmulatorIllegalInstruction(state);

            state->trapflag.value = 0;
        }
    }
}

/* ------------------------------------------------------------ public API glue */

RiscvEmulatorState_t *RiscvEmulatorCreate(uint32_t initial_sp) {
    RiscvEmulatorState_t *state = calloc(1, sizeof *state);
    if (state) RiscvEmulatorInit(state, initial_sp);
    return state;
}
void RiscvEmulatorDestroy(RiscvEmulatorState_t *state) {
    free(state);
}

uint32_t RiscvEmulatorGetRegister(const RiscvEmulatorState_t *state, int index) {
    return state->reg.x[index & 31];
}
void RiscvEmulatorSetRegister(RiscvEmulatorState_t *state, int index, uint32_t value) {
    index &= 31;
    if (index) state->reg.x[index] = value; /* x0 stays hard-wired zero */
}
uint32_t RiscvEmulatorGetProgramCounter(const RiscvEmulatorState_t *state) {
    return state->programcounter;
}
uint32_t RiscvEmulatorGetNextProgramCounter(const RiscvEmulatorState_t *state) {
    return state->programcounternext;
}
void RiscvEmulatorSetProgramCounter(RiscvEmulatorState_t *state, uint32_t pc) {
    state->programcounter = pc;
    state->programcounternext = pc;
}
uint32_t RiscvEmulatorGetInstruction(const RiscvEmulatorState_t *state) {
    return state->instruction.value;
}
uint16_t RiscvEmulatorGetCsrNumber(const RiscvEmulatorState_t *state) {
    return state->instruction.itypecsr.csr;
}
uint32_t RiscvEmulatorGetTrapVectorBase(const RiscvEmulatorState_t *state) {
    return state->csr.mtvec.base;
}
void RiscvEmulatorRaiseIllegalInstruction(RiscvEmulatorState_t *state) {
    state->trapflag.illegalinstruction = 1;
}
void RiscvEmulatorClearTrap(RiscvEmulatorState_t *state) {
    state->trapflag.value = 0;
}
