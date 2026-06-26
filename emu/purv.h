/*
 * purv.h - single-header RISC-V (RV32IMC + Zicsr/Zifencei) emulator core.
 *
 * Amalgamated from atoomnetmarc/RISC-V-emulator (Apache-2.0, (c) Marc Ketel),
 * pinned commit 633526d4, with the ISA flags baked in and dead branches
 * stripped. Define the implementation-specific hooks (see purv.c) then call
 * RiscvEmulatorInit() / RiscvEmulatorLoop().
 */
#ifndef PURV_H_
#define PURV_H_

#include <stdint.h>
#include <string.h>

/* ---- Baked build configuration: RV32IMC + Zicsr + Zifencei ----------------
 * These flags are compiled in. Disabled-extension code (A, B/Zb*, the generic
 * RVE_E_HOOK instrumentation) has been stripped from this amalgamation, so
 * redefining them has no effect. To change the ISA, regenerate from upstream.
 */
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

/* The engine passes a hook-context pointer to many helpers; with RVE_E_HOOK
 * baked off it is unused. Scope the warning away for the vendored engine only
 * (mirrors upstream's own pragma around RiscvEmulatorHook). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
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
 * Define these in your program (see purv.c). This is atoom's whole "API": the
 * engine reaches your memory map and trap policy only through these calls.
 */
void RiscvEmulatorLoad(uint32_t address, void *destination, uint8_t length);
void RiscvEmulatorStore(uint32_t address, const void *source, uint8_t length);
void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *state);
void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *state);
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *state);
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *state);

/* ===== RiscvEmulatorDefine.h ===== */

#define IALIGN 16

#define IO_ORIGIN   0x02000000
#define UART_ORIGIN 0x10000000
#define ROM_ORIGIN  0x20000000
#define RAM_ORIGIN  0x80000000

/* ===== RiscvEmulatorDefineBType.h ===== */

// B-type, branch.

#define FUNCT3_BRANCH_BEQ  0b000
#define FUNCT3_BRANCH_BNE  0b001
#define FUNCT3_BRANCH_BLT  0b100
#define FUNCT3_BRANCH_BGE  0b101
#define FUNCT3_BRANCH_BLTU 0b110
#define FUNCT3_BRANCH_BGEU 0b111

#define BRANCH_YES 0
#define BRANCH_NO  1

/* ===== RiscvEmulatorDefineCSRMachineTrapHandling.h ===== */

// Machine cause exception code.

#define MCAUSE_EXCEPTION_CODE_INSTRUCTION_ADDRESS_MISALIGNED 0
#define MCAUSE_EXCEPTION_CODE_INSTRUCTION_ACCESS_FAULT       1
#define MCAUSE_EXCEPTION_CODE_ILLEGAL_INSTRUCTION            2
#define MCAUSE_EXCEPTION_CODE_BREAKPOINT                     3
#define MCAUSE_EXCEPTION_CODE_LOAD_ADDRESS_MISALIGNED        4
#define MCAUSE_EXCEPTION_CODE_LOAD_ACCESS_FAULT              5
#define MCAUSE_EXCEPTION_CODE_STORE_ADDRESS_MISALIGNED       6
#define MCAUSE_EXCEPTION_CODE_STORE_ACCESS_FAULT             7
#define MCAUSE_EXCEPTION_CODE_ENVIRONMENT_CALL_FROM_UMODE    8
#define MCAUSE_EXCEPTION_CODE_ENVIRONMENT_CALL_FROM_SMODE    9
#define MCAUSE_EXCEPTION_CODE_ENVIRONMENT_CALL_FROM_MMODE    11
#define MCAUSE_EXCEPTION_CODE_INSTRUCTION_PAGE_FAULT         12
#define MCAUSE_EXCEPTION_CODE_LOAD_PAGE_FAULT                13
#define MCAUSE_EXCEPTION_CODE_STORE_PAGE_FAULT               15
#define MCAUSE_EXCEPTION_CODE_DOUBLE_TRAP                    16
#define MCAUSE_EXCEPTION_CODE_SOFTWARE_CHECK                 18
#define MCAUSE_EXCEPTION_CODE_HARDWARE_ERROR                 19

/* ===== RiscvEmulatorDefineCType.h ===== */

// Compressed Register.

#define FUNCT4_MV  0b1000
#define FUNCT4_ADD 0b1001

// Compressed Arithmetic.

#define FUNCT6_FUNCT2_SUB 0b10001100
#define FUNCT6_FUNCT2_XOR 0b10001101
#define FUNCT6_FUNCT2_OR  0b10001110
#define FUNCT6_FUNCT2_AND 0b10001111

// Compressed Branch.

#define FUNCT3_FUNCT2_SRLI 0b10000
#define FUNCT3_FUNCT2_SRAI 0b10001
#define FUNCT3_FUNCT2_ANDI 0b10010

/* ===== RiscvEmulatorDefineHook.h ===== */

#define HOOK_UNKNOWN 0
#define HOOK_BEGIN   1
#define HOOK_END     2

/* ===== RiscvEmulatorDefineIType.h ===== */

// I-type, register immediate.

#define FUNCT3_IMMEDIATE_ADDI  0b000
#define FUNCT3_IMMEDIATE_SLTI  0b010
#define FUNCT3_IMMEDIATE_SLTIU 0b011
#define FUNCT3_IMMEDIATE_XORI  0b100
#define FUNCT3_IMMEDIATE_ORI   0b110
#define FUNCT3_IMMEDIATE_ANDI  0b111

#define FUNCT3_IMMEDIATE_FUNCTIONS_1 0b001
#define FUNCT3_IMMEDIATE_FUNCTIONS_5 0b101

#define IMM11_5_FUNCT3_IMMEDIATE_SLLI 0b0000000001
#define IMM11_5_FUNCT3_IMMEDIATE_SRLI 0b0000000101
#define IMM11_5_FUNCT3_IMMEDIATE_SRAI 0b0100000101

#define FUNCT3_JUMPANDLINKREGISTER_JALR 0b000

#define FUNCT3_LOAD_LB  0b000
#define FUNCT3_LOAD_LH  0b001
#define FUNCT3_LOAD_LW  0b010
#define FUNCT3_LOAD_LBU 0b100
#define FUNCT3_LOAD_LHU 0b101

#define FUNCT3_CSR_CSRRW  0b001
#define FUNCT3_CSR_CSRRS  0b010
#define FUNCT3_CSR_CSRRC  0b011
#define FUNCT3_CSR_CSRRWI 0b101
#define FUNCT3_CSR_CSRRSI 0b110
#define FUNCT3_CSR_CSRRCI 0b111

#define FUNCT12_ECALL  0b000000000000
#define FUNCT12_EBREAK 0b000000000001
#define FUNCT12_URET   0b000000000010
#define FUNCT12_SRET   0b000100000010
#define FUNCT12_MRET   0b001100000010
#define FUNCT12_MNRET  0b011100000010
#define FUNCT12_WFI    0b000100000101

#define FUNCT3_FENCE 0b000

#define FUNCT3_FENCEI 0b001
/* ===== RiscvEmulatorDefineOpcode.h ===== */

#define OPCODE16_QUADRANT_0       0b00
#define OPCODE16_QUADRANT_1       0b01
#define OPCODE16_QUADRANT_2       0b10
#define OPCODE16_QUADRANT_INVALID 0b11

// 16-bit opcodes when RV32. Bits [15:13][1:0].

#define OPCODE16_ADDI4SPN     0b00000
#define OPCODE16_FLD          0b00100
#define OPCODE16_LW           0b01000
#define OPCODE16_FLW          0b01100
#define OPCODE16_FSD          0b10100
#define OPCODE16_SW           0b11000
#define OPCODE16_FSW          0b11100
#define OPCODE16_ADDI         0b00001
#define OPCODE16_JAL          0b00101
#define OPCODE16_LI           0b01001
#define OPCODE16_LUI_ADDI16SP 0b01101
#define OPCODE16_MISCALU      0b10001
#define OPCODE16_J            0b10101
#define OPCODE16_BEQZ         0b11001
#define OPCODE16_BNEZ         0b11101
#define OPCODE16_SLLI         0b00010
#define OPCODE16_FLDSP        0b00110
#define OPCODE16_LWSP         0b01010
#define OPCODE16_FLWSP        0b01110
#define OPCODE16_JALR_MV_ADD  0b10010
#define OPCODE16_FSDSP        0b10110
#define OPCODE16_SWSP         0b11010
#define OPCODE16_FSWSP        0b11110

// 32-bit opcodes. Bits [6:0].

