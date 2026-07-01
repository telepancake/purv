/*
 * image.h - the purva runnable image: format + read/write.
 *
 * What `transcode` produces and `purva` runs, with nothing else involved at run
 * time (no ELF, no separate ops file). Entry is implicit 0 -- the linker script
 * places the entry point first, so guest execution always starts at op 0.
 *
 * Layout (five little-endian uint32 header fields, then three byte blobs):
 *   code_size      bytes of code (the packed op array; code_size/4 == op count)
 *   rodata_size    bytes of read-only data
 *   rwdata_size    bytes of initial writable data (the .data contents)
 *   bss_size       zero bytes after rwdata -- bss plus a minimum initial heap
 *   stack_size     a minimum initial stack size
 *   code[code_size] | rodata[rodata_size] | rwdata[rwdata_size]
 *
 * That's the FILE layout; it is NOT the guest ADDRESS layout. code and rodata are
 * two separate regions in guest address space (purv.h's region[RISCV_CODE] and
 * region[RISCV_RODATA]): code at [0, code_size), rodata at [0 - rodata_size, 0)
 * -- i.e. small NEGATIVE addresses, growing down from 0 to end exactly at 2^32.
 * rodata's base is never stored, only derived from rodata_size, exactly as purv.h
 * documents for read-only data that grows down from 0 (see purva.c's mem_xlate and
 * purva.ld, which places .rodata there for exactly this reason). purva's "code" is
 * transcoded, packed op words, not real RISC-V bytes, so it is fetch-only -- never
 * data-addressable at all, unlike purv's, which stays real bytes forever and can
 * safely host rodata inline; this is why the image needs rodata to be a genuinely
 * separate region rather than appended after code the way purv's driver does it.
 *
 * bss_size and stack_size are MINIMUMS: the host may map a larger heap/stack than
 * the image asks for (e.g. via --ram=), but never smaller. */
#ifndef IMAGE_H_
#define IMAGE_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint32_t code_size, rodata_size, rwdata_size, bss_size, stack_size;
    uint8_t *code, *rodata, *rwdata;     /* code_size / rodata_size / rwdata_size bytes */
} Image;

static inline int img_wr32(FILE *f, uint32_t v) { return fwrite(&v, 4, 1, f) == 1; }
static inline int img_rd32(FILE *f, uint32_t *v) { return fread(v, 4, 1, f) == 1; }

static inline int image_write(const char *path, const Image *img) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = img_wr32(f, img->code_size) && img_wr32(f, img->rodata_size) &&
             img_wr32(f, img->rwdata_size) && img_wr32(f, img->bss_size) &&
             img_wr32(f, img->stack_size);
    ok = ok && (img->code_size   == 0 || fwrite(img->code,   1, img->code_size,   f) == img->code_size);
    ok = ok && (img->rodata_size == 0 || fwrite(img->rodata, 1, img->rodata_size, f) == img->rodata_size);
    ok = ok && (img->rwdata_size == 0 || fwrite(img->rwdata, 1, img->rwdata_size, f) == img->rwdata_size);
    fclose(f);
    return ok ? 0 : -1;
}

/* Loads into freshly malloc'd buffers (img->code/rodata/rwdata). 0 on success. */
static inline int image_read(const char *path, Image *img) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (!img_rd32(f, &img->code_size) || !img_rd32(f, &img->rodata_size) ||
        !img_rd32(f, &img->rwdata_size) || !img_rd32(f, &img->bss_size) ||
        !img_rd32(f, &img->stack_size)) { fclose(f); return -1; }
    img->code   = malloc(img->code_size   ? img->code_size   : 1);
    img->rodata = malloc(img->rodata_size ? img->rodata_size : 1);
    img->rwdata = malloc(img->rwdata_size ? img->rwdata_size : 1);
    int ok = (img->code_size   == 0 || fread(img->code,   1, img->code_size,   f) == img->code_size) &&
             (img->rodata_size == 0 || fread(img->rodata, 1, img->rodata_size, f) == img->rodata_size) &&
             (img->rwdata_size == 0 || fread(img->rwdata, 1, img->rwdata_size, f) == img->rwdata_size);
    fclose(f);
    return ok ? 0 : -1;
}

#endif /* IMAGE_H_ */
