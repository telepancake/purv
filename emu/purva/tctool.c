/*
 * tctool.c - the standalone transcode tool: RV32IM ELF -> purva image.
 *
 *   transcode <in.elf> <out.img>
 *
 * `in.elf` must be linked with --emit-relocs (see ../purva.ld and the Makefile) --
 * this tool needs the kept relocations to find every code address stored as a data
 * word (vtable slots, function-pointer globals).
 *
 * Sections are gathered by ELF FLAGS, not by name: code = executable PROGBITS,
 * rodata = read-only PROGBITS, rwdata = writable PROGBITS, bss = writable NOBITS.
 * Same-category sections are merged by address with zero-filled gaps -- deliberately
 * not name-based, since purva's own examples merge everything into one ".text", but
 * the ACT conformance linker script (act/link.ld) keeps ".text.init" and ".text" as
 * two separate sections and has no .rodata rule at all (orphan sections land
 * wherever the linker's default placement puts them).
 *
 * Each category's merge preserves its REAL starting address relative to a fixed
 * boundary (code at 0, rodata right after code, rwdata at RISCV_HALF, bss right
 * after rwdata) by zero-padding any alignment gap rather than assuming none. This
 * matters: a pc-relative `la` of a rodata symbol, or an absolute pointer into bss,
 * was encoded by the linker against the REAL address: if our image silently closed
 * a real gap, every such reference would land on the wrong byte.
 *
 * What it does, in order:
 *   1. gather code/rodata/rwdata/bss by flags (exact sizes, no name heuristics);
 *   2. transcode the merged code bytes into the packed op array (transcode.c) --
 *      the ONLY decoding that happens anywhere in this pipeline;
 *   3. scan every .rela.* section for R_RISCV_32 relocations whose value falls
 *      inside the code window: each one is a code address stored as a plain data
 *      word (the standard "vtable slot" / function-pointer-global pattern -- e.g.
 *      SQLite alone has ~1300 such call sites). Resolve it through the op-index
 *      map and overwrite the word in our rodata/rwdata buffer with that op index
 *      (the same "fake pc" units jal already produces), so a jalr that later loads
 *      and jumps through it needs no runtime translation at all;
 *   4. write the image (image.h): header + code + rodata + rwdata. No ELF, no
 *      symbols, no entry field (entry is implicit 0 -- see image.h).
 *
 * Symbols are NOT carried into the image (the engine has no notion of them, same
 * as purv); a caller that needs --invoke or signature-bound resolution keeps the
 * original ELF around and resolves names from it directly, never from the image.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transcode.h"
#include "image.h"

#define RISCV_HALF 0x80000000u

/* ---- minimal ELF32 reader: sections, symbols, relocations ---- */
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version,
    e_entry, e_phoff, e_shoff, e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
    e_shentsize, e_shnum, e_shstrndx; } Ehdr;
typedef struct { uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
    sh_link, sh_info, sh_addralign, sh_entsize; } Shdr;
typedef struct { uint32_t st_name, st_value, st_size; uint8_t st_info, st_other;
    uint16_t st_shndx; } Sym;
typedef struct { uint32_t r_offset, r_info, r_addend; } Rela;
#define SHT_PROGBITS  1
#define SHT_NOBITS    8
#define SHT_RELA      4
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define R_RISCV_32    1

static uint8_t *g_elf;

static const Shdr *shdr(const Ehdr *eh, int i) {
    return (const Shdr *)(g_elf + eh->e_shoff + (size_t)i * eh->e_shentsize);
}

typedef struct { uint32_t addr, size; const uint8_t *bytes; } Span;

static void sort_spans(Span *s, int n) {
    for (int i = 1; i < n; i++) { Span v = s[i]; int j = i - 1;
        while (j >= 0 && s[j].addr > v.addr) { s[j + 1] = s[j]; j--; } s[j + 1] = v; }
}

/* Merge every matching ALLOC PROGBITS section (exec/write flags as given) into one
 * buffer starting at `base`: a real gap between `base` and the lowest matching
 * section (or between sections) is zero-filled, not closed, so every byte keeps its
 * real relative address. Errors if a matching section starts before `base`
 * (overlap/corruption). *len is the merged extent (0 if nothing matched). */
