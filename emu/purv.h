/*
 * purv.h - RISC-V (RV32IMC + Zicsr/Zifencei) emulator: public interface.
 *
 * Amalgamated from atoomnetmarc/RISC-V-emulator (Apache-2.0, (c) Marc Ketel),
 * pinned commit 633526d4. Types + config + prototypes; the bodies live in
 * purv.c. The flags below are baked into both files (disabled-extension and
 * RVE_E_HOOK code is stripped), so redefining them has no effect.
 */
#ifndef PURV_H_
#define PURV_H_

#include <stdint.h>
#include <string.h>

#define RVE_E_M        1
#define RVE_E_C        1
#define RVE_E_ZICSR    1
#define RVE_E_ZIFENCEI 1
#define RVE_E_A        0
#define RVE_E_B        0
#define RVE_E_ZBA      0
#define RVE_E_ZBB      0
#define RVE_E_ZBC      0
#define RVE_E_ZBS      0
#define RVE_E_HOOK     0

/* Architectural memory-map origins (resolved for RV32IMC). */
#define IALIGN      16
#define IO_ORIGIN   0x02000000
#define UART_ORIGIN 0x10000000
#define ROM_ORIGIN  0x20000000
#define RAM_ORIGIN  0x80000000

/* ===== RiscvEmulatorType.h ===== */

/* ===== RiscvEmulatorTypeEmulator.h ===== */

/* ===== RiscvEmulatorTypeCSR.h ===== */

/* ===== RiscvEmulatorTypeCSRMachineInformationRegister.h ===== */

/**
 * Hart ID Register.
 */
typedef struct __attribute__((packed)) {
    uint32_t hartid : 32;
} RiscvCSRmhartid_t;

/* ===== RiscvEmulatorTypeCSRMachineMemoryProtection.h ===== */

/**
 * Physical memory protection configuration.
 */
typedef struct __attribute__((packed)) {
    uint8_t pmp0cfg;
    uint8_t pmp1cfg;
    uint8_t pmp2cfg;
    uint8_t pmp3cfg;
} RiscvCSRpmpcfg0_t;

/**
 * Physical memory protection address register.
 */
typedef struct __attribute__((packed)) {
    uint32_t address;
} RiscvCSRpmpaddr0_t;

/* ===== RiscvEmulatorTypeCSRMachineNonMaskableInterruptHandling.h ===== */

/**
 * Resumable NMI status.
 */
typedef struct __attribute__((packed)) {
    uint8_t : 3;
    uint8_t nmie : 1;
    uint8_t : 3;
    uint8_t mnpv : 1;
    uint8_t : 3;
    uint8_t mnpp : 2;
    uint32_t : 19;
} RiscvCSRmnstatus_t;

/* ===== RiscvEmulatorTypeCSRMachineTrapHandling.h ===== */

/**
 * Scratch register for machine trap handlers.
 */
typedef struct __attribute__((packed)) {
    uint32_t mscratch;
} RiscvCSRmscratch_t;

/**
 *  Machine trap cause.
 */
typedef struct __attribute__((packed)) {
    uint32_t exceptioncode : 31;
    uint8_t interrupt : 1;
} RiscvCSRmcause_t;

/**
 *   Machine interrupt pending.
 */
typedef struct __attribute__((packed)) {
    uint32_t mip;
} RiscvCSRmip_t;

/* ===== RiscvEmulatorTypeCSRMachineTrapSetup.h ===== */

/**
 * Machine status register.
 */
