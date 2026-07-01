/*
 * purvhost.h - reusable host scaffolding for the purv / purvs engines.
 *
 * Every host driver (main.c, compact/host.c, sqlite/host.c, ...) needs the same
 * setup before it can run anything: allocate the host buffers, parse and place an
 * image, and wire the engine's four regions. This header factors that out so the
 * setup/parsing is written once, not re-puzzled per host.
 *
 * The two halves a host actually wants:
 *   purvhost_setup_elf (h, st, path)   load an ELF32 RISC-V image, ready to run
 *   purvhost_setup_flat(h, st, path)   load a flat (objcopy -O binary) image
 * Each allocates the default-sized regions, loads the image, and Inits the state;
 * the host then assigns its trap handlers + memory callback and runs. For custom
 * region sizes, call the granular pieces (purvhost_alloc + purvhost_load_* +
 * purvhost_init) instead.
 *
 * Engine-agnostic: it uses only the public names both purv.h and purvs.h share
 * (RiscvEmulatorState_t, RiscvEmulatorRegion_t, RiscvEmulatorInit, RISCV_HALF, the
 * region enum). Include the engine header FIRST, then this one.
 */
#ifndef PURVHOST_H_
#define PURVHOST_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef RISCV_HALF
#error "include the engine header (purv.h or purvs.h) before purvhost.h"
#endif

/* Default region sizes; #define before including to override. `readonly` holds code
 * (text+rodata, read-only, also fetched) at 0; `writable` is one buffer holding the
 * stack (just below RISCV_HALF) then the heap (data+bss+malloc arena) from RISCV_HALF. */
#ifndef PURVHOST_CODE_BYTES
#define PURVHOST_CODE_BYTES   (64u * 1024 * 1024)
#endif
#ifndef PURVHOST_HEAP_BYTES
#define PURVHOST_HEAP_BYTES   (256u * 1024 * 1024)
#endif
#ifndef PURVHOST_STACK_BYTES
#define PURVHOST_STACK_BYTES  (16u * 1024 * 1024)
#endif

/* The host buffers and what loading discovered. `stack` and `heap` are two views into
 * ONE writable allocation (stack is its base, at RISCV_HALF - stack_len; heap begins
 * stack_len bytes in, at RISCV_HALF); sp starts at RISCV_HALF and grows down. */
typedef struct {
    uint8_t *code,  *heap,  *stack;          /* code = readonly; stack = writable base, heap its seam */
    uint32_t code_len, heap_len, stack_len;
    uint32_t entry;                          /* image entry point (pc starts here)     */
    uint32_t data_end;                       /* one past the highest writable-data byte */
                                             /* (== RISCV_HALF when the image has none; */
                                             /*  the malloc arena starts here)          */
    uint8_t *elf;                            /* whole ELF file, kept for symbol lookup  */
    long     elf_len;                        /* (NULL/0 after a flat load)              */
} PurvHost;

/* ---- minimal ELF32 little-endian layout (so we need no <elf.h>) ---- */
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version,
    e_entry, e_phoff, e_shoff, e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
    e_shentsize, e_shnum, e_shstrndx; } PurvElf32_Ehdr;
typedef struct { uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
    p_flags, p_align; } PurvElf32_Phdr;
typedef struct { uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
    sh_link, sh_info, sh_addralign, sh_entsize; } PurvElf32_Shdr;
typedef struct { uint32_t st_name, st_value, st_size; uint8_t st_info, st_other;
    uint16_t st_shndx; } PurvElf32_Sym;
#define PURV_PT_LOAD    1
#define PURV_SHT_SYMTAB 2
#define PURV_EM_RISCV   243

/* Read a whole file into a malloc'd buffer; fatal-exit on failure. */
static inline uint8_t *purvhost_slurp(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "purvhost: cannot open %s: %s\n", path, strerror(errno)); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (n <= 0 || !buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "purvhost: read failed for %s\n", path); exit(2);
    }
    fclose(f);
    *len = n;
    return buf;
}

/* Allocate the three region buffers (0 picks the default size). 0 on success, -1 on OOM. */
static inline int purvhost_alloc(PurvHost *h, uint32_t code_len, uint32_t heap_len, uint32_t stack_len) {
    memset(h, 0, sizeof *h);
    h->code_len  = code_len  ? code_len  : PURVHOST_CODE_BYTES;
    h->heap_len  = heap_len  ? heap_len  : PURVHOST_HEAP_BYTES;
    h->stack_len = stack_len ? stack_len : PURVHOST_STACK_BYTES;
    h->code  = calloc(1, h->code_len);
    h->stack = calloc(1, (size_t)h->stack_len + h->heap_len);  /* ONE writable buffer: stack then heap */
    h->heap  = h->stack ? h->stack + h->stack_len : NULL;      /* heap view, based at RISCV_HALF */
    h->data_end = RISCV_HALF;                /* no writable data until an ELF places some */
    if (!h->code || !h->stack) {
        fprintf(stderr, "purvhost: cannot allocate regions\n");
        free(h->code); free(h->stack);
        h->code = h->heap = h->stack = NULL;
        return -1;
    }
    return 0;
}

/* Map the buffers into the engine's two regions and Init the state (sp=RISCV_HALF,
 * pc=0, default callback + eval). The caller then assigns trap handlers and pc. A
 * guest's .rodata rides in the code (readonly) region. */
