/*
 * embed.c - the host half of the embedding demo: purvarun.h, start to finish.
 *
 *     make -C ../purva embed-demo
 *
 * Reads a pvcc-built image (embed-app.c -> embed-app.img), initializes a
 * PurvaRun with two callbacks (write -> stdout/stderr; one user hostcall),
 * and runs it in deliberately small fuel slices to show the metering: the
 * host regains control between slices, exactly the "run a bit, do host
 * things, continue" embedding loop.
 */
#include <stdio.h>
#include <stdlib.h>

#include "purvarun.h"

static long cb_write(void *user, int fd, const void *buf, unsigned long len) {
    (void)user;
    return (long)fwrite(buf, 1, len, fd == 2 ? stderr : stdout);
}

/* fn 16: "roll a die with a0 sides" -- deterministic here, it's a demo */
static uint32_t cb_hostcall(void *user, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, int *stop) {
    (void)user; (void)a1; (void)a2; (void)stop;
    if (fn == 16) return a0 ? (uint32_t)(4211 % a0) + 1 : 0;
    return 0;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "embed-app.img";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "embed: cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *img = malloc((size_t)len);
    if (fread(img, 1, (size_t)len, f) != (size_t)len) { fclose(f); return 2; }
    fclose(f);

    PurvaRun r;
    PurvaCallbacks cb = { .write = cb_write, .hostcall = cb_hostcall };
    if (purva_run_init(&r, img, (size_t)len, 0, 0, &cb) != 0) {
        fprintf(stderr, "embed: bad image\n");
        return 2;
    }
    free(img);                                /* init copied everything it needs */

    uint64_t total = 0; int slices = 0, st;
    do {
        uint64_t used = 0;
        st = purva_run(&r, 200 * 1000, &used);           /* 200k-instruction slices */
        total += used; slices++;
    } while (st == PURVA_RUN_FUEL);

    printf("host: guest %s, exit=%d, ~%llu insns in %d fuel slices\n",
           st == PURVA_RUN_DONE ? "finished" : "trapped",
           r.exit_code, (unsigned long long)total, slices);
    purva_run_free(&r);
    return st == PURVA_RUN_DONE ? 0 : 1;
}
