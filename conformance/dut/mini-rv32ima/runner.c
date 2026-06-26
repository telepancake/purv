// ACT4 runner for cnlohr/mini-rv32ima.
//
// The stock mini-rv32ima.c wrapper loads a flat binary + DTB and only knows how
// to poweroff (exit 0). ACT4 self-checking ELFs instead need a DUT that:
//   * loads an ELF,
//   * exposes a UART so RVMODEL_IO_WRITE_STR text (incl. "RVCP-SUMMARY: ...")
//     reaches stdout, and
//   * exits 0 on RVMODEL_HALT_PASS and non-zero on RVMODEL_HALT_FAIL.
//
// This runner embeds mini-rv32ima.h directly and provides exactly that. The
// emulator core (incl. trap vectoring) is unchanged; we only own loading + I/O.
//
//   usage: mini-rv32ima-runner <elf>
//
// Memory map (matches conformance/dut/mini-rv32ima/rvmodel_macros.h):
//   RAM    base 0x80000000 (64 MiB)
//   UART   0x10000000  (byte store -> stdout)
//   SYSCON 0x11100000  (store 0x5555 -> exit 0 / PASS, anything else -> exit 1)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

static int HandleControlStore(uint32_t addy, uint32_t val);
static uint32_t HandleControlLoad(uint32_t addy);
static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value);
static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno);

static uint32_t ram_amt = 64 * 1024 * 1024;
static uint8_t *ram_image = 0;
static struct MiniRV32IMAState *core;

// mini-rv32ima.h configuration hooks.
#define MINIRV32WARN(x...) fprintf(stderr, x)
#define MINIRV32_DECORATE static
#define MINI_RV32_RAM_SIZE ram_amt
#define MINIRV32_IMPLEMENTATION
#define MINIRV32_POSTEXEC(pc, ir, retval) \
    { if (retval > 0) retval = retval; }   // traps vector internally; nothing to do
#define MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val) \
    if (HandleControlStore(addy, val)) return val;
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL(addy, rval) rval = HandleControlLoad(addy);
#define MINIRV32_OTHERCSR_WRITE(csrno, value) HandleOtherCSRWrite(image, csrno, value);
#define MINIRV32_OTHERCSR_READ(csrno, value) value = HandleOtherCSRRead(image, csrno);

#include "mini-rv32ima.h"

// UART -> stdout; SYSCON -> break out of the step loop with the written value.
static int HandleControlStore(uint32_t addy, uint32_t val) {
    if (addy == 0x10000000) {            // UART THR
        putchar((int)(val & 0xff));
        fflush(stdout);
        return 0;
    }
    if (addy == 0x11100000) {            // SYSCON
        core->pc += 4;
        return 1;                        // non-zero -> step returns `val` to main loop
    }
    return 0;
}

static uint32_t HandleControlLoad(uint32_t addy) {
    if (addy == 0x10000005) return 0x60; // UART LSR: THR empty + TEMT, always ready
    return 0;
}

static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value) {
    (void)image; (void)csrno; (void)value;
}
static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno) {
    (void)image; (void)csrno;
    return 0;
}

// Minimal ELF32 loader: copy PT_LOAD segments to (p_paddr - 0x80000000).
static uint32_t load_elf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "runner: cannot open %s\n", path); exit(2); }
    Elf32_Ehdr eh;
    if (fread(&eh, sizeof(eh), 1, f) != 1 || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "runner: %s is not an ELF\n", path); exit(2);
    }
    for (int i = 0; i < eh.e_phnum; i++) {
        Elf32_Phdr ph;
        fseek(f, eh.e_phoff + (long)i * eh.e_phentsize, SEEK_SET);
        if (fread(&ph, sizeof(ph), 1, f) != 1) break;
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        uint32_t off = ph.p_paddr - MINIRV32_RAM_IMAGE_OFFSET;
        if ((uint64_t)off + ph.p_memsz > ram_amt) {
            fprintf(stderr, "runner: segment 0x%x out of RAM\n", ph.p_paddr); exit(2);
        }
        if (ph.p_filesz) {
            fseek(f, ph.p_offset, SEEK_SET);
            if (fread(ram_image + off, ph.p_filesz, 1, f) != 1) {
                fprintf(stderr, "runner: short read on segment\n"); exit(2);
            }
        }
    }
    fclose(f);
    return eh.e_entry;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <elf>\n", argv[0]); return 2; }

    ram_image = calloc(1, ram_amt);
    if (!ram_image) { fprintf(stderr, "runner: OOM\n"); return 2; }

    uint32_t entry = load_elf(argv[1]);

    core = (struct MiniRV32IMAState *)(ram_image + ram_amt - sizeof(struct MiniRV32IMAState));
    memset(core, 0, sizeof(*core));
    core->pc = entry;
    core->regs[10] = 0;          // hart id
    core->regs[11] = 0;          // no DTB
    core->extraflags |= 3;       // start in machine mode

    const uint64_t MAX_INSTRS = 256ull * 1024 * 1024;
    for (uint64_t done = 0; done < MAX_INSTRS; done += 1024) {
        int ret = MiniRV32IMAStep(core, ram_image, 0, 0, 1024);
        switch (ret) {
            case 0: break;                 // ran a batch, keep going
            case 1: core->cyclel += 1024; break;  // WFI: just advance time
            case 0x5555: return 0;         // SYSCON pass code -> PASS
            default:                       // any other SYSCON value (incl. HALT_FAIL)
                return 1;                  // -> FAIL
        }
    }
    fprintf(stderr, "runner: instruction cap reached without HALT\n");
    return 2;
}