#define OPCODE32_LOAD                  0b0000011
#define OPCODE32_LOADFP                0b0000111
#define OPCODE32_CUSTOM0               0b0001011
#define OPCODE32_MISCMEM               0b0001111
#define OPCODE32_IMMEDIATE             0b0010011
#define OPCODE32_ADDUPPERIMMEDIATE2PC  0b0010111
#define OPCODE32_STORE                 0b0100011
#define OPCODE32_STOREFP               0b0100111
#define OPCODE32_CUSTOM1               0b0101011
#define OPCODE32_ATOMICMEMORYOPERATION 0b0101111
#define OPCODE32_OPERATION             0b0110011
#define OPCODE32_LOADUPPERIMMEDIATE    0b0110111
#define OPCODE32_OPERATIONFPADD        0b1000011
#define OPCODE32_OPERATIONFPSUB        0b1000111
#define OPCODE32_OPERATIONFPFNMSUB     0b1001011
#define OPCODE32_OPERATIONFPFNMADD     0b1001111
#define OPCODE32_OPERATIONFP           0b1010011
#define OPCODE32_OPERATIONVECTOR       0b1010111
#define OPCODE32_CUSTOM2               0b1011011
#define OPCODE32_BRANCH                0b1100011
#define OPCODE32_JUMPANDLINKREGISTER   0b1100111
#define OPCODE32_RESERVED_6B           0b1101011
#define OPCODE32_JUMPANDLINK           0b1101111
#define OPCODE32_SYSTEM                0b1110011
#define OPCODE32_PACKED_SIMD           0b1110111
#define OPCODE32_CUSTOM3               0b1111011
#define OPCODE32_ILLEGAL               0b1111111

/* ===== RiscvEmulatorDefineRType.h ===== */

// R-type, register register.

#define FUNCT7_FUNCT3_OPERATION_ADD  0b0000000000
#define FUNCT7_FUNCT3_OPERATION_SUB  0b0100000000
#define FUNCT7_FUNCT3_OPERATION_SLL  0b0000000001
#define FUNCT7_FUNCT3_OPERATION_SLT  0b0000000010
#define FUNCT7_FUNCT3_OPERATION_SLTU 0b0000000011
#define FUNCT7_FUNCT3_OPERATION_XOR  0b0000000100
#define FUNCT7_FUNCT3_OPERATION_SRL  0b0000000101
#define FUNCT7_FUNCT3_OPERATION_SRA  0b0100000101
#define FUNCT7_FUNCT3_OPERATION_OR   0b0000000110
#define FUNCT7_FUNCT3_OPERATION_AND  0b0000000111

#define FUNCT7_FUNCT3_OPERATION_MUL    0b0000001000
#define FUNCT7_FUNCT3_OPERATION_MULH   0b0000001001
#define FUNCT7_FUNCT3_OPERATION_MULHSU 0b0000001010
#define FUNCT7_FUNCT3_OPERATION_MULHU  0b0000001011
#define FUNCT7_FUNCT3_OPERATION_DIV    0b0000001100
#define FUNCT7_FUNCT3_OPERATION_DIVU   0b0000001101
#define FUNCT7_FUNCT3_OPERATION_REM    0b0000001110
#define FUNCT7_FUNCT3_OPERATION_REMU   0b0000001111

/* ===== RiscvEmulatorDefineSType.h ===== */

// S-type, store.

#define FUNCT3_STORE_SB 0b000
#define FUNCT3_STORE_SH 0b001
#define FUNCT3_STORE_SW 0b010

/* ===== RiscvEmulatorHook.h ===== */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

/**
 * Generic hook function.
 */
__attribute__((weak)) void RiscvEmulatorHook(
    const RiscvEmulatorState_t *state,
    const RiscvEmulatorHookContext_t *context) {
}

#pragma GCC diagnostic pop

/* ===== RiscvEmulatorExtension.h ===== */

/* ===== RiscvEmulatorExtensionA.h ===== */

/* ===== RiscvEmulatorExtensionC.h ===== */

/**
 * rd = (*sp + nzuimm)
 */
static inline void RiscvEmulatorC_ADDI4SPN(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    void *sp,
    const uint16_t nzuimm) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)sp + nzuimm;

}

/**
 * Load word from memory to rd.
 */
static inline void RiscvEmulatorC_LW(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    void *rs1,
    const uint8_t offset) {

    uint8_t length = sizeof(uint32_t);

    uint32_t memorylocation = *(int32_t *)rs1 + offset;

    if (state->trapflag.storeaddressmisaligned == 1) {
        return;
    }

    RiscvEmulatorLoad(memorylocation, rd, length);

}

/**
 * Store word in rs2 to memory.
 */
static inline void RiscvEmulatorC_SW(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rs1num __attribute__((unused)),
    void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    void *rs2,
    const uint8_t offset) {

    uint8_t length = sizeof(uint32_t);

    uint32_t memorylocation = *(int32_t *)rs1 + offset;

    if (state->trapflag.storeaddressmisaligned == 1) {
        return;
    }

    RiscvEmulatorStore(memorylocation, rs2, length);

}

/**
 * rd = rd + nzimm
 */
static inline void RiscvEmulatorC_ADDI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const int8_t nzimm) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rd + nzimm;

}

/**
 * pc += offset
 */
static inline void RiscvEmulatorC_JAL(
    RiscvEmulatorState_t *state __attribute__((unused)),
    void *ra,
    const int16_t offset) {

    *(uint32_t *)ra = state->programcounter + 2;
    state->programcounternext = state->programcounter + offset;

}

/**
 * pc += rs1
 */
static inline void RiscvEmulatorC_JALR(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rs1num __attribute__((unused)),
    void *rs1,
    void *ra) {

    uint32_t originalvaluers1 = *(int32_t *)rs1;

    *(uint32_t *)ra = state->programcounter + 2;
    state->programcounternext = (originalvaluers1 & (UINT32_MAX - 1));

}

/**
 * pc += offset
 */
static inline void RiscvEmulatorC_J(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const int16_t offset) {

    state->programcounternext = state->programcounter + offset;

}

/**
 * pc += rs1
 */
static inline void RiscvEmulatorC_JR(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rs1num __attribute__((unused)),
    void *rs1) {

    state->programcounternext = *(int32_t *)rs1 & (UINT32_MAX - 1);

}

/**
 * Branch if rs1 == 0
 */
static inline void RiscvEmulatorC_BEQZ(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rs1num __attribute__((unused)),
    void *rs1,
    const int16_t imm) {

    if (*(int32_t *)rs1 == 0) {
        state->programcounternext = state->programcounter + imm;
    };

}

/**
 * Branch if rs1 != 0
 */
static inline void RiscvEmulatorC_BNEZ(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rs1num __attribute__((unused)),
    void *rs1,
    const int16_t imm) {

    if (*(int32_t *)rs1 != 0) {
        state->programcounternext = state->programcounter + imm;
    };

}

/**
 * Logical shift left: rd = rs1 << shamt
 */
static inline void RiscvEmulatorC_SLLI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t shamt) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = *(uint32_t *)rd << shamt;

}

/**
 * rd = imm
 */
static inline void RiscvEmulatorC_LI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const int8_t imm) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = imm;

}

/**
 * sp += imm*16
 */
static inline void RiscvEmulatorC_ADDI16SP(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd) {

    RiscvInstructionTypeCIAddi16spDecoderImm_u immdecoder = {0};
    immdecoder.bit.imm4 = state->instruction.ciaddi16sp.imm4;
    immdecoder.bit.imm5 = state->instruction.ciaddi16sp.imm5;
    immdecoder.bit.imm6 = state->instruction.ciaddi16sp.imm6;
    immdecoder.bit.imm8_7 = state->instruction.ciaddi16sp.imm8_7;
    immdecoder.bit.imm9 = state->instruction.ciaddi16sp.imm9;

    int16_t nzimm = immdecoder.imm;

    if (nzimm == 0) {
        return;
    }

    *(int32_t *)rd += nzimm;

}

/**
 * Load upper with immediate.
 */
static inline void RiscvEmulatorC_LUI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd) {

    RiscvInstructionTypeCILuiDecoderImm_u immdecoder = {0};
    immdecoder.bit.imm16_12 = state->instruction.cilui.imm16_12;
    immdecoder.bit.imm17 = state->instruction.cilui.imm17;
    int32_t nzimm = immdecoder.imm;

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = nzimm;

}

/**
 * Logical shift right: rd = rd >> shamt
 */
static inline void RiscvEmulatorC_SRLI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    uint8_t shamt) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = *(uint32_t *)rd >> shamt;

}

/**
 * Arithmetic shift right: rd = rs1 >> shamt
 */
static inline void RiscvEmulatorC_SRAI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    uint8_t shamt) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rd >> shamt;

}

/**
 * rd = rd & imm.
 */
static inline void RiscvEmulatorC_ANDI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    int8_t imm) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rd & imm;

}

/**
 * rd = rd - rs2.
 */
static inline void RiscvEmulatorC_SUB(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs2num __attribute__((unused)),
    void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rd - *(int32_t *)rs2;

}

/**
 * Exclusive or: rd = rd ^ rs2
 */
static inline void RiscvEmulatorC_XOR(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs2num __attribute__((unused)),
    void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = *(uint32_t *)rd ^ *(uint32_t *)rs2;

}

