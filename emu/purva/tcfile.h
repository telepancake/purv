/*
 * tcfile.h - the transcoded-program container (.tc): format + read/write.
 *
 * The standalone `transcode` tool turns an RV32IM ELF into one of these self-
 * contained files; the `purva` evaluator loads it and runs it directly -- it never
 * sees the original ELF or any RISC-V instruction. A .tc carries everything a run
 * needs: the packed op stream and pc->op map (the transcoded code), the lower-half
 * image (so data loads of rodata resolve), the initial upper-half data, the entry
 * pc and data extent, and the symbol table (so --invoke / --signature still work).
 *
 * Layout (all integers little-endian uint32):
 *   magic, version, entry, code_len, data_end, n_ops, udata_len, n_syms
 *   ops   [n_ops]
 *   map   [(code_len>>1) + 2]
 *   lower [code_len]                       (code + rodata bytes)
 *   udata [udata_len]                      (initial [RISCV_HALF, data_end) bytes)
 *   syms  [n_syms] of { value, namelen, name[namelen] }
 */
#ifndef TCFILE_H_
#define TCFILE_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transcode.h"

#define TC_MAGIC   0x30435650u    /* "PVC0" */
#define TC_VERSION 1u

typedef struct { uint32_t value; char *name; } TcSym;

typedef struct {
    uint32_t entry, code_len, data_end, n_ops, udata_len, n_syms;
    uint32_t *ops;        /* n_ops words                       */
    uint32_t *map;        /* (code_len>>1)+2 entries           */
    uint8_t  *lower;      /* code_len bytes (code + rodata)     */
    uint8_t  *udata;      /* udata_len bytes (initial data)     */
    TcSym    *syms;       /* n_syms                             */
} TcImage;

static inline uint32_t tc_map_len(uint32_t code_len) { return (code_len >> 1) + 2; }

static inline int tc_wr32(FILE *f, uint32_t v) { return fwrite(&v, 4, 1, f) == 1; }
static inline int tc_rd32(FILE *f, uint32_t *v) { return fread(v, 4, 1, f) == 1; }

/* Serialize img to path. Returns 0 on success. */
static inline int tc_write(const char *path, const TcImage *img) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = tc_wr32(f, TC_MAGIC) && tc_wr32(f, TC_VERSION) && tc_wr32(f, img->entry) &&
             tc_wr32(f, img->code_len) && tc_wr32(f, img->data_end) && tc_wr32(f, img->n_ops) &&
             tc_wr32(f, img->udata_len) && tc_wr32(f, img->n_syms);
    ok = ok && fwrite(img->ops, 4, img->n_ops, f) == img->n_ops;
    uint32_t ml = tc_map_len(img->code_len);
    ok = ok && fwrite(img->map, 4, ml, f) == ml;
    ok = ok && (img->code_len == 0 || fwrite(img->lower, 1, img->code_len, f) == img->code_len);
    ok = ok && (img->udata_len == 0 || fwrite(img->udata, 1, img->udata_len, f) == img->udata_len);
    for (uint32_t i = 0; ok && i < img->n_syms; i++) {
        uint32_t n = (uint32_t)strlen(img->syms[i].name);
        ok = tc_wr32(f, img->syms[i].value) && tc_wr32(f, n) && fwrite(img->syms[i].name, 1, n, f) == n;
    }
    fclose(f);
    return ok ? 0 : -1;
}

/* Load path into img (allocates). Returns 0 on success. */
static inline int tc_read(const char *path, TcImage *img) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t magic, ver;
    if (!tc_rd32(f, &magic) || magic != TC_MAGIC || !tc_rd32(f, &ver) || ver != TC_VERSION) { fclose(f); return -1; }
    if (!tc_rd32(f, &img->entry) || !tc_rd32(f, &img->code_len) || !tc_rd32(f, &img->data_end) ||
        !tc_rd32(f, &img->n_ops) || !tc_rd32(f, &img->udata_len) || !tc_rd32(f, &img->n_syms)) { fclose(f); return -1; }
    uint32_t ml = tc_map_len(img->code_len);
    img->ops   = malloc((size_t)img->n_ops * 4 + 4);
    img->map   = malloc((size_t)ml * 4);
    img->lower = malloc(img->code_len ? img->code_len : 1);
    img->udata = malloc(img->udata_len ? img->udata_len : 1);
    img->syms  = img->n_syms ? malloc((size_t)img->n_syms * sizeof *img->syms) : NULL;
    int ok = fread(img->ops, 4, img->n_ops, f) == img->n_ops &&
             fread(img->map, 4, ml, f) == ml &&
             (img->code_len == 0 || fread(img->lower, 1, img->code_len, f) == img->code_len) &&
             (img->udata_len == 0 || fread(img->udata, 1, img->udata_len, f) == img->udata_len);
    for (uint32_t i = 0; ok && i < img->n_syms; i++) {
        uint32_t n;
        ok = tc_rd32(f, &img->syms[i].value) && tc_rd32(f, &n);
        if (!ok) break;
        img->syms[i].name = malloc(n + 1);
        ok = fread(img->syms[i].name, 1, n, f) == n;
        img->syms[i].name[n] = 0;
    }
    fclose(f);
    return ok ? 0 : -1;
}

/* Resolve a symbol by name (linear scan). 1 and *out on success. */
static inline int tc_sym(const TcImage *img, const char *name, uint32_t *out) {
    for (uint32_t i = 0; i < img->n_syms; i++)
        if (strcmp(img->syms[i].name, name) == 0) { *out = img->syms[i].value; return 1; }
    return 0;
}

#endif /* TCFILE_H_ */
