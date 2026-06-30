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

/* Every R_RISCV_32 relocation anywhere in the file whose target value lands inside
 * the code window: every code address taken as a value ANYWHERE in the program --
 * a vtable slot, a stored function pointer -- and therefore a possible jalr target.
 * This is the set transcode_ex's fusion pass must never hide a second half inside
 * (see transcode.h's SPILL2 contract); unlike patch_all_relas, it does not matter
 * which section the relocation lives in, only what it points AT. Caller frees. */
static uint32_t *collect_code_address_relocs(const Ehdr *eh, uint32_t code_len, uint32_t *n_out) {
    uint32_t *out = NULL, n = 0, cap = 0;
    for (int i = 0; i < eh->e_shnum; i++) {
        const Shdr *sh = shdr(eh, i);
        if (sh->sh_type != SHT_RELA) continue;
        const Shdr *symsh = shdr(eh, sh->sh_link);
        const Sym  *syms = (const Sym *)(g_elf + symsh->sh_offset);
        const Rela *relas = (const Rela *)(g_elf + sh->sh_offset);
        uint32_t cnt = sh->sh_size / sizeof(Rela);
        for (uint32_t j = 0; j < cnt; j++) {
            if ((relas[j].r_info & 0xff) != R_RISCV_32) continue;
            uint32_t target = syms[relas[j].r_info >> 8].st_value + relas[j].r_addend;
            if (target >= code_len) continue;
            if (n == cap) { cap = cap ? cap * 2 : 64; out = realloc(out, cap * sizeof *out); }
            out[n++] = target;
        }
    }
    *n_out = n;
    return out;
}

/* Patch every R_RISCV_32 relocation in section `ri` whose target value is either a
 * CODE address or a RODATA address -- the two ways a data word anywhere (rodata,
 * rwdata, or a vtable slot in either) can store "the address of something" that
 * this image's layout doesn't put at the same byte offset as the original ELF:
 *
 *   - target < code_len: a code address (a function pointer, a vtable slot).
 *     Resolved through the op-index map and overwritten with op_index*4 (fake-pc
 *     units, like jal's link) -- unchanged from before this function grew a
 *     second case.
 *   - code_len <= target < code_len+rodata_len: a RODATA address (e.g. a `static
 *     const char *` initializer pointing at a string literal -- SQLite alone
 *     stores dozens of these in plain data words, not just instructions). The
 *     image places rodata at prog.n_ops*4, not code_len, once anything in the
 *     whole program has fused or expanded (see fixup_rodata_auipc below, which
 *     does the identical correction for values baked into the op array instead
 *     of into a data word) -- so the same `delta` shift applies here.
 *   - target >= code_len+rodata_len: an rwdata/heap pointer. Always left alone:
 *     rwdata sits at the fixed RISCV_HALF boundary in both the ELF and the
 *     image, independent of how the code transcoded, so no shift is needed (and
 *     this function is never even called with such a target in code_len's
 *     bucket -- the caller only reaches it for rodata/rwdata-hosted sections). */
static void patch_rela(const Ehdr *eh, int ri, uint32_t buf_base, uint8_t *buf, uint32_t buf_len,
                        const uint32_t *map, uint32_t code_len, uint32_t rodata_len, int64_t delta) {
    const Shdr *rsh = shdr(eh, ri);
    const Shdr *symsh = shdr(eh, rsh->sh_link);
    const Sym  *syms = (const Sym *)(g_elf + symsh->sh_offset);
    const Rela *relas = (const Rela *)(g_elf + rsh->sh_offset);
    uint32_t n = rsh->sh_size / sizeof(Rela);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t type = relas[i].r_info & 0xff, sym_idx = relas[i].r_info >> 8;
        if (type != R_RISCV_32) continue;
        uint32_t target = syms[sym_idx].st_value + relas[i].r_addend;
        uint32_t new_value;
        if (target < code_len) {
            uint32_t op_index = map[target >> 1];
            if (op_index == TC_SENTINEL) continue;          /* not an instruction start */
            new_value = op_index * 4;                        /* fake-pc units, like jal's link */
        } else if (target < code_len + rodata_len) {
            new_value = (uint32_t)((int64_t)target - delta); /* real ELF offset -> image offset */
        } else {
            continue;                                        /* rwdata/heap: no shift needed */
        }
        if (relas[i].r_offset < buf_base || relas[i].r_offset - buf_base + 4 > buf_len) continue;
        uint32_t at = relas[i].r_offset - buf_base;
        buf[at] = (uint8_t)new_value; buf[at + 1] = (uint8_t)(new_value >> 8);
        buf[at + 2] = (uint8_t)(new_value >> 16); buf[at + 3] = (uint8_t)(new_value >> 24);
    }
}

/* Patch code- and rodata-pointer relocations in every .rela.X section whose
 * target X is the rodata or rwdata buffer (matched by address range, not by
 * name). */