/**
 * Exclusive or: rd = rd | rs2
 */
static inline void RiscvEmulatorC_OR(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs2num __attribute__((unused)),
    void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = *(uint32_t *)rd | *(uint32_t *)rs2;

}

/**
 * Exclusive or: rd = rd & rs2
 */
static inline void RiscvEmulatorC_AND(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs2num __attribute__((unused)),
    void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = *(uint32_t *)rd & *(uint32_t *)rs2;

}

/**
 * Load memorylocation (*sp + offset) into rd.
 */
static inline void RiscvEmulatorC_LWSP(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    void *sp,
    const uint8_t offset) {

    uint32_t memorylocation = *(int32_t *)sp + offset;

    if (rdnum == 0) {
        return;
    }

    RiscvEmulatorLoad(memorylocation, rd, sizeof(uint32_t));

}

/**
 * rd = rs2
 */
static inline void RiscvEmulatorC_MV(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs2num __attribute__((unused)),
    void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rs2;

}

/**
 * Cause control to be transferred back to a debugging environment.
 */
static inline void RiscvEmulatorC_EBREAK(RiscvEmulatorState_t *state) {

    state->trapflag.breakpoint = 1;
    state->csr.mtval = state->programcounter;

    RiscvEmulatorHandleEBREAK(state);
}

/**
 * rd = rd + rs2
 */
static inline void RiscvEmulatorC_ADD(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs2num __attribute__((unused)),
    void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rd + *(int32_t *)rs2;

}

/**
 * Store rs2 to memorylocation (*sp + offset)
 */
static inline void RiscvEmulatorC_SWSP(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rs2num __attribute__((unused)),
    void *rs2,
    void *sp,
    const uint8_t offset) {

    uint32_t memorylocation = *(int32_t *)sp + offset;

    RiscvEmulatorStore(memorylocation, rs2, sizeof(uint32_t));

}

/**
 * Process compressed opcodes.
 */
static inline void RiscvEmulatorOpcodeCompressed(RiscvEmulatorState_t *state) {
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

    // Whenever possible, combine decoding instruction bits for multiple instructions.
    switch (opfunct3) {
        case OPCODE16_ADDI4SPN: {
            RiscvInstructionTypeCIWDecoderImm_u RiscvInstructionTypeCIWDecoderImm = {0};
            RiscvInstructionTypeCIWDecoderImm.bit.imm2 = state->instruction.ciwtype.imm2;
            RiscvInstructionTypeCIWDecoderImm.bit.imm3 = state->instruction.ciwtype.imm3;
            RiscvInstructionTypeCIWDecoderImm.bit.imm5_4 = state->instruction.ciwtype.imm5_4;
            RiscvInstructionTypeCIWDecoderImm.bit.imm9_6 = state->instruction.ciwtype.imm9_6;
            imm = RiscvInstructionTypeCIWDecoderImm.imm;
            rdnum = state->instruction.ciwtype.rdp + 8;
            break;
        }
        case OPCODE16_MISCALU: {
            RiscvInstructionTypeCBDecoderFunct3Funct2_u RiscvInstructionTypeCBDecoderFunct3Funct2 = {0};
            RiscvInstructionTypeCBDecoderFunct3Funct2.funct3 = state->instruction.cbimm.funct3;
            RiscvInstructionTypeCBDecoderFunct3Funct2.funct2 = state->instruction.cbimm.funct2;
            funct3_funct2 = RiscvInstructionTypeCBDecoderFunct3Funct2.funct3_funct2;

            RiscvInstructionTypeCBImmDecoderImm_u RiscvInstructionTypeCBImmDecoderImm = {0};

            if (funct3_funct2 == FUNCT3_FUNCT2_SRLI ||
                funct3_funct2 == FUNCT3_FUNCT2_SRAI ||
                funct3_funct2 == FUNCT3_FUNCT2_ANDI) {
                RiscvInstructionTypeCBImmDecoderImm.bit.imm4_0 = state->instruction.cbimm.imm4_0;
                RiscvInstructionTypeCBImmDecoderImm.bit.imm5 = state->instruction.cbimm.imm5;
                imm = RiscvInstructionTypeCBImmDecoderImm.imm;

                rdnum = state->instruction.cbimm.rdp + 8;
                break;
            }

            RiscvInstructionTypeCADecoderFunct6Funct2_u RiscvInstructionTypeCADecoderFunct6Funct2 = {0};
            RiscvInstructionTypeCADecoderFunct6Funct2.funct6 = state->instruction.catype.funct6;
            RiscvInstructionTypeCADecoderFunct6Funct2.funct2 = state->instruction.catype.funct2;
            funct6_funct2 = RiscvInstructionTypeCADecoderFunct6Funct2.funct6_funct2;

            if (funct6_funct2 == FUNCT6_FUNCT2_SUB ||
                funct6_funct2 == FUNCT6_FUNCT2_XOR ||
                funct6_funct2 == FUNCT6_FUNCT2_OR ||
                funct6_funct2 == FUNCT6_FUNCT2_AND) {
                rdnum = state->instruction.catype.rdp + 8;
                rs2num = state->instruction.catype.rs2p + 8;
                break;
            }
            break;
        }
        case OPCODE16_BEQZ:
        case OPCODE16_BNEZ: {
            RiscvInstructionTypeCBDecoderImm_u RiscvInstructionTypeCBDecoderImm = {0};
            RiscvInstructionTypeCBDecoderImm.bit.imm2_1 = state->instruction.cbtype.imm2_1;
            RiscvInstructionTypeCBDecoderImm.bit.imm4_3 = state->instruction.cbtype.imm4_3;
            RiscvInstructionTypeCBDecoderImm.bit.imm5 = state->instruction.cbtype.imm5;
            RiscvInstructionTypeCBDecoderImm.bit.imm7_6 = state->instruction.cbtype.imm7_6;
            RiscvInstructionTypeCBDecoderImm.bit.imm8 = state->instruction.cbtype.imm8;
            imm = RiscvInstructionTypeCBDecoderImm.imm;
            rs1num = state->instruction.cbtype.rs1p + 8;
            break;
        }
        case OPCODE16_ADDI:
        case OPCODE16_LI:
        case OPCODE16_SLLI: {
            RiscvInstructionTypeCIDecoderImm_u RiscvInstructionTypeCIDecoderImm = {0};
            RiscvInstructionTypeCIDecoderImm.bit.imm4_0 = state->instruction.citype.imm4_0;
            RiscvInstructionTypeCIDecoderImm.bit.imm5 = state->instruction.citype.imm5;
            imm = RiscvInstructionTypeCIDecoderImm.imm;
            rdnum = state->instruction.citype.rd;
            break;
        }
        case OPCODE16_JAL:
        case OPCODE16_J: {
            RiscvInstructionTypeCJDecoderImm_u RiscvInstructionTypeCJDecoderImm = {0};
            RiscvInstructionTypeCJDecoderImm.bit.imm3_1 = state->instruction.cjtype.imm3_1;
            RiscvInstructionTypeCJDecoderImm.bit.imm4 = state->instruction.cjtype.imm4;
            RiscvInstructionTypeCJDecoderImm.bit.imm5 = state->instruction.cjtype.imm5;
            RiscvInstructionTypeCJDecoderImm.bit.imm6 = state->instruction.cjtype.imm6;
            RiscvInstructionTypeCJDecoderImm.bit.imm7 = state->instruction.cjtype.imm7;
            RiscvInstructionTypeCJDecoderImm.bit.imm9_8 = state->instruction.cjtype.imm9_8;
            RiscvInstructionTypeCJDecoderImm.bit.imm10 = state->instruction.cjtype.imm10;
            RiscvInstructionTypeCJDecoderImm.bit.imm11 = state->instruction.cjtype.imm11;
            imm = RiscvInstructionTypeCJDecoderImm.imm;
            break;
        }
        case OPCODE16_LW: {
            RiscvInstructionTypeCLDecoderImm_u RiscvInstructionTypeCLDecoderImm = {0};
            RiscvInstructionTypeCLDecoderImm.bit.imm2 = state->instruction.cltype.imm2;
            RiscvInstructionTypeCLDecoderImm.bit.imm5_3 = state->instruction.cltype.imm5_3;
            RiscvInstructionTypeCLDecoderImm.bit.imm6 = state->instruction.cltype.imm6;
            imm = RiscvInstructionTypeCLDecoderImm.imm;
            rs1num = state->instruction.cltype.rs1p + 8;
            rdnum = state->instruction.cltype.rdp + 8;
            break;
        }
        case OPCODE16_SW: {
            RiscvInstructionTypeCSDecoderImm_u RiscvInstructionTypeCSDecoderImm = {0};
            RiscvInstructionTypeCSDecoderImm.bit.imm2 = state->instruction.cstype.imm2;
            RiscvInstructionTypeCSDecoderImm.bit.imm5_3 = state->instruction.cstype.imm5_3;
            RiscvInstructionTypeCSDecoderImm.bit.imm6 = state->instruction.cstype.imm6;
            imm = RiscvInstructionTypeCSDecoderImm.imm;
            rs1num = state->instruction.cstype.rs1p + 8;
            rs2num = state->instruction.cstype.rs2p + 8;
            break;
        }
        case OPCODE16_LWSP: {
            RiscvInstructionTypeCILwspDecoderImm_u RiscvInstructionTypeCILwspDecoderImm = {0};
            RiscvInstructionTypeCILwspDecoderImm.bit.imm4_2 = state->instruction.cilwsp.imm4_2;
            RiscvInstructionTypeCILwspDecoderImm.bit.imm5 = state->instruction.cilwsp.imm5;
            RiscvInstructionTypeCILwspDecoderImm.bit.imm7_6 = state->instruction.cilwsp.imm7_6;
            imm = RiscvInstructionTypeCILwspDecoderImm.imm;
            rdnum = state->instruction.cilwsp.rd;
            break;
        }
        case OPCODE16_SWSP: {
            RiscvInstructionTypeCSSDecoderImm_u RiscvInstructionTypeCSSDecoderImm = {0};
            RiscvInstructionTypeCSSDecoderImm.bit.imm5_2 = state->instruction.csstype.imm5_2;
            RiscvInstructionTypeCSSDecoderImm.bit.imm7_6 = state->instruction.csstype.imm7_6;
            imm = RiscvInstructionTypeCSSDecoderImm.imm;
            rs2num = state->instruction.csstype.rs2;
            break;
        }
    }

    if (rdnum >= 0) {
        rd = &state->reg.x[rdnum];
    }
    if (rs1num >= 0) {
        rs1 = &state->reg.x[rs1num];
    }
    if (rs2num >= 0) {
        rs2 = &state->reg.x[rs2num];
    }

    switch (opfunct3) {
        case OPCODE16_ADDI4SPN:
            if (imm != 0) {
                RiscvEmulatorC_ADDI4SPN(state, rdnum, rd, sp, imm);
            } else {
                state->trapflag.illegalinstruction = 1;
            }
            break;
        case OPCODE16_LW:
            RiscvEmulatorC_LW(state, rdnum, rd, rs1num, rs1, imm);
            break;
        case OPCODE16_SW:
            RiscvEmulatorC_SW(state, rs1num, rs1, rs2num, rs2, imm);
            break;
        case OPCODE16_ADDI:
            RiscvEmulatorC_ADDI(state, rdnum, rd, imm);
            break;
        case OPCODE16_JAL:
            RiscvEmulatorC_JAL(state, ra, imm);
            break;
        case OPCODE16_LI:
            RiscvEmulatorC_LI(state, rdnum, rd, imm);
            break;
        case OPCODE16_LUI_ADDI16SP: {
            rdnum = state->instruction.cilui.rd;
            rd = &state->reg.x[rdnum];

            if (rdnum == 2) {
                RiscvEmulatorC_ADDI16SP(state, rdnum, rd);
            } else {
                RiscvEmulatorC_LUI(state, rdnum, rd);
            }
            break;
        }
        case OPCODE16_MISCALU:
            if (funct3_funct2 == FUNCT3_FUNCT2_SRLI) {
                RiscvEmulatorC_SRLI(state, rdnum, rd, imm);
                break;
            }

            if (funct3_funct2 == FUNCT3_FUNCT2_SRAI) {
                RiscvEmulatorC_SRAI(state, rdnum, rd, imm);
                break;
            }

            if (funct3_funct2 == FUNCT3_FUNCT2_ANDI) {
                RiscvEmulatorC_ANDI(state, rdnum, rd, imm);
                break;
            }

            if (funct6_funct2 == FUNCT6_FUNCT2_SUB) {
                RiscvEmulatorC_SUB(state, rdnum, rd, rs2num, rs2);
                break;
            }

            if (funct6_funct2 == FUNCT6_FUNCT2_XOR) {
                RiscvEmulatorC_XOR(state, rdnum, rd, rs2num, rs2);
                break;
            }

            if (funct6_funct2 == FUNCT6_FUNCT2_OR) {
                RiscvEmulatorC_OR(state, rdnum, rd, rs2num, rs2);
                break;
            }

            if (funct6_funct2 == FUNCT6_FUNCT2_AND) {
                RiscvEmulatorC_AND(state, rdnum, rd, rs2num, rs2);
                break;
            }

            state->trapflag.illegalinstruction = 1;
            break;
        case OPCODE16_J:
            RiscvEmulatorC_J(state, imm);
            break;
        case OPCODE16_BEQZ:
            RiscvEmulatorC_BEQZ(state, rs1num, rs1, imm);
            break;
        case OPCODE16_BNEZ:
            RiscvEmulatorC_BNEZ(state, rs1num, rs1, imm);
            break;
        case OPCODE16_SLLI:
            RiscvEmulatorC_SLLI(state, rdnum, rd, imm);
            break;
        case OPCODE16_LWSP:
            RiscvEmulatorC_LWSP(state, rdnum, rd, sp, imm);
            break;
        case OPCODE16_JALR_MV_ADD:
            rdnum = state->instruction.crtype.rd;
            rd = &state->reg.x[rdnum];
            rs2num = state->instruction.crtype.rs2;
            rs2 = &state->reg.x[rs2num];

            if (state->instruction.crtype.funct4 == FUNCT4_MV) {
                if (rs2num == 0) {
                    RiscvEmulatorC_JR(state, rdnum, rd);
                } else {
                    RiscvEmulatorC_MV(state, rdnum, rd, rs2num, rs2);
                }
            } else /* FUNCT4_ADD */
            {
                if (rdnum == 0 &&
                    rs2num == 0) {
                    RiscvEmulatorC_EBREAK(state);
                } else if (rs2num == 0) {
                    RiscvEmulatorC_JALR(state, rdnum, rd, ra);
                } else {
                    RiscvEmulatorC_ADD(state, rdnum, rd, rs2num, rs2);
                }
            }
            break;
        case OPCODE16_SWSP:
            RiscvEmulatorC_SWSP(state, rs2num, rs2, sp, imm);
            break;
        default:
            state->trapflag.illegalinstruction = 1;
            break;
    }
}