typedef struct __attribute__((packed)) {
    uint8_t : 1;

    /**
     * S-mode Interrupt Enable.
     */
    uint8_t sie : 1;

    uint8_t : 1;

    /**
     * M-mode Interrupt Enable.
     */
    uint8_t mie : 1;

    uint8_t : 1;

    /**
     * S-mode Previous Interrupt Enable.
     *
     * The SPIE bit indicates whether supervisor interrupts were enabled prior to trapping into supervisor mode.
     */
    uint8_t spie : 1;

    /**
     * U-mode Big-Endian Enable.
     */
    uint8_t ube : 1;

    /**
     * M-mode Previous Interrupt Enable.
     */
    uint8_t mpie : 1;

    /**
     * S-mode Previous Privilege.
     */
    uint8_t spp : 1;

    /**
     * Status of the vector extension.
     */
    uint8_t vs : 2;

    /**
     * M-mode Previous Privilege.
     */
    uint8_t mpp : 2;

    /**
     * Status of the floating-point unit.
     */
    uint8_t fs : 2;

    /**
     * Status of additional user-mode extensions and associated state.
     */
    uint8_t xs : 2;

    /**
     * Modify PRiVilege.
     */
    uint8_t mprv : 1;

    /**
     * Permit Supervisor User Memory access.
     */
    uint8_t sum : 1;

    /**
     * Make eXecutable Readable.
     */
    uint8_t mxr : 1;

    /**
     * Trap Virtual Memory.
     */
    uint8_t tvm : 1;

    /**
     * Timeout Wait.
     */
    uint8_t tw : 1;

    /**
     * Trap SRET.
     */
    uint8_t tsr : 1;

    uint8_t : 8;

    /**
     * FS, VS, or XS bits encode a Dirty state.
     */
    uint8_t sd : 1;
} RiscvCSRmstatus_t;

/**
 * Additional machine-mode status register.
 */
typedef struct __attribute__((packed)) {
    uint8_t : 4;

    /**
     * S-mode Big-Endian Enable.
     */
    uint8_t sbe : 1;

    /**
     * M-mode Big-Endian Enable.
     */
    uint8_t mbe : 1;

    /**
     * Guest Virtual Address.
     */
    uint8_t gva : 1;

    /**
     * Machine Previous Virtualization Mode.
     */
    uint8_t mpv : 1;

    uint32_t : 24;
} RiscvCSRmstatush_t;

/**
 * Machine ISA Register with all the extensions.
 */
typedef union {
    struct __attribute__((packed)) {
        uint32_t extensions : 26;
        uint8_t mxlen : 4;
        uint8_t mxl : 2;
    };

    struct __attribute__((packed)) {
        /**
         * Atomic extension.
         */
        uint8_t a : 1;

        /**
         * B extension.
         */
        uint8_t b : 1;

        /**
         * Compressed extension.
         */
        uint8_t c : 1;

        /**
         * Double-precision floating-point extension.
         */
        uint8_t d : 1;

        /**
         * RV32E base ISA.
         */
        uint8_t e : 1;

        /**
         * Single-precision floating-point extension.
         */
        uint8_t f : 1;

        /**
         * Reserved.
         */
        uint8_t g : 1;

        /**
         * Hypervisor extension.
         */
        uint8_t h : 1;

        /**
         * RV32I/64I/128I base ISA.
         */
        uint8_t i : 1;

        /**
         * Reserved.
         */
        uint8_t j : 1;

        /**
         * Reserved.
         */
        uint8_t k : 1;

        /**
         * Reserved.
         */
        uint8_t l : 1;

        /**
         * Integer Multiply/Divide extension.
         */
        uint8_t m : 1;

        /**
         * Tentatively reserved for User-Level Interrupts extension.
         */
        uint8_t n : 1;

        /**
         * Reserved.
         */
        uint8_t o : 1;

        /**
         * Tentatively reserved for Packed-SIMD extension.
         */
        uint8_t p : 1;

        /**
         * Quad-precision floating-point extension.
         */
        uint8_t q : 1;

        /**
         * Reserved.
         */
        uint8_t r : 1;

        /**
         * Supervisor mode implemented.
         */
        uint8_t s : 1;

        /**
         * Reserved.
         */
        uint8_t t : 1;

        /**
         * User mode implemented.
         */
        uint8_t u : 1;

        /**
         *  Vector extension.
         */
        uint8_t v : 1;

        /**
         * Reserved.
         */
        uint8_t w : 1;

        /**
         * Non-standard extensions present.
         */
        uint8_t x : 1;

        /**
         * Reserved.
         */
        uint8_t y : 1;

        /**
         * Reserved.
         */
        uint8_t z : 1;
    };

} RiscvCSRmisa_u;

