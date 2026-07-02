/*
 * zbench.c - zlib compression benchmark, shared by two builds (the bench.c
 * pattern): the freestanding RV32 guest (-DPURV_GUEST, output via the write
 * host-call) and a native, self-timed host binary. `make benchmark` runs both
 * and requires byte-identical transcripts.
 *
 * The workload is the unmodified zlib 1.3.1 (vendor/, built -DZ_SOLO so it is
 * genuinely freestanding: no stdio, no stdlib -- the allocator comes in through
 * zalloc/zfree, which we point at malloc/free; on the guest those are the
 * malloc-group host calls). It deflates a deterministic mixed corpus at levels
 * 1, 6 and 9, inflates each stream back, verifies the round trip, and prints
 * one summary line per level (sizes + crc32 + adler32). BENCH_SCALE (percent)
 * dials the corpus size.
 */
#include "zlib.h"

#ifndef BENCH_SCALE
#define BENCH_SCALE 100
#endif

#define CORPUS_BYTES ((unsigned)(1400u * 1024 * BENCH_SCALE / 100))
#define N_PASSES 3   /* deflate/inflate rounds per level */

static void bench_emit(const char *s);

/* ---- tiny formatting (no printf in the freestanding build) ---------------- */
static void emit_u32(unsigned long v) {
    char b[12]; int i = 11; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    bench_emit(b + i);
}
static void emit_hex32(unsigned long v) {
    char b[9]; b[8] = 0;
    for (int i = 7; i >= 0; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    bench_emit(b);
}

/* ---- allocator hooks: Z_SOLO strips zlib's defaults ------------------------ */
#include <stddef.h>
void *malloc(size_t);
void free(void *);
static voidpf zb_alloc(voidpf o, uInt n, uInt s) { (void)o; return malloc((size_t)n * s); }
static void   zb_free (voidpf o, voidpf p)       { (void)o; free(p); }

/* ---- deterministic corpus: text-ish runs, structured records, noise -------
 * Mixed redundancy so all three levels do real work (pure noise would make
 * them equal; pure repetition would make them trivial). xorshift PRNG, fixed
 * seed, so the corpus -- and thus the whole transcript -- is reproducible. */
static unsigned long rng_state = 0x2545f491u;
static unsigned rng(void) {
    unsigned long x = rng_state;
    x ^= (x << 13) & 0xffffffffu; x ^= x >> 17; x ^= (x << 5) & 0xffffffffu;
    rng_state = x;
    return (unsigned)x;
}
static void make_corpus(unsigned char *buf, unsigned len) {
    static const char words[] =
        "the quick brown fox jumps over the lazy dog while zlib deflates "
        "huffman trees and lz77 windows into compact deterministic streams ";
    unsigned wl = sizeof words - 1, i = 0;
    while (i < len) {
        unsigned mode = rng() % 100;
        if (mode < 55) {                       /* text run, word-aligned-ish */
            unsigned off = rng() % wl, n = 16 + rng() % 96;
            while (n-- && i < len) buf[i++] = (unsigned char)words[(off++) % wl];
        } else if (mode < 85) {                /* structured "records"       */
            unsigned v = rng(), n = 8 + rng() % 24;
            while (n-- && i < len) { buf[i++] = (unsigned char)(v & 0xff); v = v >> 3 | v << 29; }
        } else {                               /* incompressible noise       */
            unsigned n = 4 + rng() % 40;
            while (n-- && i < len) buf[i++] = (unsigned char)rng();
        }
    }
}

/* ---- one level: N_PASSES deflate+inflate rounds, then one summary line ---- */
static int run_level(int level, const unsigned char *src, unsigned srclen,
                     unsigned char *comp, unsigned complen,
                     unsigned char *back) {
    unsigned long csize = 0, ccrc = 0, cadler = 0;
    for (int pass = 0; pass < N_PASSES; pass++) {
        z_stream ds; ds.zalloc = zb_alloc; ds.zfree = zb_free; ds.opaque = 0;
        if (deflateInit(&ds, level) != Z_OK) return -1;
        ds.next_in = (Bytef *)src; ds.avail_in = srclen;
        ds.next_out = comp;        ds.avail_out = complen;
        if (deflate(&ds, Z_FINISH) != Z_STREAM_END) return -1;
        csize = ds.total_out;
        deflateEnd(&ds);

        z_stream is; is.zalloc = zb_alloc; is.zfree = zb_free; is.opaque = 0;
        is.next_in = comp; is.avail_in = (uInt)csize;
        if (inflateInit(&is) != Z_OK) return -1;
        is.next_out = back; is.avail_out = srclen;
        if (inflate(&is, Z_FINISH) != Z_STREAM_END || is.total_out != srclen) return -1;
        inflateEnd(&is);

        for (unsigned i = 0; i < srclen; i++)
            if (back[i] != src[i]) return -1;
        ccrc   = crc32(crc32(0, 0, 0), comp, (uInt)csize);
        cadler = adler32(adler32(0, 0, 0), back, srclen);
    }
    bench_emit("level "); emit_u32((unsigned long)level);
    bench_emit(": in="); emit_u32(srclen);
    bench_emit(" out=");  emit_u32(csize);
    bench_emit(" crc=");  emit_hex32(ccrc);
    bench_emit(" adler="); emit_hex32(cadler);
    bench_emit("\n");
    return 0;
}

static int zbench_run(void) {
    unsigned len = CORPUS_BYTES;
    unsigned char *src  = malloc(len);
    unsigned char *back = malloc(len);
    unsigned complen = len + len / 500 + 64;
    unsigned char *comp = malloc(complen);
    if (!src || !back || !comp) { bench_emit("FAILED: oom\n"); return 1; }

    bench_emit("zlib "); bench_emit(ZLIB_VERSION);
    bench_emit(" Z_SOLO, deterministic corpus\n");
    make_corpus(src, len);

    static const int levels[] = { 1, 6, 9 };
    for (unsigned i = 0; i < sizeof levels / sizeof levels[0]; i++)
        if (run_level(levels[i], src, len, comp, complen, back) != 0) {
            bench_emit("FAILED\n"); return 1;
        }
    free(src); free(back); free(comp);
    bench_emit("done.\n");
    return 0;
}

/* ------------------------------------------------------------ build shims --- */

#ifdef PURV_GUEST
extern long host_write(int fd, const void *buf, long len);
static void bench_emit(const char *s) {
    long n = 0; while (s[n]) n++;
    host_write(1, s, n);
}
int cmain(void) { return zbench_run(); }
#else
#include <stdio.h>
#include <time.h>
static void bench_emit(const char *s) { fputs(s, stdout); }
int main(void) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int rc = zbench_run();
    clock_gettime(CLOCK_MONOTONIC, &b);
    fflush(stdout);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    fprintf(stderr, "BENCH wall_ms=%ld\n", ms);
    return rc;
}
#endif
