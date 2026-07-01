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
 * Each category's merge preserves its REAL starting address by zero-padding any
 * alignment gap rather than assuming none. This matters: a pc-relative `la` of a
 * rodata symbol, or an absolute pointer into bss, was encoded by the linker
 * against the REAL address: if our image silently closed a real gap, every such
 * reference would land on the wrong byte. code is fixed at 0 and rwdata at
 * RISCV_HALF (every purva linker script agrees on both); rodata is READ BACK from
 * wherever the linker actually placed it (purva.ld: growing DOWN from 0, so it
 * ends exactly at 2^32 and lives at small negative addresses -- see purv.h's
 * two-cluster layout and merge_progbits's MERGE_AUTO_BASE) rather than assumed, so
 * the image never needs to (and never does) move it relative to what auipc-computed
 * or relocation-held pointers into it were baked against.
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
 *      (op_index*4 -- the pc, the same units jal already produces), so a jalr that later loads
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
#define R_RISCV_32            1
#define R_RISCV_PCREL_HI20    23  /* auipc's own contribution to a symbol+addend target */
#define R_RISCV_PCREL_LO12_I  24  /* addi/jalr/load completing it -- symbol is the HI20's own address */
#define R_RISCV_PCREL_LO12_S  25  /* store completing it, same symbol convention as LO12_I */

static uint8_t *g_elf;

static const Shdr *shdr(const Ehdr *eh, int i) {
    return (const Shdr *)(g_elf + eh->e_shoff + (size_t)i * eh->e_shentsize);
}

typedef struct { uint32_t addr, size; const uint8_t *bytes; } Span;

static void sort_spans(Span *s, int n) {
    for (int i = 1; i < n; i++) { Span v = s[i]; int j = i - 1;
        while (j >= 0 && s[j].addr > v.addr) { s[j + 1] = s[j]; j--; } s[j + 1] = v; }
}

#define MERGE_AUTO_BASE 0xffffffffu   /* merge_progbits: use the lowest matching section's own
                                        * real address as base, whatever the linker chose --
                                        * see its comment on why rodata needs this. */

/* Merge every matching ALLOC PROGBITS section (exec/write flags as given) into one
 * buffer starting at `base`: a real gap between `base` and the lowest matching
 * section (or between sections) is zero-filled, not closed, so every byte keeps its
 * real relative address. Errors if a matching section starts before `base`
 * (overlap/corruption). *len is the merged extent (0 if nothing matched); *out_base
 * reports the base actually used (== `base`, unless it was MERGE_AUTO_BASE).
 *
 * `base == MERGE_AUTO_BASE` skips the fixed-base assertion and instead uses the
 * lowest matching section's OWN real address as base (so there is no gap to
 * fill -- the buffer starts exactly where the linker put the content). This is
 * for rodata specifically: unlike code (always 0) and rwdata (always
 * RISCV_HALF, both fixed architectural boundaries every purva linker script
 * agrees on), rodata's real link-time address depends on the linker script (see
 * purva.ld: placed to grow down from 0, so it ends at 2^32 and sits at small
 * negative addresses, matching purv.h's two-cluster layout) -- and whatever it
 * is, that is what auipc-computed pointers into it were baked against, so the
 * image must preserve it exactly, not assume a fixed value or shift it to match
 * anything transcode-time. */
static uint8_t *merge_progbits(const Ehdr *eh, int want_exec, int want_write, uint32_t base,
                                uint32_t *len, uint32_t *out_base) {
    Span spans[64]; int n = 0;
    for (int i = 0; i < eh->e_shnum && n < 64; i++) {
        const Shdr *sh = shdr(eh, i);
        if (sh->sh_type != SHT_PROGBITS || !(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) continue;
        int is_exec = (sh->sh_flags & SHF_EXECINSTR) != 0, is_write = (sh->sh_flags & SHF_WRITE) != 0;
        if (is_exec != want_exec || is_write != want_write) continue;
        spans[n++] = (Span){ sh->sh_addr, sh->sh_size, g_elf + sh->sh_offset };
    }
    if (n == 0) { *len = 0; if (out_base) *out_base = base == MERGE_AUTO_BASE ? 0 : base; return NULL; }
    sort_spans(spans, n);
    if (base == MERGE_AUTO_BASE) {
        base = spans[0].addr;
    } else if (spans[0].addr < base) {
        fprintf(stderr, "transcode: section at 0x%x overlaps the previous region (base 0x%x)\n", spans[0].addr, base);
        exit(2);
    }
    uint32_t end = spans[n - 1].addr + spans[n - 1].size;
    uint8_t *buf = calloc(1, end - base);
    for (int i = 0; i < n; i++) memcpy(buf + (spans[i].addr - base), spans[i].bytes, spans[i].size);
    *len = end - base;
    if (out_base) *out_base = base;
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

/* Every R_RISCV_32 or R_RISCV_PCREL_HI20 relocation anywhere in the file whose
 * target value lands inside the code window: every code address taken as a
 * value ANYWHERE in the program -- a vtable slot, a stored function pointer, an
 * auipc materializing a function pointer to store as data -- and therefore a
 * possible jalr target. This is the set transcode_ex's fusion pass must never
 * hide a second half inside (see transcode.h's SPILL2 contract); unlike
 * patch_all_relas, it does not matter which section the relocation lives in,
 * only what it points AT. Caller frees. */
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
            uint32_t type = relas[j].r_info & 0xff;
            if (type != R_RISCV_32 && type != R_RISCV_PCREL_HI20) continue;
            uint32_t target = syms[relas[j].r_info >> 8].st_value + relas[j].r_addend;
            if (target >= code_len) continue;
            if (n == cap) { cap = cap ? cap * 2 : 64; out = realloc(out, cap * sizeof *out); }
            out[n++] = target;
        }
    }
    *n_out = n;
    return out;
}