/**
 * Machine Exception Delegation Register.
 */
typedef struct __attribute__((packed)) {
    uint32_t synchronousexceptions;
} RiscvCSRmedeleg_t;

/**
 * Machine Interrupt Delegation Register.
 */
typedef struct __attribute__((packed)) {
    uint32_t interrupts;
} RiscvCSRmideleg_t;

/**
 * Machine interrupt-enable register.
 */
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

/**
 * Machine Trap-Vector Base-Address Register.
 */
typedef struct __attribute__((packed)) {
    uint8_t mode : 2;
    uint32_t base : 30;
} RiscvCSRmtvec_t;

/* ===== RiscvEmulatorTypeCSRSupervisorProtectionAndTranslation.h ===== */

/**
 * Supervisor Address Translation and Protection (satp) Register.
 */
typedef struct __attribute__((packed)) {
    /**
     * physical page number.
     */
    uint32_t ppn : 22;

    /**
     * address space identifier.
     */
    uint16_t asid : 9;

    /**
     * current address-translation scheme.
     */
    uint8_t mode : 1;

} RiscvCSRsatp_t;

/**
 * Collection of control and status registers.
 */
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

/* ===== RiscvEmulatorTypeInstruction.h ===== */

/* ===== RiscvEmulatorTypeB.h ===== */

/**
 * B-type instruction.
 */
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

/**
 * Union for decoding imm field of a B-type instruction.
 */
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

/* ===== RiscvEmulatorTypeC.h ===== */

/**
 * Compressed Register instruction format.
 *
 * Valid for jr, mv, ebreak, jalr and add.
 */
typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rs2 : 5;
    uint8_t rd : 5;
    uint8_t funct4 : 4;
} RiscvInstructionTypeCR_t;

/**
 * Compressed Immediate instruction format.
 *
 * Valid for addi, li and slli.
 */
typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t imm4_0 : 5;
    uint8_t rd : 5;
    uint8_t imm5 : 1;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCI_t;

/**
 * Compressed Immediate instruction format.
 *
 * Valid for addi16sp.
 */
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

/**
 * Compressed Immediate instruction format.
 *
 * Valid for lui.
 */
typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t imm16_12 : 5;
    uint8_t rd : 5;
    uint8_t imm17 : 1;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCILui_t;

/**
 * Compressed Immediate instruction format.
 *
 * Valid for lwsp and flwsp.
 */
typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t imm7_6 : 2;
    uint8_t imm4_2 : 3;
    uint8_t rd : 5;
    uint8_t imm5 : 1;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCILwsp_t;

/**
 * Compressed Stack-relative Store instruction format.
 *
 * Valid for fsdsp.
 */
typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rs2 : 5;
    uint8_t imm8_6 : 3;
    uint8_t imm5_3 : 3;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCSSFsdsp_t;

/**
 * Compressed Stack-relative Store instruction format.
 *
 * Valid for swsp and fswsp.
 */
typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rs2 : 5;
    uint8_t imm7_6 : 2;
    uint8_t imm5_2 : 4;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCSS_t;

/**
 * Compressed Wide Immediate instruction format.
 *
 * Valid for addi4spn.
 */
typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rdp : 3;
    uint8_t imm3 : 1;
    uint8_t imm2 : 1;
    uint8_t imm9_6 : 4;
    uint8_t imm5_4 : 2;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCIW_t;

/**
 * Compressed Load instruction format.
 *
 * Valid for lw, flw.
 */
typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rdp : 3;
    uint8_t imm6 : 1;
    uint8_t imm2 : 1;
    uint8_t rs1p : 3;
    uint8_t imm5_3 : 3;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCL_t;

/**
 * Compressed Store instruction format.
 *
 * Valid for sw, fsw.
 */
typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rs2p : 3;
    uint8_t imm6 : 1;
    uint8_t imm2 : 1;
    uint8_t rs1p : 3;
    uint8_t imm5_3 : 3;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCS_t;

/**
 * Compressed Arithmetic instruction format.
 *
 * Valid for sub, xor, or, and.
 */
typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t rs2p : 3;
    uint8_t funct2 : 2;
    uint8_t rdp : 3;
    uint8_t funct6 : 6;
} RiscvInstructionTypeCA_t;

/**
 * Compressed Branch instruction format.
 *
 * Valid for beqz, bnez.
 */
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

/**
 * Compressed Branch instruction format.
 *
 * Valid for srli, srai, andi.
 */
typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint8_t imm4_0 : 5;
    uint8_t rdp : 3;
    uint8_t funct2 : 2;
    uint8_t imm5 : 1;
    uint8_t funct3 : 3;
} RiscvInstructionTypeCBImm_t;

/**
 * Compressed Jump instruction format.
 *
 * Valid for jal, j.
 */
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

/**
 * Union for decoding imm of TypeCI.
 */
typedef union {
    struct __attribute__((packed)) {
        uint8_t imm4_0 : 5;
        uint8_t imm5 : 1;
    } bit;

    struct __attribute__((packed)) {
        int32_t imm : 6;
    };
} RiscvInstructionTypeCIDecoderImm_u;

/**
 * Union for decoding imm of addi16sp.
 */
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

/**
 * Union for decoding imm of lui.
 */
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

/**
 * Union for decoding imm of lwsp and flwsp.
 */
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

/**
 * Union for decoding imm of TypeCSS.
 */
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

/**
 * Union for decoding imm of TypeCIW.
 */
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

/**
 * Union for decoding imm of TypeCL.
 */
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

/**
 * Union for decoding imm of TypeCS.
 */
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

/**
 * Union for decoding funct6_funct2 of TypeCA.
 */
typedef union {
    struct __attribute__((packed)) {
        uint8_t funct2 : 2;
        uint8_t funct6 : 6;
    };

    struct __attribute__((packed)) {
        uint8_t funct6_funct2 : 8;
    };
} RiscvInstructionTypeCADecoderFunct6Funct2_u;

/**
 * Union for decoding imm of TypeCB.
 */
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

/**
 * Union for decoding imm of TypeCB.
 */
typedef union {
    struct __attribute__((packed)) {
        uint8_t imm4_0 : 5;
        uint8_t imm5 : 1;
    } bit;

    struct __attribute__((packed)) {
        int16_t imm : 6;
    };
} RiscvInstructionTypeCBImmDecoderImm_u;

/**
 * Union for decoding funct3_funct2 of TypeCB.
 */
typedef union {
    struct __attribute__((packed)) {
        uint8_t funct2 : 2;
        uint8_t funct3 : 3;
    };

    struct __attribute__((packed)) {
        uint8_t funct3_funct2 : 5;
    };
} RiscvInstructionTypeCBDecoderFunct3Funct2_u;

/**
 * Union for decoding imm of TypeCJ.
 */
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

/**
 * Easier access to the opcode of an 16-bit instruction when you do not know the instruction yet.
 */
typedef struct __attribute__((packed)) {
    uint8_t op : 2;
    uint16_t : 11;
    uint8_t funct3 : 3;
} RiscvInstructionOpcodeC_t;

/**
 * Union for decoding opcode of a compressed instruction.
 */
typedef union {
    struct __attribute__((packed)) {
        uint8_t op : 2;
        uint8_t funct3 : 3;
    };

    struct __attribute__((packed)) {
        uint8_t opfunct3 : 5;
    };
} RiscvInstructionTypeCDecoderOpcode_u;

/* ===== RiscvEmulatorTypeI.h ===== */

/**
 * I-type instruction.
 */
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
    uint8_t custom5 : 5;
    uint8_t funct3 : 3;
    uint16_t custom11 : 11;
    uint16_t funct6 : 6;
} RiscvInstructionTypeIStystemCustom_t;

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