/* ===== RiscvEmulatorExtensionI.h ===== */

/* ===== RiscvEmulatorExtensionM.h ===== */

/**
 * Multiply signed or unsigned.
 */
static inline void RiscvEmulatorMUL(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = (*(uint32_t *)rs1 * *(uint32_t *)rs2);

}

/**
 * Multiply signed, return 32 bit MSB of resulting 64-bit value.
 */
static inline void RiscvEmulatorMULH(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    int64_t result = (int64_t)(*(int32_t *)rs1 * (int64_t)*(int32_t *)rs2);
    *(int32_t *)rd = (result >> 32);

}

/**
 * Multiply signed rs1 and unsigned rs2, return 32 bit MSB of resulting unsigned 64-bit value.
 */
static inline void RiscvEmulatorMULHSU(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    int64_t result = (int64_t)(*(int32_t *)rs1 * (uint64_t)*(uint32_t *)rs2);
    *(int32_t *)rd = (result >> 32);

}

/**
 * Multiply unsigned, return 32 bit MSB of resulting unsigned 64-bit value.
 */
static inline void RiscvEmulatorMULHU(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    uint64_t result = (uint64_t)(*(uint32_t *)rs1 * (uint64_t)*(uint32_t *)rs2);
    *(uint32_t *)rd = (result >> 32);

}

/**
 * Divide signed.
 */
static inline void RiscvEmulatorDIV(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    if (*(int32_t *)rs2 == 0) {
        // Division by zero.
        *(int32_t *)rd = -1;
    } else if (*(int32_t *)rs1 == INT32_MIN && *(int32_t *)rs2 == -1) {
        // Overflow.
        *(int32_t *)rd = INT32_MIN;
    } else {
        *(int32_t *)rd = (*(int32_t *)rs1 / *(int32_t *)rs2);
    }

}

/**
 * Divide unsigned.
 */
static inline void RiscvEmulatorDIVU(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    if (*(uint32_t *)rs2 == 0) {
        // Division by zero.
        *(uint32_t *)rd = UINT32_MAX;
    } else {
        *(uint32_t *)rd = (*(uint32_t *)rs1 / *(uint32_t *)rs2);
    }

}

/**
 * Remainder signed.
 */
static inline void RiscvEmulatorREM(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    if (*(int32_t *)rs2 == 0) {
        // Division by zero.
        *(int32_t *)rd = *(int32_t *)rs1;
    } else if (*(int32_t *)rs1 == INT32_MIN && *(int32_t *)rs2 == -1) {
        // Overflow.
        *(int32_t *)rd = 0;
    } else {
        *(int32_t *)rd = (*(int32_t *)rs1 % *(int32_t *)rs2);
    }

}

/**
 * Remainder unsigned.
 */