/* auipc+addi (or +load/store) materializing a CODE address into a register, to
 * be used as data rather than jumped to immediately (a function pointer stored
 * into a struct field, not a link-time relocation -- collect_code_address_
 * relocs/patch_rela already cover that case). --emit-relocs keeps the pair this
 * was resolved from: R_RISCV_PCREL_HI20 on the auipc, and a paired
 * R_RISCV_PCREL_LO12_I (addi/jalr/load) or _S (store) on the completing
 * instruction, whose symbol points back at the auipc's own address rather than
 * at the target (the standard local-label convention).
 *
 * What needs re-encoding is the pc-to-pc displacement between the auipc and the
 * target -- both are op-index*4 (transcode.h has the argument for treating that
 * as simply "the pc"), and neither is known until the map exists, so this just
 * collects the pairs; apply_pcrel_code_fixups does the actual re-encoding.
 *
 * `sym_value` and `addend` are kept separate rather than pre-summed: an addend
 * can be a small negative offset (`.Ltmp7 - 1`, seen in ACT's I-jalr-01, to test
 * jalr masking the low bit of a computed target), landing the sum off an
 * instruction boundary. The symbol alone is always aligned; the addend is added
 * back after resolving it, since a byte offset means the same thing before and
 * after transcoding. Caller frees. */
typedef struct { uint32_t hi_off, lo_off, sym_value; int32_t addend; int lo_is_store; } PcrelFixup;