static inline void purvhost_init(const PurvHost *h, RiscvEmulatorState_t *st) {
    RiscvEmulatorRegion_t readonly = { h->code,  h->code_len, 0 };                          /* code+rodata at 0 */
    RiscvEmulatorRegion_t writable = { h->stack, h->stack_len + h->heap_len, RISCV_HALF - h->stack_len };
    RiscvEmulatorInit(st, readonly, writable);
}

/* Load an ELF32 RISC-V image: each PT_LOAD goes to the half its vaddr names --
 * lower (text/rodata) into the code buffer at vaddr, upper (data/bss) into the heap
 * buffer at vaddr-RISCV_HALF. Sets h->entry and h->data_end; keeps the file in
 * h->elf for purvhost_sym. Fatal-exits on a malformed file or an oversized segment. */
static inline void purvhost_load_elf(PurvHost *h, const char *path) {
    h->elf = purvhost_slurp(path, &h->elf_len);
    if (h->elf_len < (long)sizeof(PurvElf32_Ehdr) || memcmp(h->elf, "\177ELF", 4) != 0) {
        fprintf(stderr, "purvhost: %s is not an ELF\n", path); exit(2);
    }
    PurvElf32_Ehdr eh; memcpy(&eh, h->elf, sizeof eh);
    if (eh.e_ident[4] != 1 /* ELFCLASS32 */ || eh.e_machine != PURV_EM_RISCV) {
        fprintf(stderr, "purvhost: %s is not a 32-bit RISC-V ELF\n", path); exit(2);
    }
    for (int i = 0; i < eh.e_phnum; i++) {
        PurvElf32_Phdr ph;
        memcpy(&ph, h->elf + eh.e_phoff + (long)i * eh.e_phentsize, sizeof ph);
        if (ph.p_type != PURV_PT_LOAD || ph.p_memsz == 0) continue;
        uint8_t *dst; uint32_t base;
        if (ph.p_vaddr < RISCV_HALF) {                       /* text/rodata -> code */
            if ((uint64_t)ph.p_vaddr + ph.p_memsz > h->code_len) {
                fprintf(stderr, "purvhost: code segment vaddr=0x%08x size=0x%x too big\n",
                        ph.p_vaddr, ph.p_memsz); exit(2);
            }
            dst = h->code; base = 0;
        } else {                                             /* data/bss -> heap */
            if ((uint64_t)ph.p_vaddr + ph.p_memsz > (uint64_t)RISCV_HALF + h->heap_len) {
                fprintf(stderr, "purvhost: data segment vaddr=0x%08x size=0x%x outside heap\n",
                        ph.p_vaddr, ph.p_memsz); exit(2);
            }
            dst = h->heap; base = RISCV_HALF;
            uint32_t end = ph.p_vaddr + ph.p_memsz;
            if (end > h->data_end) h->data_end = end;
        }
        if (ph.p_filesz) memcpy(&dst[ph.p_vaddr - base], h->elf + ph.p_offset, ph.p_filesz);
    }
    h->entry = eh.e_entry;
}

/* Load a flat binary (objcopy -O binary): the raw bytes go to the code region at 0
 * with the entry point first, so entry = 0. No writable data segment, so data_end
 * stays at RISCV_HALF (the heap region). Fatal-exits if the image overflows code. */
static inline void purvhost_load_flat(PurvHost *h, const char *path) {
    long n; uint8_t *buf = purvhost_slurp(path, &n);
    if ((uint64_t)n > h->code_len) { fprintf(stderr, "purvhost: flat image too big\n"); exit(2); }
    memcpy(h->code, buf, (size_t)n);
    free(buf);
    h->entry = 0;
}

/* Resolve an ELF symbol by name (after purvhost_load_elf). 1 and *out on success. */
static inline int purvhost_sym(const PurvHost *h, const char *name, uint32_t *out) {
    if (!h->elf) return 0;
    PurvElf32_Ehdr eh; memcpy(&eh, h->elf, sizeof eh);
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
            if (sym.st_name && strcmp(strs + sym.st_name, name) == 0) { *out = sym.st_value; return 1; }
        }
    }
    return 0;
}

/* Read one guest byte through the engine's two regions (writable first, then the
 * read-only span), each a base-relative bounded check. For host-side syscalls and
 * signature dumps that touch guest memory. */
static inline uint8_t purvhost_guest_byte(const RiscvEmulatorState_t *st, uint32_t a) {
    uint32_t rel = a - st->writable.base;
    if (rel < st->writable.len) return st->writable.ptr[rel];
    rel = a - st->readonly.base;
    if (rel < st->readonly.len) return st->readonly.ptr[rel];
    return 0;
}

/* ---- the wrapper pair: allocate default regions, load, and Init in one call ---- */

/* Returns the entry point (also in h->entry); the state is ready bar trap handlers. */
static inline uint32_t purvhost_setup_elf(PurvHost *h, RiscvEmulatorState_t *st, const char *path) {
    if (purvhost_alloc(h, 0, 0, 0) != 0) exit(2);
    purvhost_load_elf(h, path);
    purvhost_init(h, st);
    st->pc = h->entry;
    return h->entry;
}
static inline uint32_t purvhost_setup_flat(PurvHost *h, RiscvEmulatorState_t *st, const char *path) {
    if (purvhost_alloc(h, 0, 0, 0) != 0) exit(2);
    purvhost_load_flat(h, path);
    purvhost_init(h, st);
    st->pc = h->entry;
    return h->entry;
}

#endif /* PURVHOST_H_ */