static inline void RiscvEmulatorREMU(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    if (*(uint32_t *)rs2 == 0) {
        // Division by zero.
        *(uint32_t *)rd = *(uint32_t *)rs1;
    } else {
        *(uint32_t *)rd = (*(uint32_t *)rs1 % *(uint32_t *)rs2);
    }

}

/* ===== RiscvEmulatorExtensionZba.h ===== */

/* ===== RiscvEmulatorExtensionZbb.h ===== */

/* ===== RiscvEmulatorExtensionZbc.h ===== */

/* ===== RiscvEmulatorExtensionZbs.h ===== */

/* ===== RiscvEmulatorExtensionZicsr.h ===== */

/**
 * Return from machine mode.
 */
static inline void RiscvEmulatorMRET(RiscvEmulatorState_t *state) {

    // TODO: Determine what the new privilege mode will be according to the values of MPP and MPV in mstatus.
    // Could this always be M-mode for this emulator?

    state->csr.mstatush.mpv = 0;
    state->csr.mstatus.mpp = 0;
    state->csr.mstatus.mie = state->csr.mstatus.mpie;
    state->csr.mstatus.mpie = 1;

    // TODO: Set the privilege mode as previously determined.

    state->programcounternext = state->csr.mepc;

}

/**
 * Get the address of an CSR structure.
 *
 * Do not forget to update RiscvEmulatorGetCSRName()
 */
static inline void *RiscvEmulatorGetCSRAddress(RiscvEmulatorState_t *state, const uint16_t csr) {
    void *address = 0;
    switch (csr) {
        // Machine Information Registers
        case 0xF14:
            address = &state->csr.mhartid;
            break;

        // Machine Trap Setup
        case 0x300:
            address = &state->csr.mstatus;
            break;
        case 0x301:
            address = &state->csr.misa;
            break;
        case 0x302:
            address = &state->csr.medeleg;
            break;
        case 0x303:
            address = &state->csr.mideleg;
            break;
        case 0x304:
            address = &state->csr.mie;
            break;
        case 0x305:
            address = &state->csr.mtvec;
            break;
        case 0x310:
            address = &state->csr.mstatush;
            break;

        // Machine Trap Handling
        case 0x340:
            address = &state->csr.mscratch;
            break;
        case 0x341:
            address = &state->csr.mepc;
            break;
        case 0x342:
            address = &state->csr.mcause;
            break;
        case 0x343:
            address = &state->csr.mtval;
            break;
        case 0x344:
            address = &state->csr.mip;
            break;

        // Machine Memory Protection
        case 0x3A0:
            address = &state->csr.pmpcfg0;
            break;
        case 0x3B0:
            address = &state->csr.pmpaddr0;
            break;

        // Machine Non-Maskable Interrupt Handling
        case 0x744:
            address = &state->csr.mnstatus;
            break;

        // Supervisor Protection and Translation
        case 0x180:
            address = &state->csr.satp;
            break;

        default:
            state->trapflag.illegalinstruction = 1;
            RiscvEmulatorUnknownCSR(state);
    }

    return address;
}

/**
 * Atomic read and write CSR.
 */
static inline void RiscvEmulatorCSRRW(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    const void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint16_t csrnum __attribute__((unused)),
    const void *csr) {

    uint32_t originalvaluers1 = *(uint32_t *)rs1;

    // Read old value into destination register when requested.
    if (rdnum != 0) {
        *(uint32_t *)rd = *(uint32_t *)csr;
    }

    *(uint32_t *)csr = originalvaluers1;

}

/**
 * Atomic read and write CSR, immediate.
 */
static inline void RiscvEmulatorCSRRWI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    const void *rd,
    const uint8_t uimm,
    const uint16_t csrnum __attribute__((unused)),
    const void *csr) {

    // Read old value into destination register when requested.
    if (rdnum != 0) {
        *(uint32_t *)rd = *(uint32_t *)csr;
    }

    *(uint32_t *)csr = uimm;

}

/**
 * Atomic read and set bits in CSR.
 */
static inline void RiscvEmulatorCSRRS(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    const void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint16_t csrnum __attribute__((unused)),
    const void *csr) {

    int32_t initialrs1value = *(uint32_t *)rs1;

    if (rdnum != 0) {
        *(uint32_t *)rd = *(uint32_t *)csr;
    }

    // Set bits when requested.
    if (initialrs1value != 0) {
        *(uint32_t *)csr |= initialrs1value;
    }

}

/**
 * Atomic read and set bits in CSR, immediate.
 */
static inline void RiscvEmulatorCSRRSI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    const void *rd,
    const uint8_t uimm,
    const uint16_t csrnum __attribute__((unused)),
    const void *csr) {

    if (rdnum != 0) {
        *(uint32_t *)rd = *(uint32_t *)csr;
    }

    // Set bits when requested.
    if (uimm != 0) {
        *(uint32_t *)csr |= uimm;
    }

}

/**
 * Atomic read and clear bits in CSR.
 */
static inline void RiscvEmulatorCSRRC(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    const void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint16_t csrnum __attribute__((unused)),
    const void *csr) {

    int32_t initialrs1value = *(uint32_t *)rs1;

    if (rdnum != 0) {
        *(uint32_t *)rd = *(uint32_t *)csr;
    }

    // Clear bits when requested.
    if (initialrs1value != 0) {
        *(uint32_t *)csr &= ~initialrs1value;
    }

}

/**
 * Atomic read and clear bits in CSR, immediate.
 */
static inline void RiscvEmulatorCSRRCI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    const void *rd,
    const uint8_t uimm,
    const uint16_t csrnum __attribute__((unused)),
    const void *csr) {

    if (rdnum != 0) {
        *(uint32_t *)rd = *(uint32_t *)csr;
    }

    // Clear bits when requested.
    if (uimm != 0) {
        *(uint32_t *)csr &= ~uimm;
    }

}

/**
 * Jump and link register.
 */
static inline void RiscvEmulatorJALR(RiscvEmulatorState_t *state) {
    uint8_t rdnum = state->instruction.itype.rd;
    void *rd = &state->reg.x[rdnum];
    uint8_t rs1num = state->instruction.itype.rs1;
    void *rs1 = &state->reg.x[rs1num];

    int16_t imm = state->instruction.itype.imm;

    uint32_t jumptoprogramcounter = (*(uint32_t *)rs1 + imm) & (UINT32_MAX - 1);

    // Set destination register to current next instruction acting as a return address.
    if (rdnum != 0) {
        *(uint32_t *)rd = state->programcounternext;
    }

    // Execute jump.
    state->programcounternext = jumptoprogramcounter;

}

/**
 * Process JALR opcode.
 */
static inline void RiscvEmulatorOpcodeJumpAndLinkRegister(RiscvEmulatorState_t *state) {
    if (state->instruction.itype.funct3 == FUNCT3_JUMPANDLINKREGISTER_JALR) {
        RiscvEmulatorJALR(state);
    } else {
        state->trapflag.illegalinstruction = 1;
    }
}

/**
 * Add: rd = rs1 + rs2
 */
static inline void RiscvEmulatorADD(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rs1 + *(int32_t *)rs2;

}

/**
 * Add: rd = rs1 + imm
 */
static inline void RiscvEmulatorADDI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const int16_t imm) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rs1 + imm;

}

/**
 * Subtract: rd = rs1 - rs2
 */
static inline void RiscvEmulatorSUB(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rs1 - *(int32_t *)rs2;

}

/**
 * Logical shift left: rd = rs1 << (rs2 & 0b11111)
 */
static inline void RiscvEmulatorSLL(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = *(uint32_t *)rs1 << (*(uint32_t *)rs2 & 0b11111);

}

/**
 * Logical shift left: rd = rs1 << (shamt & 0b11111)
 */
static inline void RiscvEmulatorSLLI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t shamt) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = *(uint32_t *)rs1 << (shamt & 0b11111);

}

/**
 * Signed compare: rd = (rs1 < rs2)
 */
static inline void RiscvEmulatorSLT(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = (*(int32_t *)rs1 < *(int32_t *)rs2);

}

/**
 * Signed compare: rd = (rs1 < imm)
 */
static inline void RiscvEmulatorSLTI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const int16_t imm) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = (*(int32_t *)rs1 < imm);

}

/**
 * Unsigned compare: rd = (rs1 < rs2)
 */
static inline void RiscvEmulatorSLTU(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = (*(uint32_t *)rs1 < *(uint32_t *)rs2);

}

/**
 * Unsigned compare: rd = (rs1 < imm)
 */
static inline void RiscvEmulatorSLTIU(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint32_t imm) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = (*(uint32_t *)rs1 < imm);

}

/**
 * Exclusive or: rd = rs1 ^ rs2
 */
static inline void RiscvEmulatorXOR(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = *(uint32_t *)rs1 ^ *(uint32_t *)rs2;

}

/**
 * Exclusive or: rd = rs1 ^ imm
 */
static inline void RiscvEmulatorXORI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint32_t imm) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = *(uint32_t *)rs1 ^ imm;

}