static PcrelFixup *collect_pcrel_code_fixups(const Ehdr *eh, uint32_t code_len, uint32_t *n_out) {
    PcrelFixup *out = NULL; uint32_t n = 0, cap = 0;
    for (int i = 0; i < eh->e_shnum; i++) {
        const Shdr *sh = shdr(eh, i);
        if (sh->sh_type != SHT_RELA) continue;
        const Shdr *symsh = shdr(eh, sh->sh_link);
        const Sym  *syms = (const Sym *)(g_elf + symsh->sh_offset);
        const Rela *relas = (const Rela *)(g_elf + sh->sh_offset);
        uint32_t cnt = sh->sh_size / sizeof(Rela);
        for (uint32_t j = 0; j < cnt; j++) {
            if ((relas[j].r_info & 0xff) != R_RISCV_PCREL_HI20) continue;
            uint32_t sym_value = syms[relas[j].r_info >> 8].st_value;
            int32_t addend = relas[j].r_addend;
            if (sym_value >= code_len) continue;                /* not a code address; the
                                                                    * existing unmodified-real-
                                                                    * address bake is already
                                                                    * correct for these */
            uint32_t hi_off = relas[j].r_offset;
            /* One auipc's result can feed more than one lo12-completing
             * instruction (e.g. auipc t0,..; lw a,..(t0); sw b,..(t0)) -- every
             * LO12 relocation whose symbol points back at this hi_off needs its
             * own fixup entry, not just the first one found. */
            int found = 0;
            for (int k = 0; k < eh->e_shnum; k++) {
                const Shdr *sh2 = shdr(eh, k);
                if (sh2->sh_type != SHT_RELA) continue;
                const Shdr *symsh2 = shdr(eh, sh2->sh_link);
                const Sym  *syms2 = (const Sym *)(g_elf + symsh2->sh_offset);
                const Rela *relas2 = (const Rela *)(g_elf + sh2->sh_offset);
                uint32_t cnt2 = sh2->sh_size / sizeof(Rela);
                for (uint32_t l = 0; l < cnt2; l++) {
                    uint32_t type2 = relas2[l].r_info & 0xff;
                    if (type2 != R_RISCV_PCREL_LO12_I && type2 != R_RISCV_PCREL_LO12_S) continue;
                    if (syms2[relas2[l].r_info >> 8].st_value != hi_off) continue;
                    found = 1;
                    if (n == cap) { cap = cap ? cap * 2 : 64; out = realloc(out, cap * sizeof *out); }
                    out[n++] = (PcrelFixup){ hi_off, relas2[l].r_offset, sym_value, addend,
                                              type2 == R_RISCV_PCREL_LO12_S };
                }
            }
            if (!found) {
                fprintf(stderr, "transcode: auipc at 0x%x (code target 0x%x+%d) has no paired "
                        "PCREL_LO12 relocation -- cannot fix up\n", hi_off, sym_value, addend);
                exit(2);
            }
        }
    }
    *n_out = n;
    return out;
}

/* Re-encode each pair for the op-space displacement between the auipc (at its own op
 * index at_hi) and the target (at_sym, resolved through the same map patch_rela uses):
 * a standard hi20/lo12 split against at_hi*4. The runtime auipc adds uimm<<12 to its
 * live op CURSOR (== at_hi*4), so this is correct however much fusion drifted the stream
 * -- the displacement is measured in the same op space the cursor walks. */
static void apply_pcrel_code_fixups(const PcrelFixup *fx, uint32_t n, const uint32_t *map, uint32_t *ops) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t at_hi = map[fx[i].hi_off >> 1], at_lo = map[fx[i].lo_off >> 1],
                 at_sym = map[fx[i].sym_value >> 1];
        if (at_hi == TC_SENTINEL || at_lo == TC_SENTINEL || at_sym == TC_SENTINEL) {
            fprintf(stderr, "transcode: pcrel fixup at 0x%x/0x%x -> 0x%x+%d: unresolved instruction start\n",
                    fx[i].hi_off, fx[i].lo_off, fx[i].sym_value, fx[i].addend);
            exit(2);
        }
        /* try_fuse_spill2 only ever fuses a pair of sw -- an addi/load/jalr lo12
         * can't become one, so only check when it's a store. ext_addrs already
         * stops it being hidden as a fusion's second half; nothing stops it
         * becoming a fusion's first half (try_fuse_spill2 never checks that), so
         * this catches that case before writing into the wrong field. */
        if (fx[i].lo_is_store && TC_OP(ops[at_lo]) != RISCV_OP_SW) {
            fprintf(stderr, "transcode: pcrel fixup at 0x%x: lo12 store at 0x%x got fused\n", fx[i].hi_off, fx[i].lo_off);
            exit(2);
        }
        int32_t target_pc = (int32_t)(at_sym * 4) + fx[i].addend;   /* an addend is just an offset, same in either pc space */
        int32_t delta = target_pc - (int32_t)(at_hi * 4);
        uint8_t hi_op = TC_OP(ops[at_hi]);
        if (hi_op != RISCV_OP_AUIPC) {
            fprintf(stderr, "transcode: pcrel fixup at 0x%x: op %u is not an auipc\n", fx[i].hi_off, hi_op);
            exit(2);
        }
        int32_t hi20 = (delta + 0x800) >> 12, lo12 = delta - (hi20 << 12);
        ops[at_hi] = (ops[at_hi] & ~0xfffffu) | ((uint32_t)hi20 & 0xfffffu);
        ops[at_lo] = (ops[at_lo] & ~0xffffu) | ((uint32_t)lo12 & 0xffffu);
    }
}