static void patch_all_relas(const Ehdr *eh, uint32_t rodata_base, uint8_t *rodata, uint32_t rodata_len,
                             uint32_t rwdata_base, uint8_t *rwdata, uint32_t rwdata_len,
                             const uint32_t *map, uint32_t code_len, int64_t delta) {
    for (int i = 0; i < eh->e_shnum; i++) {
        const Shdr *sh = shdr(eh, i);
        if (sh->sh_type != SHT_RELA) continue;
        const Shdr *target = shdr(eh, sh->sh_info);     /* section this .rela.X applies to */
        if (rodata_len && target->sh_addr >= rodata_base && target->sh_addr < rodata_base + rodata_len)
            patch_rela(eh, i, rodata_base, rodata, rodata_len, map, code_len, rodata_len, delta);
        else if (rwdata_len && target->sh_addr >= rwdata_base && target->sh_addr < rwdata_base + rwdata_len)
            patch_rela(eh, i, rwdata_base, rwdata, rwdata_len, map, code_len, rodata_len, delta);
    }
}

/* Correct every baked RISCV_OP_AUIPC_ABS target that points into rodata: step()
 * (transcode.c) bakes such a target unconditionally, in REAL-ELF coordinates,
 * because it can't yet know the whole-program delta between the original code's
 * byte length (`code_len`) and the transcoded code's (`prog.n_ops*4`) -- that
 * delta only exists once the whole pass is done. Now that it's done (the caller
 * computes it once and shares it with patch_rela's identical correction for
 * relocation-held rodata pointers), shift every baked target that lands in
 * [code_len, code_len+rodata_len) -- a rodata pointer -- by that delta,
 * converting it from "byte offset in the original ELF" to "byte offset in this
 * image" (where rodata starts at prog.n_ops*4, not code_len). A target >=
 * code_len+rodata_len is an rwdata/heap pointer instead: those need no
 * correction, since rwdata is always placed at the fixed RISCV_HALF boundary in
 * both the ELF and the image, independent of how code transcoded. */
static void fixup_rodata_auipc(uint32_t *ops, uint32_t n_ops, uint32_t code_len,
                                uint32_t rodata_len, int64_t delta) {
    if (delta == 0) return;
    for (uint32_t i = 0; i < n_ops; ) {
        if (TC_OP(ops[i]) == RISCV_OP_AUIPC_ABS) {
            uint32_t target = ops[i + 1];
            if (target >= code_len && target < code_len + rodata_len)
                ops[i + 1] = (uint32_t)((int64_t)target - delta);
            i += 2;
        } else {
            i += 1;
        }
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

    /* Every code address taken as a value anywhere in the file: the fusion pass's
     * required input (transcode.h's SPILL2 contract) -- a bare transcode() call has
     * no way to know these (it never reads the ELF), only this tool does. */
    uint32_t n_ext;
    uint32_t *ext_addrs = collect_code_address_relocs(eh, code_len, &n_ext);
    TcExternalTargets ext = { ext_addrs, n_ext, code_len + rodata_len };

    /* 1. transcode code -- the only decoding in this pipeline. */
    Transcoded prog;
    transcode_ex(code_bytes, code_len, &ext, &prog);

    /* Rebuild the map for the patcher (transcode_ex() consumed its own internal
     * copy) -- with the SAME ext targets, so it makes the identical fusion
     * decisions and the map matches what's actually in the op array. Cheap: one
     * pass over code, done once, at build time. */
    uint32_t n_ops_check;
    uint32_t *map = transcode_map_ex(code_bytes, code_len, &ext, &n_ops_check);
    if (getenv("TCVERIFY")) {  /* TEMP debug: independent map/ops consistency check */
        fprintf(stderr, "TCVERIFY: prog.n_ops=%u n_ops_check=%u %s\n",
                prog.n_ops, n_ops_check, prog.n_ops == n_ops_check ? "MATCH" : "MISMATCH");
        /* Independently replay map[off>>1] against a fresh walk of prog.ops and
         * report the first byte offset where they disagree. */
        uint32_t at = 0;
        for (uint32_t off = 0; off + 4 <= code_len; off += 4) {
            uint32_t mapped = map[off >> 1];
            if (mapped == TC_SENTINEL) continue;
            if (mapped != at) {
                fprintf(stderr, "TCVERIFY: first mismatch at off=0x%x: map=%u walked_at=%u\n", off, mapped, at);
                break;
            }
            uint8_t op = (uint8_t)(prog.ops[at] >> 26);
            at += (op == RISCV_OP_AUIPC_ABS) ? 2 : 1;
        }
    }

    /* The whole-program byte-length delta fusion/expansion introduced -- shared by
     * patch_rela (relocation-held rodata pointers) and fixup_rodata_auipc (baked
     * auipc rodata pointers); both correct the same real-ELF-vs-image mismatch. */
    int64_t delta = (int64_t)code_len - (int64_t)(prog.n_ops * 4);

    /* 2. patch every code- and rodata-pointer relocation found anywhere into
     * rodata/rwdata. */
    patch_all_relas(eh, code_len, rodata_bytes, rodata_len,
                     RISCV_HALF, rwdata_bytes, rwdata_len, map, code_len, delta);

    /* 3. correct every baked rodata-pointing auipc for the same delta. */
    fixup_rodata_auipc(prog.ops, prog.n_ops, code_len, rodata_len, delta);

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
    free(ext_addrs);
    return 0;
}