/**
 * Logical shift right: rd = rs1 >> (rs2 & 0b11111)
 */
static inline void RiscvEmulatorSRL(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = *(uint32_t *)rs1 >> (*(uint32_t *)rs2 & 0b11111);

}

/**
 * Logical shift right: rd = rs1 >> (shamt & 0b11111)
 */
static inline void RiscvEmulatorSRLI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t shamt) {

    if (rdnum == 0) {
        return;
    }

    *(uint32_t *)rd = *(uint32_t *)rs1 >> (shamt & 0b11111);

}

/**
 * Arithmetic shift right: rd = rs1 >> (rs2 & 0b11111)
 */
static inline void RiscvEmulatorSRA(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rs1 >> (*(uint32_t *)rs2 & 0b11111);

}

/**
 * Arithmetic shift right: rd = rs1 >> (shamt & 0b11111)
 */
static inline void RiscvEmulatorSRAI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t shamt) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rs1 >> (shamt & 0b11111);

}

/**
 * Boolean or: rd = rs1 | rs2
 */
static inline void RiscvEmulatorOR(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rs1 | *(int32_t *)rs2;

}

/**
 * Boolean or: rd = rs1 | imm
 */
static inline void RiscvEmulatorORI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const int16_t imm) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rs1 | imm;

}

/**
 * Boolean and: rd = rs1 & rs2
 */
static inline void RiscvEmulatorAND(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum,
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rs1 & *(int32_t *)rs2;

}

/**
 * Boolean and: rd = rs1 & imm
 */
static inline void RiscvEmulatorANDI(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rdnum __attribute__((unused)),
    void *rd,
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const int16_t imm) {

    if (rdnum == 0) {
        return;
    }

    *(int32_t *)rd = *(int32_t *)rs1 & imm;

}

/**
 * Process operation opcodes.
 */
