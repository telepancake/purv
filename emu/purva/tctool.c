/*
 * tctool.c - the standalone transcode tool: RV32IM ELF -> .tc transcoded binary.
 *
 *   transcode <in.elf> <out.tc>
 *
 * Loads the ELF (reusing the host loader for the memory image and symbols),
 * transcodes the code image once (transcode.c), and writes a self-contained .tc the
 * purva evaluator runs directly. The tool is the ONLY thing that decodes RISC-V; the
 * evaluator never sees a raw instruction.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../purv.h"
#include "../purvhost.h"
#include "transcode.h"
#include "tcfile.h"

/* Highest lower-half (code+rodata) byte the image occupies, rounded to a 4-byte
 * instruction boundary: the transcoder's sweep bound. */
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

/* Collect every named symbol (value + name) from the ELF symbol table. */
static TcSym *collect_syms(const PurvHost *h, uint32_t *n_out) {
    PurvElf32_Ehdr eh; memcpy(&eh, h->elf, sizeof eh);
    TcSym *out = NULL; uint32_t n = 0, cap = 0;
    for (int i = 0; i < eh.e_shnum; i++) {
        PurvElf32_Shdr sh;
        memcpy(&sh, h->elf + eh.e_shoff + (long)i * eh.e_shentsize, sizeof sh);
        if (sh.sh_type != PURV_SHT_SYMTAB || sh.sh_entsize == 0) continue;
        PurvElf32_Shdr strsh;
        memcpy(&strsh, h->elf + eh.e_shoff + (long)sh.sh_link * eh.e_shentsize, sizeof strsh);
        const char *strs = (const char *)(h->elf + strsh.sh_offset);
        uint32_t cnt = sh.sh_size / sh.sh_entsize;
        for (uint32_t s = 0; s < cnt; s++) {
            PurvElf32_Sym sym;
            memcpy(&sym, h->elf + sh.sh_offset + (long)s * sh.sh_entsize, sizeof sym);
            if (!sym.st_name) continue;
            if (n == cap) { cap = cap ? cap * 2 : 256; out = realloc(out, cap * sizeof *out); }
            const char *nm = strs + sym.st_name;
            size_t l = strlen(nm) + 1;
            out[n].value = sym.st_value;
            out[n].name = memcpy(malloc(l), nm, l);
            n++;
        }
    }
    *n_out = n;
    return out;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <in.elf> <out.tc>\n", argv[0]); return 2; }

    PurvHost host;
    if (purvhost_alloc(&host, 0, 0, 0) != 0) return 2;
    purvhost_load_elf(&host, argv[1]);

    uint32_t code_len = lower_half_extent(&host);

    Transcoded prog;
    transcode(host.code, code_len, &prog);

    TcImage img;
    img.entry     = host.entry;
    img.code_len  = code_len;
    img.data_end  = host.data_end;
    img.n_ops     = prog.n_ops;
    img.udata_len = host.data_end > RISCV_HALF ? host.data_end - RISCV_HALF : 0;
    img.ops       = prog.ops;
    img.map       = prog.map;
    img.lower     = host.code;                 /* [0, code_len)            */
    img.udata     = host.heap;                 /* [RISCV_HALF, data_end)   */
    img.syms      = collect_syms(&host, &img.n_syms);

    if (tc_write(argv[2], &img) != 0) { fprintf(stderr, "transcode: cannot write %s\n", argv[2]); return 1; }

    fprintf(stderr, "transcode: %s -> %s  (%u ops, code_len=%u, %u syms)\n",
            argv[1], argv[2], img.n_ops, img.code_len, img.n_syms);
    return 0;
}