/* Any auipc that materialises a DATA address is a load-immediate (the address is a
 * fixed constant -- code is non-relocatable), so it should be an LI_LO/LI_HI, not an
 * auipc that the runtime evaluates cursor-relative (that value drifts with fusion and
 * there is no code fixup to correct a data auipc). transcode.c's step() rewrites the
 * ones it can classify by value alone, but leaves a bare auipc for the few whose value
 * sits in the ~2KB around 0 a code address could also occupy. Here -- with the exact
 * symbol from the PCREL_HI20 relocation, so no ambiguity -- rewrite every remaining
 * data auipc to the LI that loads its own constant `val`; its paired lo12 still adds
 * its offset. (A data auipc step() already rewrote is not an auipc here, so skipped.) */
static void resolve_data_auipc(const Ehdr *eh, uint32_t code_len, const uint32_t *map, uint32_t *ops) {
    for (int i = 0; i < eh->e_shnum; i++) {
        const Shdr *sh = shdr(eh, i);
        if (sh->sh_type != SHT_RELA) continue;
        const Shdr *symsh = shdr(eh, sh->sh_link);
        const Sym  *syms = (const Sym *)(g_elf + symsh->sh_offset);
        const Rela *relas = (const Rela *)(g_elf + sh->sh_offset);
        uint32_t cnt = sh->sh_size / sizeof(Rela);
        for (uint32_t j = 0; j < cnt; j++) {
            if ((relas[j].r_info & 0xff) != R_RISCV_PCREL_HI20) continue;
            if (syms[relas[j].r_info >> 8].st_value < code_len) continue;   /* code auipc: fixed elsewhere */
            uint32_t hi_off = relas[j].r_offset, at = map[hi_off >> 1];
            if (at == TC_SENTINEL || TC_OP(ops[at]) != RISCV_OP_AUIPC) continue;   /* already an LI, or not a slot start */
            uint32_t rd = (ops[at] >> 21) & 31, val = hi_off + ((ops[at] & 0xfffffu) << 12);
            int32_t s0 = (int32_t)val, sh_ = (int32_t)(val - RISCV_HALF);
            uint8_t op; uint32_t imm;
            if (s0 > -(1 << 20) && s0 < (1 << 20))         { op = RISCV_OP_LI_LO; imm = val & 0x1fffffu; }
            else if (sh_ > -(1 << 20) && sh_ < (1 << 20))  { op = RISCV_OP_LI_HI; imm = (val - RISCV_HALF) & 0x1fffffu; }
            else {
                fprintf(stderr, "transcode: data auipc at 0x%x -> 0x%x is >1MiB from both 0 and "
                        "RISCV_HALF; too far for a 21-bit load-immediate\n", hi_off, val);
                exit(2);
            }
            ops[at] = ((uint32_t)op << 26) | rd << 21 | imm;
        }
    }
}