static uint8_t *merge_progbits(const Ehdr *eh, int want_exec, int want_write, uint32_t base, uint32_t *len) {
    Span spans[64]; int n = 0;
    for (int i = 0; i < eh->e_shnum && n < 64; i++) {
        const Shdr *sh = shdr(eh, i);
        if (sh->sh_type != SHT_PROGBITS || !(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) continue;
        int is_exec = (sh->sh_flags & SHF_EXECINSTR) != 0, is_write = (sh->sh_flags & SHF_WRITE) != 0;
        if (is_exec != want_exec || is_write != want_write) continue;
        spans[n++] = (Span){ sh->sh_addr, sh->sh_size, g_elf + sh->sh_offset };
    }
    if (n == 0) { *len = 0; return NULL; }
    sort_spans(spans, n);
    if (spans[0].addr < base) {
        fprintf(stderr, "transcode: section at 0x%x overlaps the previous region (base 0x%x)\n", spans[0].addr, base);
        exit(2);
    }
    uint32_t end = spans[n - 1].addr + spans[n - 1].size;
    uint8_t *buf = calloc(1, end - base);
    for (int i = 0; i < n; i++) memcpy(buf + (spans[i].addr - base), spans[i].bytes, spans[i].size);
    *len = end - base;
    return buf;
}

/* Highest address any writable NOBITS (.bss-like) section reaches, or `base` if
 * there is none. Used the same way as merge_progbits's `end`, but with no bytes to
 * copy -- the image only needs a size, zero-filled at load time. */
static uint32_t nobits_top(const Ehdr *eh, uint32_t base) {
    uint32_t top = base;
    for (int i = 0; i < eh->e_shnum; i++) {
        const Shdr *sh = shdr(eh, i);
        if (sh->sh_type != SHT_NOBITS || !(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) continue;
        if (sh->sh_addr < base) { fprintf(stderr, "transcode: bss section at 0x%x before base 0x%x\n", sh->sh_addr, base); exit(2); }
        uint32_t end = sh->sh_addr + sh->sh_size;
        if (end > top) top = end;
    }
    return top;
}

/* Patch every R_RISCV_32 relocation in section `ri` whose target value lands inside
 * the code window [0, code_len): overwrite the word at its (absolute address -
 * buf_base) in `buf` with its resolved op index * 4. */
static void patch_rela(const Ehdr *eh, int ri, uint32_t buf_base, uint8_t *buf, uint32_t buf_len,
                        const uint32_t *map, uint32_t code_len) {
    const Shdr *rsh = shdr(eh, ri);
    const Shdr *symsh = shdr(eh, rsh->sh_link);
    const Sym  *syms = (const Sym *)(g_elf + symsh->sh_offset);
    const Rela *relas = (const Rela *)(g_elf + rsh->sh_offset);
    uint32_t n = rsh->sh_size / sizeof(Rela);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t type = relas[i].r_info & 0xff, sym_idx = relas[i].r_info >> 8;
        if (type != R_RISCV_32) continue;
        uint32_t target = syms[sym_idx].st_value + relas[i].r_addend;
        if (target >= code_len) continue;                  /* not a code address */
        uint32_t op_index = map[target >> 1];
        if (op_index == TC_SENTINEL) continue;              /* not an instruction start */
        if (relas[i].r_offset < buf_base || relas[i].r_offset - buf_base + 4 > buf_len) continue;
        uint32_t at = relas[i].r_offset - buf_base;
        uint32_t new_value = op_index * 4;                  /* fake-pc units, like jal's link */
        buf[at] = (uint8_t)new_value; buf[at + 1] = (uint8_t)(new_value >> 8);
        buf[at + 2] = (uint8_t)(new_value >> 16); buf[at + 3] = (uint8_t)(new_value >> 24);
    }
}

/* Patch code-pointer relocations in every .rela.X section whose target X is the
 * rodata or rwdata buffer (matched by address range, not by name). */
static void patch_all_relas(const Ehdr *eh, uint32_t rodata_base, uint8_t *rodata, uint32_t rodata_len,
                             uint32_t rwdata_base, uint8_t *rwdata, uint32_t rwdata_len,
                             const uint32_t *map, uint32_t code_len) {
    for (int i = 0; i < eh->e_shnum; i++) {
        const Shdr *sh = shdr(eh, i);
        if (sh->sh_type != SHT_RELA) continue;
        const Shdr *target = shdr(eh, sh->sh_info);     /* section this .rela.X applies to */
        if (rodata_len && target->sh_addr >= rodata_base && target->sh_addr < rodata_base + rodata_len)
            patch_rela(eh, i, rodata_base, rodata, rodata_len, map, code_len);
        else if (rwdata_len && target->sh_addr >= rwdata_base && target->sh_addr < rwdata_base + rwdata_len)
            patch_rela(eh, i, rwdata_base, rwdata, rwdata_len, map, code_len);
    }
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <in.elf> <out.img>\n", argv[0]); return 2; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "transcode: cannot read %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long flen = ftell(f); fseek(f, 0, SEEK_SET);
    g_elf = malloc((size_t)flen);
    if (fread(g_elf, 1, (size_t)flen, f) != (size_t)flen) { fprintf(stderr, "transcode: short read\n"); return 2; }
    fclose(f);

    const Ehdr *eh = (const Ehdr *)g_elf;
    if (memcmp(eh->e_ident, "\177ELF", 4) != 0) { fprintf(stderr, "transcode: not an ELF\n"); return 2; }
    if (eh->e_entry != 0) {
        fprintf(stderr, "transcode: entry must be 0 (got 0x%x) -- link so the entry "
                "point's section is placed first\n", eh->e_entry);
        return 2;
    }

    /* Each category's real base relative to a fixed boundary; a gap before it (e.g.
     * alignment padding between .text and .rodata) is zero-filled, not closed, so
     * every pc-relative / absolute reference baked by the linker still lands right. */
    uint32_t code_len;
    uint8_t *code_bytes = merge_progbits(eh, 1, 0, 0, &code_len);          /* exec, base 0 */
    if (!code_bytes) { fprintf(stderr, "transcode: no executable section\n"); return 2; }

    uint32_t rodata_len;
    uint8_t *rodata_bytes = merge_progbits(eh, 0, 0, code_len, &rodata_len);   /* base = right after code */

    uint32_t rwdata_len;
    uint8_t *rwdata_bytes = merge_progbits(eh, 0, 1, RISCV_HALF, &rwdata_len); /* base = RISCV_HALF */

    uint32_t bss_len = nobits_top(eh, RISCV_HALF + rwdata_len) - (RISCV_HALF + rwdata_len);

    /* 1. transcode code -- the only decoding in this pipeline. */
    Transcoded prog;
    transcode(code_bytes, code_len, &prog);

    /* Rebuild the map for the patcher (transcode() consumed its own internal copy).
     * Cheap: one pass over code, done once, at build time. */
    uint32_t n_ops_check;
    uint32_t *map = transcode_map(code_bytes, code_len, &n_ops_check);

    /* 2. patch every code-pointer relocation found anywhere into rodata/rwdata. */
    patch_all_relas(eh, code_len, rodata_bytes, rodata_len,
                     RISCV_HALF, rwdata_bytes, rwdata_len, map, code_len);

    Image img = {0};
    img.code_size   = prog.n_ops * 4;
    img.rodata_size = rodata_len;
    img.rwdata_size = rwdata_len;
    img.bss_size    = bss_len;
    img.stack_size  = 64u * 1024;          /* minimum; the host may map more */
    img.code   = (uint8_t *)prog.ops;
    img.rodata = rodata_bytes ? rodata_bytes : malloc(1);
    img.rwdata = rwdata_bytes ? rwdata_bytes : malloc(1);

    if (image_write(argv[2], &img) != 0) { fprintf(stderr, "transcode: cannot write %s\n", argv[2]); return 1; }

    fprintf(stderr, "transcode: %s -> %s  (%u ops, rodata=%u, rwdata=%u, bss>=%u)\n",
            argv[1], argv[2], prog.n_ops, rodata_len, rwdata_len, bss_len);
    free(map);
    return 0;
}
