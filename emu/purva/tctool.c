/*
 * tctool.c - the standalone transcode tool: RV32IM ELF -> .ops op array.
 *
 *   transcode <in.elf> <out.ops>
 *
 * The ONLY thing that decodes RISC-V. It reads the code image from the ELF, lowers
 * it to one packed op word per instruction (transcode.c), and writes that array --
 * nothing else. Memory, symbols, and loading are the host's job (it has the ELF);
 * the .ops file is pure code.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../purv.h"
#include "../purvhost.h"
#include "transcode.h"

/* Highest lower-half (code+rodata) byte the image occupies, rounded up to a 4-byte
 * instruction boundary: how much code to transcode. */
static uint32_t lower_half_extent(const PurvHost *h) {
    PurvElf32_Ehdr eh; memcpy(&eh, h->elf, sizeof eh);
    uint32_t end = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        PurvElf32_Phdr ph;
        memcpy(&ph, h->elf + eh.e_phoff + (long)i * eh.e_phentsize, sizeof ph);
        if (ph.p_type != PURV_PT_LOAD || ph.p_memsz == 0) continue;
        if (ph.p_vaddr < RISCV_HALF) { uint32_t e = ph.p_vaddr + ph.p_memsz; if (e > end) end = e; }
    }
    return (end + 3u) & ~3u;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <in.elf> <out.ops>\n", argv[0]); return 2; }

    PurvHost host;
    if (purvhost_alloc(&host, 0, 0, 0) != 0) return 2;
    purvhost_load_elf(&host, argv[1]);

    Transcoded prog;
    transcode(host.code, lower_half_extent(&host), &prog);

    FILE *f = fopen(argv[2], "wb");
    if (!f || fwrite(prog.ops, 4, prog.n_ops, f) != prog.n_ops) {
        fprintf(stderr, "transcode: cannot write %s\n", argv[2]); return 1;
    }
    fclose(f);
    fprintf(stderr, "transcode: %s -> %s  (%u ops)\n", argv[1], argv[2], prog.n_ops);
    return 0;
}