/* Patch every R_RISCV_32 relocation in section `ri` whose target value lands inside
 * the code window [0, code_len): overwrite the word at its (absolute address -
 * buf_base) in `buf` with its resolved op index * 4. A relocation targeting rodata
 * or rwdata needs no patching at all: both regions keep their linked address
 * unmodified in the image (rwdata is always at the fixed RISCV_HALF boundary;
 * rodata is wherever the linker put it -- see merge_progbits's MERGE_AUTO_BASE
 * comment -- and the image preserves that address exactly), so a relocation's raw
 * linked value is already correct and is left as-is. */
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
        uint32_t new_value = op_index * 4;                  /* the pc, same as jal's link */
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
        /* Relative unsigned tests: rodata now grows down to end EXACTLY at 2^32, so
         * rodata_base + rodata_len wraps to 0 -- a `sh_addr < base + len` form would
         * be false for every section. `sh_addr - base < len` is wrap-safe. */
        if (rodata_len && (uint32_t)(target->sh_addr - rodata_base) < rodata_len)
            patch_rela(eh, i, rodata_base, rodata, rodata_len, map, code_len);
        else if (rwdata_len && (uint32_t)(target->sh_addr - rwdata_base) < rwdata_len)
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
     * every pc-relative / absolute reference baked by the linker still lands right.
     * code (base 0) and rwdata (base RISCV_HALF) are fixed architectural boundaries
     * every purva linker script agrees on. rodata's is too -- 0 minus its own
     * length, purv.h's formula for read-only data that grows down from 0 to end
     * exactly at 2^32 (purva.ld places .rodata there for exactly this reason: it
     * sits at small negative addresses just below code, so the engine can compute
     * its base the same way at run time with no extra state -- see purva.c's
     * mem_xlate). That base isn't known until rodata_len is (its length is exactly
     * what merge_progbits computes), so MERGE_AUTO_BASE reads the real address back
     * from the ELF instead of assuming it, then this asserts the linker actually
     * produced purv.h's layout rather than silently trusting an arbitrary address
     * (which mem_xlate's fixed formula would then get wrong at run time). */
    uint32_t code_len;
    uint8_t *code_bytes = merge_progbits(eh, 1, 0, 0, &code_len, NULL);          /* exec, base 0 */
    if (!code_bytes) { fprintf(stderr, "transcode: no executable section\n"); return 2; }

    uint32_t rodata_len, rodata_base;
    uint8_t *rodata_bytes = merge_progbits(eh, 0, 0, MERGE_AUTO_BASE, &rodata_len, &rodata_base);
    if (rodata_len && rodata_base != 0u - rodata_len) {
        fprintf(stderr, "transcode: rodata at 0x%x, expected 0x%x (0 - rodata_len) --\n"
                "  the linker script must place rodata to grow down from 0 (the top of\n"
                "  the read-only region), like purva.ld; base is derived from length.\n",
                rodata_base, 0u - rodata_len);
        return 2;
    }

    uint32_t rwdata_len;
    uint8_t *rwdata_bytes = merge_progbits(eh, 0, 1, RISCV_HALF, &rwdata_len, NULL); /* base = RISCV_HALF */

    uint32_t bss_len = nobits_top(eh, RISCV_HALF + rwdata_len) - (RISCV_HALF + rwdata_len);

    /* Every code address taken as a value anywhere in the file: the fusion pass's
     * required input (transcode.h's SPILL2 contract) -- a bare transcode() call has
     * no way to know these (it never reads the ELF), only this tool does. */
    uint32_t n_ext;
    uint32_t *ext_addrs = collect_code_address_relocs(eh, code_len, &n_ext);

    /* auipc+addi/load/store pairs materializing a code address as data (see
     * collect_pcrel_code_fixups) -- their lo12-completing instruction must also
     * keep its own op slot (apply_pcrel_code_fixups patches its imm[15:0]
     * directly, which a SPILL2 fusion would silently repurpose as a different
     * field), so its address joins the SAME fusion-protected set. */
    uint32_t n_fixups;
    PcrelFixup *fixups = collect_pcrel_code_fixups(eh, code_len, &n_fixups);
    if (n_fixups) {
        ext_addrs = realloc(ext_addrs, (n_ext + n_fixups) * sizeof *ext_addrs);
        for (uint32_t i = 0; i < n_fixups; i++) ext_addrs[n_ext + i] = fixups[i].lo_off;
        n_ext += n_fixups;
    }
    TcExternalTargets ext = { ext_addrs, n_ext };

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
            at += 1;                                  /* every emitted op is one word */
        }
    }

    /* 2. patch every code-pointer relocation found anywhere into rodata/rwdata.
     * rwdata needs no correcting: it keeps its linked address unmodified in
     * the image (see merge_progbits), so a relocation pointing at it is never
     * wrong to begin with. rodata is the same UNLESS a relocation's target is
     * actually a code address stored there (a vtable in rodata, still R_RISCV_32
     * -- patch_rela already resolves those through the map like any other code-
     * address relocation, regardless of which section holds the word). */
    patch_all_relas(eh, rodata_base, rodata_bytes, rodata_len,
                     RISCV_HALF, rwdata_bytes, rwdata_len, map, code_len);

    /* 3. re-encode every auipc+addi/load/store pair that materializes a code
     * address as data (see collect_pcrel_code_fixups) for the pc-to-pc
     * displacement it actually needs, now that the map exists. */
    apply_pcrel_code_fixups(fixups, n_fixups, map, prog.ops);

    /* 4. rewrite any DATA auipc step() left bare (the near-zero ambiguous few) into the
     * load-immediate it really is, now that the relocation gives its exact symbol. */
    resolve_data_auipc(eh, code_len, map, prog.ops);

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
    free(fixups);
    return 0;
}
