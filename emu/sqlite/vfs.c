/*
 * vfs.c - the minimal SQLite VFS required by SQLITE_OS_OTHER.
 *
 * For an in-memory database (":memory:") with SQLITE_TEMP_STORE=3 the file
 * methods are never reached, so they are stubs. The only methods SQLite really
 * uses are xRandomness (to seed its PRNG), xCurrentTime/xSleep, and
 * xFullPathname. None of them touch the host: randomness and time are produced
 * in-guest, so the guest's *only* outside dependency remains the two host calls
 * (write/exit) used by the test program for output.
 */
#include "sqlite3.h"

/* ---- file object: never actually opened for :memory:, but must be valid ---- */
typedef struct { sqlite3_file base; } VFile;

static int ioErr(sqlite3_file *p)                                  { (void)p; return SQLITE_IOERR; }
static int xClose(sqlite3_file *p)                                 { (void)p; return SQLITE_OK; }
static int xRead(sqlite3_file *p, void *b, int n, sqlite3_int64 o) { (void)p;(void)b;(void)n;(void)o; return SQLITE_IOERR_READ; }
static int xWrite(sqlite3_file *p, const void *b, int n, sqlite3_int64 o){ (void)p;(void)b;(void)n;(void)o; return SQLITE_IOERR_WRITE; }
static int xTrunc(sqlite3_file *p, sqlite3_int64 s)                { (void)p;(void)s; return SQLITE_OK; }
static int xSync(sqlite3_file *p, int f)                           { (void)p;(void)f; return SQLITE_OK; }
static int xFileSize(sqlite3_file *p, sqlite3_int64 *s)            { (void)p; *s = 0; return SQLITE_OK; }
static int xLock(sqlite3_file *p, int l)                           { (void)p;(void)l; return SQLITE_OK; }
static int xUnlock(sqlite3_file *p, int l)                         { (void)p;(void)l; return SQLITE_OK; }
static int xCheckLock(sqlite3_file *p, int *r)                     { (void)p; *r = 0; return SQLITE_OK; }
static int xFileCtl(sqlite3_file *p, int op, void *a)              { (void)p;(void)op;(void)a; return SQLITE_NOTFOUND; }
static int xSectorSz(sqlite3_file *p)                              { (void)p; return 512; }
static int xDevChar(sqlite3_file *p)                               { (void)p; return 0; }

static const sqlite3_io_methods io_methods = {
    1, xClose, xRead, xWrite, xTrunc, xSync, xFileSize,
    xLock, xUnlock, xCheckLock, xFileCtl, xSectorSz, xDevChar
};

/* ---- VFS methods ---- */

static int xOpen(sqlite3_vfs *v, sqlite3_filename name, sqlite3_file *f, int flags, int *out) {
    (void)v; (void)name; (void)flags;
    f->pMethods = &io_methods;        /* valid, though :memory: never reads/writes it */
    if (out) *out = flags;
    return SQLITE_OK;
}
static int xDelete(sqlite3_vfs *v, const char *name, int sync) { (void)v;(void)name;(void)sync; return SQLITE_OK; }
static int xAccess(sqlite3_vfs *v, const char *name, int flags, int *res) { (void)v;(void)name;(void)flags; *res = 0; return SQLITE_OK; }
static int xFullPath(sqlite3_vfs *v, const char *name, int nOut, char *out) {
    (void)v;
    int i = 0;
    while (name[i] && i < nOut - 1) { out[i] = name[i]; i++; }
    out[i] = 0;
    return SQLITE_OK;
}

/* Deterministic in-guest PRNG (xorshift64); no host randomness needed. */
static sqlite3_uint64 g_rng = 0x9e3779b97f4a7c15ULL;
static int xRandomness(sqlite3_vfs *v, int n, char *out) {
    (void)v;
    for (int i = 0; i < n; i++) {
        g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
        out[i] = (char)(g_rng >> 24);
    }
    return n;
}
static int xSleep(sqlite3_vfs *v, int us) { (void)v; return us; }
/* With SQLITE_OMIT_FLOATING_POINT the struct's `double*` is sqlite3_int64*
 * (Julian day in milliseconds). Date funcs are omitted, so the value is nominal. */
static int xCurrentTime(sqlite3_vfs *v, sqlite3_int64 *p) { (void)v; *p = (sqlite3_int64)2451544 * 86400000; return SQLITE_OK; }
static int xGetLastError(sqlite3_vfs *v, int n, char *b) { (void)v;(void)n; if (n) b[0] = 0; return 0; }

static sqlite3_vfs g_vfs = {
    1,                      /* iVersion (v1: file ops + the basics, no Int64 time / syscalls) */
    sizeof(VFile),          /* szOsFile */
    512,                    /* mxPathname */
    0,                      /* pNext */
    "purv",                 /* zName */
    0,                      /* pAppData */
    xOpen, xDelete, xAccess, xFullPath,
    0, 0, 0, 0,             /* dlopen/error/sym/close: no extension loading */
    xRandomness, xSleep, xCurrentTime, xGetLastError
};

int sqlite3_os_init(void) { return sqlite3_vfs_register(&g_vfs, 1 /* make default */); }
int sqlite3_os_end(void)  { return SQLITE_OK; }