static inline void RiscvEmulatorOpcodeOperation(RiscvEmulatorState_t *state) {
    int8_t detectedUnknownInstruction = 1;

    uint8_t rdnum = state->instruction.rtype.rd;
    void *rd = &state->reg.x[rdnum];
    uint8_t rs1num = state->instruction.rtype.rs1;
    void *rs1 = &state->reg.x[rs1num];
    uint8_t rs2num = state->instruction.rtype.rs2;
    void *rs2 = &state->reg.x[rs2num];

    if (detectedUnknownInstruction == 1) {
        RiscvInstructionTypeRDecoderFunct7Funct3_u instruction_decoderhelper_rtype = {0};
        instruction_decoderhelper_rtype.funct3 = state->instruction.rtype.funct3;
        instruction_decoderhelper_rtype.funct7 = state->instruction.rtype.funct7;

        detectedUnknownInstruction = -1;
        switch (instruction_decoderhelper_rtype.funct7_3) {
            case FUNCT7_FUNCT3_OPERATION_ADD:
                RiscvEmulatorADD(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_SUB:
                RiscvEmulatorSUB(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_SLL:
                RiscvEmulatorSLL(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_SLT:
                RiscvEmulatorSLT(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_SLTU:
                RiscvEmulatorSLTU(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_XOR:
                RiscvEmulatorXOR(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_SRL:
                RiscvEmulatorSRL(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_SRA:
                RiscvEmulatorSRA(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_OR:
                RiscvEmulatorOR(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_AND:
                RiscvEmulatorAND(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_MUL:
                RiscvEmulatorMUL(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_MULH:
                RiscvEmulatorMULH(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_MULHSU:
                RiscvEmulatorMULHSU(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_MULHU:
                RiscvEmulatorMULHU(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_DIV:
                RiscvEmulatorDIV(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_DIVU:
                RiscvEmulatorDIVU(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_REM:
                RiscvEmulatorREM(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            case FUNCT7_FUNCT3_OPERATION_REMU:
                RiscvEmulatorREMU(state, rdnum, rd, rs1num, rs1, rs2num, rs2);
                break;
            default:
                detectedUnknownInstruction = 1;
                break;
        }
    }

    if (detectedUnknownInstruction == 1) {
        state->trapflag.illegalinstruction = 1;
    }
}

/**
 * Process immediate opcodes.
 */
static inline void RiscvEmulatorOpcodeImmediate(RiscvEmulatorState_t *state) {
    int8_t detectedUnknownInstruction = 1;

    uint8_t rdnum = state->instruction.itype.rd;
    void *rd = &state->reg.x[rdnum];
    uint8_t rs1num = state->instruction.itype.rs1;
    void *rs1 = &state->reg.x[rs1num];

    if (detectedUnknownInstruction == 1) {
        // If funct3 == 0b001 or 0b101 then a whole set of functions are encoded in parts of imm.
        if (state->instruction.itype.funct3 == FUNCT3_IMMEDIATE_FUNCTIONS_1 ||
            state->instruction.itype.funct3 == FUNCT3_IMMEDIATE_FUNCTIONS_5) {

            uint8_t shamt = state->instruction.itypeshiftbyconstant.shamt;

            RiscvInstructionTypeIDecoderImm11_7Funct3Imm11_7Funct3_u instruction_decoderhelper_itype_functions_shamt = {0};

            instruction_decoderhelper_itype_functions_shamt.funct3 = state->instruction.itype.funct3;
            instruction_decoderhelper_itype_functions_shamt.imm11_5 = state->instruction.itypeshiftbyconstant.imm11_5;

            detectedUnknownInstruction = -1;
            switch (instruction_decoderhelper_itype_functions_shamt.imm11_5funct3) {
                case IMM11_5_FUNCT3_IMMEDIATE_SLLI:
                    RiscvEmulatorSLLI(state, rdnum, rd, rs1num, rs1, shamt);
                    break;
                case IMM11_5_FUNCT3_IMMEDIATE_SRLI:
                    RiscvEmulatorSRLI(state, rdnum, rd, rs1num, rs1, shamt);
                    break;
                case IMM11_5_FUNCT3_IMMEDIATE_SRAI:
                    RiscvEmulatorSRAI(state, rdnum, rd, rs1num, rs1, shamt);
                    break;
                default:
                    detectedUnknownInstruction = 1;
                    break;
            }
        }
    }

    if (detectedUnknownInstruction == 1) {
        detectedUnknownInstruction = -1;
        int16_t imm = state->instruction.itype.imm;
        switch (state->instruction.itype.funct3) {
            case FUNCT3_IMMEDIATE_ADDI:
                RiscvEmulatorADDI(state, rdnum, rd, rs1num, rs1, imm);
                break;
            case FUNCT3_IMMEDIATE_SLTI:
                RiscvEmulatorSLTI(state, rdnum, rd, rs1num, rs1, imm);
                break;
            case FUNCT3_IMMEDIATE_SLTIU:
                RiscvEmulatorSLTIU(state, rdnum, rd, rs1num, rs1, imm);
                break;
            case FUNCT3_IMMEDIATE_XORI:
                RiscvEmulatorXORI(state, rdnum, rd, rs1num, rs1, imm);
                break;
            case FUNCT3_IMMEDIATE_ORI:
                RiscvEmulatorORI(state, rdnum, rd, rs1num, rs1, imm);
                break;
            case FUNCT3_IMMEDIATE_ANDI:
                RiscvEmulatorANDI(state, rdnum, rd, rs1num, rs1, imm);
                break;
            default:
                detectedUnknownInstruction = 1;
                break;
        }
    }

    if (detectedUnknownInstruction == 1) {
        state->trapflag.illegalinstruction = 1;
    }
}

/**
 * Process load opcodes.
 */
static inline void RiscvEmulatorOpcodeLoad(RiscvEmulatorState_t *state) {
    uint8_t rdnum = state->instruction.itype.rd;
    void *rd = &state->reg.x[rdnum];
    uint8_t rs1num = state->instruction.stype.rs1;
    void *rs1 = &state->reg.x[rs1num];

    int16_t imm = state->instruction.itype.imm;
    uint32_t memorylocation = imm + *(uint32_t *)rs1;

    uint8_t length = 0;
    switch (state->instruction.itype.funct3) {
        case FUNCT3_LOAD_LB:
            length = sizeof(uint8_t);
            break;
        case FUNCT3_LOAD_LBU:
            length = sizeof(uint8_t);
            break;
        case FUNCT3_LOAD_LH:
            length = sizeof(uint16_t);
            break;
        case FUNCT3_LOAD_LHU:
            length = sizeof(uint16_t);
            break;
        case FUNCT3_LOAD_LW:
            length = sizeof(uint32_t);
            break;
        default:
            state->trapflag.illegalinstruction = 1;
            return;
    }

    // Check if the load is aligned.
    if (length > 1) {
        // Only the last few bits need to be checked.
        uint8_t memorylocation8 = memorylocation & 0xFF;
        if ((memorylocation8 % length) != 0) {
            state->trapflag.loadaddressmisaligned = 1;
            state->csr.mtval = memorylocation;
        }
    }

    if (rdnum == 0) {
        return;
    }

    if (state->trapflag.loadaddressmisaligned == 1) {
        return;
    }

    uint32_t value = 0;
    RiscvEmulatorLoad(memorylocation, &value, length);

    switch (state->instruction.itype.funct3) {
        case FUNCT3_LOAD_LB:
            *(int32_t *)rd = (int8_t)value;
            break;
        case FUNCT3_LOAD_LBU:
            *(uint32_t *)rd = (uint8_t)value;
            break;
        case FUNCT3_LOAD_LH:
            *(int32_t *)rd = (int16_t)value;
            break;
        case FUNCT3_LOAD_LHU:
            *(uint32_t *)rd = (uint16_t)value;
            break;
        case FUNCT3_LOAD_LW:
            *(uint32_t *)rd = (uint32_t)value;
            break;
    }

}

/**
 * Process store opcodes.
 */
static inline void RiscvEmulatorOpcodeStore(RiscvEmulatorState_t *state) {
    // Untangle the immediate bits.
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
        case FUNCT3_STORE_SW:
            length = sizeof(uint32_t);
            break;
        case FUNCT3_STORE_SH:
            length = sizeof(uint16_t);
            break;
        case FUNCT3_STORE_SB:
            length = sizeof(uint8_t);
            break;
        default:
            state->trapflag.illegalinstruction = 1;
            return;
    }

    // Check if the store is aligned.
    if (length > 1) {
        // Only the last few bits need to be checked.
        uint8_t memorylocation8 = memorylocation & 0xFF;
        if ((memorylocation8 % length) != 0) {
            state->trapflag.storeaddressmisaligned = 1;
            state->csr.mtval = memorylocation;
        }
    }

    if (state->trapflag.storeaddressmisaligned == 1) {
        return;
    }

    RiscvEmulatorStore(memorylocation, rs2, length);

}

/**
 * Branch if equal.
 */
static inline void RiscvEmulatorBEQ(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2,
    const int16_t imm __attribute__((unused)),
    uint8_t *executebranch,
    RiscvEmulatorHookContext_t *hc) {

    if (*(int32_t *)rs1 == *(int32_t *)rs2) {
        *executebranch = BRANCH_YES;
    }
}

/**
 * Branch if not equal.
 */
static inline void RiscvEmulatorBNE(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2,
    const int16_t imm __attribute__((unused)),
    uint8_t *executebranch,
    RiscvEmulatorHookContext_t *hc) {

    if (*(int32_t *)rs1 != *(int32_t *)rs2) {
        *executebranch = BRANCH_YES;
    }
}

/**
 * Branch if greater than or equal.
 */
static inline void RiscvEmulatorBGE(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2,
    const int16_t imm __attribute__((unused)),
    uint8_t *executebranch,
    RiscvEmulatorHookContext_t *hc) {

    if (*(int32_t *)rs1 >= *(int32_t *)rs2) {
        *executebranch = BRANCH_YES;
    }
}

/**
 * Branch if greater than or equal unsigned.
 */
static inline void RiscvEmulatorBGEU(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2,
    const int16_t imm __attribute__((unused)),
    uint8_t *executebranch,
    RiscvEmulatorHookContext_t *hc) {

    if (*(uint32_t *)rs1 >= *(uint32_t *)rs2) {
        *executebranch = BRANCH_YES;
    }
}

/**
 * Branch if less than.
 */
static inline void RiscvEmulatorBLT(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2,
    const int16_t imm __attribute__((unused)),
    uint8_t *executebranch,
    RiscvEmulatorHookContext_t *hc) {

    if (*(int32_t *)rs1 < *(int32_t *)rs2) {
        *executebranch = BRANCH_YES;
    }
}

/**
 * Branch if less than unsigned.
 */
static inline void RiscvEmulatorBLTU(
    RiscvEmulatorState_t *state __attribute__((unused)),
    const uint8_t rs1num __attribute__((unused)),
    const void *rs1,
    const uint8_t rs2num __attribute__((unused)),
    const void *rs2,
    const int16_t imm __attribute__((unused)),
    uint8_t *executebranch,
    RiscvEmulatorHookContext_t *hc) {

    if (*(uint32_t *)rs1 < *(uint32_t *)rs2) {
        *executebranch = BRANCH_YES;
    }
}

/**
 * Process branch opcodes.
 */
static inline void RiscvEmulatorOpcodeBranch(RiscvEmulatorState_t *state) {
    uint8_t executebranch = BRANCH_NO;
    uint8_t rs1num = state->instruction.btype.rs1;
    void *rs1 = &state->reg.x[rs1num];
    uint8_t rs2num = state->instruction.btype.rs2;
    void *rs2 = &state->reg.x[rs2num];

    // Untangle the immediate bits.
    RiscvInstructionTypeBDecoderImm_u immdecoder = {0};
    immdecoder.bit.imm4_1 = state->instruction.btype.imm4_1;
    immdecoder.bit.imm10_5 = state->instruction.btype.imm10_5;
    immdecoder.bit.imm11 = state->instruction.btype.imm11;
    immdecoder.bit.imm12 = state->instruction.btype.imm12;
    int16_t imm = immdecoder.imm;

    RiscvEmulatorHookContext_t hc;

    switch (state->instruction.btype.funct3) {
        case FUNCT3_BRANCH_BEQ:
            RiscvEmulatorBEQ(state, rs1num, rs1, rs2num, rs2, imm, &executebranch, &hc);
            break;
        case FUNCT3_BRANCH_BNE:
            RiscvEmulatorBNE(state, rs1num, rs1, rs2num, rs2, imm, &executebranch, &hc);
            break;
        case FUNCT3_BRANCH_BGE:
            RiscvEmulatorBGE(state, rs1num, rs1, rs2num, rs2, imm, &executebranch, &hc);
            break;
        case FUNCT3_BRANCH_BGEU:
            RiscvEmulatorBGEU(state, rs1num, rs1, rs2num, rs2, imm, &executebranch, &hc);
            break;
        case FUNCT3_BRANCH_BLT:
            RiscvEmulatorBLT(state, rs1num, rs1, rs2num, rs2, imm, &executebranch, &hc);
            break;
        case FUNCT3_BRANCH_BLTU:
            RiscvEmulatorBLTU(state, rs1num, rs1, rs2num, rs2, imm, &executebranch, &hc);
            break;
        default:
            state->trapflag.illegalinstruction = 1;
            return;
    }

    if (executebranch == BRANCH_YES) {
        state->programcounternext = state->programcounter + imm;

    }
}

/**
 * Add upper immediate to program counter.
 */
static inline void RiscvEmulatorAUIPC(RiscvEmulatorState_t *state) {
    uint32_t upperimmediate = state->instruction.utype.imm31_12;

    RiscvInstructionTypeUDecoderImm_u immdecoder = {0};
    immdecoder.bit.imm31_12 = upperimmediate;
    int32_t imm = immdecoder.imm;

    uint8_t rdnum = state->instruction.utype.rd;

    if (rdnum != 0) {
        state->reg.x[rdnum] = state->programcounter + imm;
    }

}

/**
 * Load upper with immediate.
 */
static inline void RiscvEmulatorLUI(RiscvEmulatorState_t *state) {
    RiscvInstructionTypeUDecoderImm_u immdecoder = {0};
    immdecoder.bit.imm11_0 = 0;
    immdecoder.bit.imm31_12 = state->instruction.utype.imm31_12;

    uint8_t rdnum = state->instruction.utype.rd;
    void *rd = &state->reg.x[rdnum];

    uint32_t imm = immdecoder.imm;

    if (rdnum != 0) {
        *(uint32_t *)rd = imm;
    }

}

/**
 * Jump and link.
 */
static inline void RiscvEmulatorJAL(RiscvEmulatorState_t *state) {
    uint8_t rdnum = state->instruction.jtype.rd;
    void *rd = &state->reg.x[rdnum];

    // Untangle the immediate bits.
    RiscvInstructionTypeJDecoderImm_u immdecoder = {0};
    immdecoder.bit.imm10_1 = state->instruction.jtype.imm10_1;
    immdecoder.bit.imm11 = state->instruction.jtype.imm11;
    immdecoder.bit.imm19_12 = state->instruction.jtype.imm19_12;
    immdecoder.bit.imm20 = state->instruction.jtype.imm20;

    uint32_t jumptoprogramcounter = state->programcounter + immdecoder.imm;

    // Set destination register to current next instruction acting as a return address.
    if (rdnum != 0) {
        *(uint32_t *)rd = state->programcounternext;
    }

    // Execute jump.
    state->programcounternext = jumptoprogramcounter;

}

/**
 * Make a service request to the execution environment.
 */
static inline void RiscvEmulatorECALL(RiscvEmulatorState_t *state) {

    state->trapflag.environmentcallfrommmode = 1;

    RiscvEmulatorHandleECALL(state);
}

/**
 * Cause control to be transferred back to a debugging environment.
 */
static inline void RiscvEmulatorEBREAK(RiscvEmulatorState_t *state) {

    state->trapflag.breakpoint = 1;
    state->csr.mtval = state->programcounter;

    RiscvEmulatorHandleEBREAK(state);
}

/**
 * Process system opcodes.
 */
static inline void RiscvEmulatorOpcodeSystem(RiscvEmulatorState_t *state) {
    int8_t detectedUnknownInstruction = 1;

    if (detectedUnknownInstruction == 1) {
        if (state->instruction.itypesystem.rd == 0 &&
            state->instruction.itypesystem.funct3 == 0 &&
            state->instruction.itypesystem.rs1 == 0) {
            detectedUnknownInstruction = -1;
            switch (state->instruction.itypesystem.funct12) {
                case FUNCT12_MRET:
                    RiscvEmulatorMRET(state);
                    break;
                case FUNCT12_ECALL:
                    RiscvEmulatorECALL(state);
                    break;
                case FUNCT12_EBREAK:
                    RiscvEmulatorEBREAK(state);
                    break;
                default:
                    detectedUnknownInstruction = 1;
                    break;
            }
        }
    }

    if (detectedUnknownInstruction == 1) {
        detectedUnknownInstruction = -1;

        uint8_t rdnum = state->instruction.itypecsr.rd;
        void *rd = &state->reg.x[rdnum];

        uint8_t rs1num = state->instruction.itypecsr.rs1;
        void *rs1 = &state->reg.x[rs1num];

        uint8_t imm = state->instruction.itypecsrimm.imm;

        uint16_t csrnum = state->instruction.itypecsr.csr;
        void *csr = RiscvEmulatorGetCSRAddress(state, csrnum);

        if (state->trapflag.value > 0) {
            return;
        }

        switch (state->instruction.itypecsr.funct3) {
            case FUNCT3_CSR_CSRRW:
                RiscvEmulatorCSRRW(state, rdnum, rd, rs1num, rs1, csrnum, csr);
                break;
            case FUNCT3_CSR_CSRRWI:
                RiscvEmulatorCSRRWI(state, rdnum, rd, imm, csrnum, csr);
                break;
            case FUNCT3_CSR_CSRRS:
                RiscvEmulatorCSRRS(state, rdnum, rd, rs1num, rs1, csrnum, csr);
                break;
            case FUNCT3_CSR_CSRRSI:
                RiscvEmulatorCSRRSI(state, rdnum, rd, imm, csrnum, csr);
                break;
            case FUNCT3_CSR_CSRRC:
                RiscvEmulatorCSRRC(state, rdnum, rd, rs1num, rs1, csrnum, csr);
                break;
            case FUNCT3_CSR_CSRRCI:
                RiscvEmulatorCSRRCI(state, rdnum, rd, imm, csrnum, csr);
                break;
            default:
                detectedUnknownInstruction = 1;
                break;
        }
    }

    if (detectedUnknownInstruction == 1) {
        state->trapflag.illegalinstruction = 1;
    }
}

/**
 * Excutes the fence instuction.
 *
 * This does nothing in this emulator because all memory access is always completely processed.
 */
static inline void RiscvEmulatorFence(
    RiscvEmulatorState_t *state __attribute__((unused))) {
}

/**
 * Excutes the fencei instuction.
 *
 * This does nothing in this emulator because all memory access is always completely processed.
 */
static inline void RiscvEmulatorFencei(
    RiscvEmulatorState_t *state __attribute__((unused))) {
}

/**
 * Process miscellaneous memory opcodes.
 */
static inline void RiscvEmulatorOpcodeMiscMem(RiscvEmulatorState_t *state) {
    uint8_t detectedUnknownInstruction = 1;

    if (detectedUnknownInstruction) {
        if (state->instruction.itypemiscmem.rd == 0 &&
            state->instruction.itypemiscmem.funct3 == FUNCT3_FENCE &&
            state->instruction.itypemiscmem.rs1 == 0) {
            detectedUnknownInstruction = -1;
            RiscvEmulatorFence(state);
        }
    }

    if (detectedUnknownInstruction) {
        if (state->instruction.itypemiscmem.rd == 0 &&
            state->instruction.itypemiscmem.funct3 == FUNCT3_FENCEI &&
            state->instruction.itypemiscmem.rs1 == 0) {
            detectedUnknownInstruction = -1;
            RiscvEmulatorFencei(state);
        }
    }

    if (detectedUnknownInstruction == 1) {
        state->trapflag.illegalinstruction = 1;
    }
}

/* ===== RiscvEmulatorTrap.h ===== */

/**
 * Handle a trap.
 */
static inline void RiscvEmulatorTrap(RiscvEmulatorState_t *state) {

    // Instruction address misaligned
    if (state->trapflag.instructionaddressmisaligned == 1) {
        state->csr.mcause.exceptioncode = MCAUSE_EXCEPTION_CODE_INSTRUCTION_ADDRESS_MISALIGNED;
    }

    // Breakpoint
    if (state->trapflag.breakpoint == 1) {
        state->csr.mcause.exceptioncode = MCAUSE_EXCEPTION_CODE_BREAKPOINT;
    }

    // Load address misaligned
    if (state->trapflag.loadaddressmisaligned == 1) {
        state->csr.mcause.exceptioncode = MCAUSE_EXCEPTION_CODE_LOAD_ADDRESS_MISALIGNED;
    }

    // Store/AMO address misaligned
    if (state->trapflag.storeaddressmisaligned == 1) {
        state->csr.mcause.exceptioncode = MCAUSE_EXCEPTION_CODE_STORE_ADDRESS_MISALIGNED;
    }

    //  Environment call from M-mode
    if (state->trapflag.environmentcallfrommmode == 1) {
        state->csr.mcause.exceptioncode = MCAUSE_EXCEPTION_CODE_ENVIRONMENT_CALL_FROM_MMODE;
    }

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
    if (state->csr.mtvec.mode == 1) {
        state->programcounternext = 4 * state->csr.mcause.exceptioncode;
    }

    if (state->trapflag.illegalinstruction == 1) {
        RiscvEmulatorIllegalInstruction(state);
    }

    state->trapflag.value = 0;
}

/* ===== RiscvEmulator.h ===== */

/**
 * Initialize the emulator.
 *
 * @param ram_length The size in bytes of the RAM available.
 */
static inline void RiscvEmulatorInit(RiscvEmulatorState_t *state, uint32_t ram_length) {
    // Initialize stack pointer.
    state->reg.sp = RAM_ORIGIN + ram_length;

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

/**
 * Call this function repeatedly to execute the emulator one instruction at a time.
 */
static inline void RiscvEmulatorLoop(RiscvEmulatorState_t *state) {

    state->programcounter = state->programcounternext;

    uint8_t instructionlength = 32;

    // Read 16 bits.
    state->instruction.H = 0;
    RiscvEmulatorLoad(
        state->programcounter,
        &state->instruction.L,
        sizeof(state->instruction.L));

    state->programcounternext += sizeof(state->instruction.L);

    // Read another 16 bits when this is a 32-bit instruction.
    if (state->instruction.copcode.op == OPCODE16_QUADRANT_INVALID) {
        RiscvEmulatorLoad(
            state->programcounternext,
            &state->instruction.H,
            sizeof(state->instruction.L));

        state->programcounternext += sizeof(state->instruction.L);
    } else {
        instructionlength = 16;
    }

    if (instructionlength == 16) {
        RiscvEmulatorOpcodeCompressed(state);
    }

    if (instructionlength == 32) {
        switch (state->instruction.opcode) {
            case OPCODE32_JUMPANDLINKREGISTER:
                RiscvEmulatorOpcodeJumpAndLinkRegister(state);
                break;
            case OPCODE32_OPERATION:
                RiscvEmulatorOpcodeOperation(state);
                break;
            case OPCODE32_IMMEDIATE:
                RiscvEmulatorOpcodeImmediate(state);
                break;
            case OPCODE32_LOAD:
                RiscvEmulatorOpcodeLoad(state);
                break;
            case OPCODE32_STORE:
                RiscvEmulatorOpcodeStore(state);
                break;
            case OPCODE32_BRANCH:
                RiscvEmulatorOpcodeBranch(state);
                break;
            case OPCODE32_ADDUPPERIMMEDIATE2PC:
                RiscvEmulatorAUIPC(state);
                break;
            case OPCODE32_LOADUPPERIMMEDIATE:
                RiscvEmulatorLUI(state);
                break;
            case OPCODE32_JUMPANDLINK:
                RiscvEmulatorJAL(state);
                break;
            case OPCODE32_SYSTEM:
                RiscvEmulatorOpcodeSystem(state);
                break;
            case OPCODE32_MISCMEM:
                RiscvEmulatorOpcodeMiscMem(state);
                break;
            default:
                state->trapflag.illegalinstruction = 1;
                break;
        }
    }

    if (state->trapflag.value > 0) {
        RiscvEmulatorTrap(state);
    }
}

#pragma GCC diagnostic pop

#endif /* PURV_H_ */