/**
 * Union for combining imm11_5 and funct3 field of R-type instruction.
 */
typedef union {
    struct __attribute__((packed)) {
        uint8_t funct3 : 3;
        int16_t imm11_5 : 7;
    };

    struct __attribute__((packed)) {
        uint16_t imm11_5funct3 : 10;
    };
} RiscvInstructionTypeIDecoderImm11_7Funct3Imm11_7Funct3_u;

/* ===== RiscvEmulatorTypeJ.h ===== */

/**
 * J-type instruction.
 */
typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t rd : 5;
    uint8_t imm19_12 : 8;
    uint8_t imm11 : 1;
    uint16_t imm10_1 : 10;
    uint8_t imm20 : 1;
} RiscvInstructionTypeJ_t;

/**
 * Union for decoding imm field of a J-type instruction.
 */
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

/* ===== RiscvEmulatorTypeR.h ===== */

/**
 * R-type instruction.
 */
typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t rd : 5;
    uint8_t funct3 : 3;
    uint8_t rs1 : 5;
    uint8_t rs2 : 5;
    uint8_t funct7 : 7;
} RiscvInstructionTypeR_t;

/**
 * Union for combining funct3 and funct7 field of R-type instruction.
 */
typedef union {
    struct __attribute__((packed)) {
        uint8_t funct3 : 3;
        uint8_t funct7 : 7;
    };

    struct __attribute__((packed)) {
        uint16_t funct7_3 : 10;
    };
} RiscvInstructionTypeRDecoderFunct7Funct3_u;

/* ===== RiscvEmulatorTypeS.h ===== */

/**
 * S-type instruction.
 */
typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t imm4_0 : 5;
    uint8_t funct3 : 3;
    uint8_t rs1 : 5;
    uint8_t rs2 : 5;
    uint8_t imm11_5 : 7;
} RiscvInstructionTypeS_t;

/**
 * Union for decoding imm field of a S-type instruction.
 */
typedef union {
    struct __attribute__((packed)) {
        uint8_t imm4_0 : 5;
        uint8_t imm11_5 : 7;
    } bit;

    struct __attribute__((packed)) {
        int16_t imm : 12;
    };
} RiscvInstructionTypeSDecoderImm_u;

/* ===== RiscvEmulatorTypeU.h ===== */

/**
 * U-type instruction.
 */
typedef struct __attribute__((packed)) {
    uint8_t opcode : 7;
    uint8_t rd : 5;
    uint32_t imm31_12 : 20;
} RiscvInstructionTypeU_t;

/**
 * Union for decoding imm field of a U-type instruction.
 */
typedef union {
    struct __attribute__((packed)) {
        uint16_t imm11_0 : 12;
        uint32_t imm31_12 : 20;
    } bit;

    struct __attribute__((packed)) {
        uint32_t imm : 32;
    };
} RiscvInstructionTypeUDecoderImm_u;

/**
 * All the instruction types combined.
 */
typedef union {
    uint32_t value;

    /**
     * Easy access to the opcode of an 32-bit instruction when you do not know the instruction type yet.
     */
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
    RiscvInstructionTypeCSSFsdsp_t cssfsdsp;
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
    RiscvInstructionTypeIStystemCustom_t itypesystemcustom;
    RiscvInstructionTypeS_t stype;
    RiscvInstructionTypeB_t btype;
    RiscvInstructionTypeU_t utype;
    RiscvInstructionTypeJ_t jtype;
} RiscvInstruction_u;

/* ===== RiscvEmulatorTypeRegister.h ===== */

/**
 * Union of all the ways a register can be accessed.
 */
