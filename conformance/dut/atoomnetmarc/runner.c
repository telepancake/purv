// ACT4 runner for atoomnetmarc/RISC-V-emulator.
//
// Loads an ACT4 self-checking ELF into a flat memory at 0x80000000, runs the
// emulator one instruction at a time until it stores to SYSCON (RVMODEL_HALT),
// and returns 0 (PASS) / non-zero (FAIL). UART writes go to stdout.
//
//   usage: atoomnetmarc-runner <elf>
//
// Build with the target ISA enabled, e.g. RV32IMC:
//   -DRVE_E_M=1 -DRVE_E_C=1 -DRVE_E_ZICSR=1 -DRVE_E_ZIFENCEI=1
//   -DRVE_E_ZBA=0 -DRVE_E_ZBB=0 -DRVE_E_ZBC=0 -DRVE_E_ZBS=0 -DRVE_E_HOOK=0

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

#include "memory.h"

uint8_t memory[RAM_LENGTH];
uint8_t pleasestop;
int g_exitcode = 1;   // default FAIL unless a HALT_PASS (SYSCON 0x5555) occurs

#include <RiscvEmulator.h>

static RiscvEmulatorState_t st;

static uint32_t load_elf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "runner: cannot open %s\n", path); exit(2); }
    Elf32_Ehdr eh;
    if (fread(&eh, sizeof(eh), 1, f) != 1 || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "runner: %s not an ELF\n", path); exit(2);
    }
    for (int i = 0; i < eh.e_phnum; i++) {
        Elf32_Phdr ph;
        fseek(f, eh.e_phoff + (long)i * eh.e_phentsize, SEEK_SET);
        if (fread(&ph, sizeof(ph), 1, f) != 1) break;
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        uint32_t off = ph.p_paddr - RAM_ORIGIN;
        if ((uint64_t)off + ph.p_memsz > RAM_LENGTH) {
            fprintf(stderr, "runner: segment 0x%x out of RAM\n", ph.p_paddr); exit(2);
        }
        if (ph.p_filesz) {
            fseek(f, ph.p_offset, SEEK_SET);
            if (fread(&memory[off], ph.p_filesz, 1, f) != 1) {
                fprintf(stderr, "runner: short read\n"); exit(2);
            }
        }
    }
    fclose(f);
    return eh.e_entry;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <elf>\n", argv[0]); return 2; }
    uint32_t entry = load_elf(argv[1]);

    pleasestop = 0;
    RiscvEmulatorInit(&st, RAM_LENGTH);
    st.programcounter = entry;       // ACT4 ELFs live at RAM_ORIGIN, not ROM_ORIGIN
    st.programcounternext = entry;

    const uint64_t MAX_INSTRS = 256ull * 1024 * 1024;
    for (uint64_t i = 0; i < MAX_INSTRS && !pleasestop; i++) {
        RiscvEmulatorLoop(&st);
    }
    if (!pleasestop) {
        fprintf(stderr, "runner: instruction cap reached without HALT\n");
        return 2;
    }
    return g_exitcode;
}