typedef union {
    /**
     * Symbolic registers.
     */
    struct {
        /**
         * Always zero.
         */
        uint32_t Zero;

        /**
         * Return address.
         */
        uint32_t ra;

        /**
         * Stack pointer.
         */
        uint32_t sp;

        /**
         * Global pointer.
         */
        uint32_t gp;

        /**
         * Thread pointer.
         */
        uint32_t tp;

        /**
         * Temporary or alternate link register.
         */
        uint32_t t0;

        /**
         * Temporary register.
         */
        uint32_t t1;

        /**
         * Temporary register.
         */
        uint32_t t2;

        /**
         * Saved register or frame pointer.
         */
        uint32_t s0_fp;

        /**
         * Saved register.
         */
        uint32_t s1;

        /**
         * Function argument or return value.
         */
        uint32_t a0;

        /**
         * Function argument or return value.
         */
        uint32_t a1;

        /**
         * Function argument.
         */
        uint32_t a2;

        /**
         * Function argument.
         */
        uint32_t a3;

        /**
         * Function argument.
         */
        uint32_t a4;

        /**
         * Function argument.
         */
        uint32_t a5;

        /**
         * Function argument.
         */
        uint32_t a6;

        /**
         * Function argument.
         */
        uint32_t a7;

        /**
         * Saved register.
         */
        uint32_t s2;

        /**
         * Saved register.
         */
        uint32_t s3;

        /**
         * Saved register.
         */
        uint32_t s4;

        /**
         * Saved register.
         */
        uint32_t s5;

        /**
         * Saved register.
         */
        uint32_t s6;

        /**
         * Saved register.
         */
        uint32_t s7;

        /**
         * Saved register.
         */
        uint32_t s8;

        /**
         * Saved register.
         */
        uint32_t s9;

        /**
         * Saved register.
         */
        uint32_t s10;

        /**
         * Saved register.
         */
        uint32_t s11;

        /**
         * Temporary register.
         */
        uint32_t t3;

        /**
         * Temporary register.
         */
        uint32_t t4;

        /**
         * Temporary register.
         */
        uint32_t t5;

        /**
         * Temporary register.
         */
        uint32_t t6;
    };

    /**
     * Named registers.
     */
    uint32_t x[32];
} RiscvRegister_u;

/**
 * Riscv emulator state flags.
 */
typedef union {
    struct
    {
        uint8_t illegalinstruction : 1;

        uint8_t instructionaddressmisaligned : 1;
        uint8_t breakpoint : 1;
        uint8_t loadaddressmisaligned : 1;
        uint8_t storeaddressmisaligned : 1;
        uint8_t environmentcallfrommmode : 1;
    };

    uint8_t value;
} RiscvEmulatorTrapFlag_u;

/**
 * Riscv emulator state.
 */
typedef struct
{
    RiscvEmulatorTrapFlag_u trapflag;
    uint32_t programcounter;
    uint32_t programcounternext;
    RiscvInstruction_u instruction;
    RiscvRegister_u reg;

    RiscvCSR_t csr;
} RiscvEmulatorState_t;

/* ===== RiscvEmulatorTypeHook.h ===== */

/**
 * Generic hook function context.
 */
typedef struct {
    uint8_t hook;
    const char *instruction;
    uint8_t rs1num;
    const void *rs1;
    uint8_t rs2num;
    const void *rs2;
    uint8_t rdnum;
    const void *rd;
    uint16_t csrnum;
    const void *csr;
    char *immname;
    uint8_t immlength;
    uint8_t immissigned;
    uint32_t imm;
    uint32_t upperimmediate;
    uint32_t memorylocation;
    uint8_t length;
} RiscvEmulatorHookContext_t;

/* ---- Implementation-specific hooks ----------------------------------------
 * Define these in your host (see main.c). This is atoom's whole "API": the
 * engine reaches your memory map and trap policy only through these calls.
 */
void RiscvEmulatorLoad(uint32_t address, void *destination, uint8_t length);
void RiscvEmulatorStore(uint32_t address, const void *source, uint8_t length);
void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *state);
void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *state);
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *state);
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *state);

/* ---- Public API ---- */
void RiscvEmulatorInit(RiscvEmulatorState_t *state, uint32_t ram_length);
void RiscvEmulatorLoop(RiscvEmulatorState_t *state);

#endif /* PURV_H_ */
