/*
 * bench.c - a deliberately non-trivial SQLite workload, shared by two builds:
 *
 *   - the freestanding RV32 guest (compiled with -DPURV_GUEST), run on the purv
 *     emulator, output via the host-call ABI;
 *   - a native host binary (no define), output via stdio and self-timed.
 *
 * `make benchmark` builds both from THIS file and the SAME sqlite3.c options, runs
 * the workload on each, checks the two outputs are byte-identical, and reports how
 * much slower the emulated CPU is than the host. So the workload has to be both
 * heavy (many seconds on the interpreter) and exactly reproducible.
 *
 * It exercises a wide spread of SQLite: bulk insert in a transaction, a secondary
 * index, GROUP BY/HAVING aggregation, window functions, a correlated self-join,
 * string functions (printf/substr/replace/instr/hex), an UPDATE+DELETE mutation
 * pass, a large ORDER BY, and two recursive CTEs (a Collatz-length tournament and
 * an integer-only Mandelbrot count) -- all in pure integer arithmetic, because the
 * guest is built -DSQLITE_OMIT_FLOATING_POINT. Every phase collapses to a single
 * checksum row so the transcript stays small and identical on both machines.
 *
 * BENCH_SCALE (percent, default 100) dials the heavy phases up or down without
 * changing which code paths run -- `make benchmark BENCH_SCALE=20` for a quick
 * pass, higher for a longer one.
 */
#include "sqlite3.h"

#ifndef BENCH_SCALE
#define BENCH_SCALE 100
#endif
#define SCALED(n) ((int)((long)(n) * BENCH_SCALE / 100))

/* Workload sizes (at BENCH_SCALE=100). Chosen so the native run is a fraction of
 * a second and the emulated run is many seconds (tens of seconds here). Note the
 * self-join is ~quadratic and the Mandelbrot grid grows as N_MANDEL^2, so raising
 * BENCH_SCALE costs super-linearly. */
#define N_ROWS    SCALED(8000)    /* base table rows                         */
#define N_JOIN    SCALED(500)     /* self-join window (O(n^2)-ish, indexed)  */
#define N_COLLATZ SCALED(1000)    /* Collatz starting values                 */
#define N_MANDEL  SCALED(24)      /* Mandelbrot grid is N_MANDEL x N_MANDEL  */
#define N_JSON    SCALED(3000)    /* rows serialized into JSON documents     */
#define N_REPS    2               /* repeat the aggregate/window block       */

/* bench_emit is provided by whichever shell wraps this file (guest vs native). */
static void bench_emit(const char *s);

/* ---- one-row-per-summary query helper -------------------------------------
 * Each diagnostic query returns exactly one row; we print "label: c0 | c1 | ..".
 * Values arrive from SQLite already formatted as text, so the workload needs no
 * number-to-string code of its own (handy in the freestanding build). */
static const char *g_label;
static int print_row(void *u, int ncol, char **val, char **name) {
    (void)u; (void)name;
    bench_emit(g_label);
    bench_emit(": ");
    for (int i = 0; i < ncol; i++) {
        if (i) bench_emit(" | ");
        bench_emit(val[i] ? val[i] : "NULL");
    }
    bench_emit("\n");
    return 0;
}

static sqlite3 *DB;
static int g_rc;

/* Run SQL that produces no rows (DDL, INSERT, UPDATE, ...). */
static void x(const char *sql) {
    char *err = 0;
    if (sqlite3_exec(DB, sql, 0, 0, &err) != SQLITE_OK) {
        bench_emit("SQL error: "); bench_emit(err ? err : "?"); bench_emit("\n");
        g_rc = 1;
    }
}
/* Run a one-row summary query and print it under `label`. */
static void q(const char *label, const char *sql) {
    char *err = 0;
    g_label = label;
    if (sqlite3_exec(DB, sql, print_row, 0, &err) != SQLITE_OK) {
        bench_emit("SQL error ["); bench_emit(label); bench_emit("]: ");
        bench_emit(err ? err : "?"); bench_emit("\n");
        g_rc = 1;
    }
}

/* sqlite3_mprintf gives us printf into a freshly-allocated string; we build each
 * statement's size constants into the SQL text so SQLite sees plain integer
 * literals (no bound parameters needed for a benchmark). */
static void xf(const char *fmt, int a) {
    char *sql = sqlite3_mprintf(fmt, a);
    if (!sql) { g_rc = 1; return; }
    x(sql);
    sqlite3_free(sql);
}
static void qf(const char *label, const char *fmt, int a) {
    char *sql = sqlite3_mprintf(fmt, a);
    if (!sql) { g_rc = 1; return; }
    q(label, sql);
    sqlite3_free(sql);
}

int bench_run(void) {
    g_rc = 0;
    sqlite3_initialize();                       /* required: SQLITE_OMIT_AUTOINIT */

    bench_emit("sqlite ");
    bench_emit(sqlite3_libversion());
    bench_emit("  scale=");
    { char *s = sqlite3_mprintf("%d", BENCH_SCALE); bench_emit(s ? s : "?"); sqlite3_free(s); }
    bench_emit("\n");

    if (sqlite3_open(":memory:", &DB) != SQLITE_OK) { bench_emit("open failed\n"); return 1; }
    /* A bigger page cache keeps the heavy joins/sorts in memory. */
    x("PRAGMA cache_size=-8000;");

    /* -- phase 1: bulk insert in a transaction, then a secondary index --------
     * id is the rowid; k is a deterministic Lehmer spread; v is id^2 (64-bit
     * inside SQLite); s is generated text. All integer, all reproducible. */
    x("BEGIN;");
    x("CREATE TABLE t(id INTEGER PRIMARY KEY, k INTEGER, v INTEGER, s TEXT);");
    xf("WITH RECURSIVE seq(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM seq WHERE i<%d)"
       " INSERT INTO t(id,k,v,s)"
       " SELECT i, (i*48271)%%4000, (i*i)%%1000000007,"     /* k range kept small so idx_k */
       "        printf('row-%%06d-%%x', i, (i*48271)%%4000)" /* has dups -> the join has work */
       " FROM seq;", N_ROWS);
    x("CREATE INDEX idx_k ON t(k);");
    x("COMMIT;");
    q("insert",
      "SELECT count(*), sum(v%1000000007), sum((k*131+v)%1000000007), sum(length(s)) FROM t;");

    /* -- phase 2: aggregation with GROUP BY / HAVING, collapsed to a checksum -- */
    for (int r = 0; r < N_REPS; r++)
        q("aggregate",
          "SELECT count(*), sum(c), sum(bucket*c), max(mx), min(mn) FROM ("
          "  SELECT k%97 AS bucket, count(*) c, sum(v%1000003) AS sv, max(v) mx, min(v) mn"
          "  FROM t GROUP BY bucket HAVING count(*) > 1 ORDER BY bucket);");

    /* -- phase 3: window functions (running sum + partitioned rank), checksum -- */
    for (int r = 0; r < N_REPS; r++)
        q("window",
          "SELECT sum(rn%1000000007), sum(run%1000000007) FROM ("
          "  SELECT row_number() OVER (PARTITION BY k%50 ORDER BY v DESC, id) AS rn,"
          "         sum(v) OVER (ORDER BY id ROWS BETWEEN 9 PRECEDING AND CURRENT ROW) AS run"
          "  FROM t);");

    /* -- phase 4: correlated self-join over an indexed window (quadratic-ish) -- */
    qf("selfjoin",
       "SELECT count(*), sum((a.id+b.id)%%1000000007) FROM t a JOIN t b"
       " ON a.k=b.k AND a.id<b.id WHERE a.id<=%d;", N_JOIN);

    /* -- phase 5: string functions: substr/replace/instr/upper/hex --------- */
    q("strings",
      "SELECT sum(length(replace(s,'row','R'))),"
      "       sum(instr(s,'-')),"
      "       sum(length(upper(substr(s,5,6)))),"
      "       sum(length(hex(s))%97) FROM t;");

    /* -- phase 6: a mutation pass (UPDATE + DELETE), then re-aggregate ------ */
    x("UPDATE t SET v=(v+id)%1000000007 WHERE k%2=0;");
    x("DELETE FROM t WHERE k%5=0;");
    q("mutated", "SELECT count(*), sum(v%1000000007) FROM t;");

    /* -- phase 7: a large ORDER BY ... LIMIT (external-ish sort), checksum --- */
    q("topk",
      "SELECT sum(id%1000000007), sum(v%1000000007) FROM ("
      "  SELECT id, v FROM t ORDER BY v DESC, id ASC LIMIT 200);");

    /* -- phase 8: recursive CTE -- Collatz stopping-time tournament --------- */
    qf("collatz",
       "WITH RECURSIVE"
       " start(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM start WHERE x<%d),"
       " walk(x,n,steps) AS ("
       "   SELECT x,x,0 FROM start"
       "   UNION ALL"
       "   SELECT x, CASE WHEN n%%2=0 THEN n/2 ELSE 3*n+1 END, steps+1 FROM walk WHERE n>1)"
       " SELECT count(*), sum(steps), max(steps) FROM ("
       "   SELECT x, max(steps) AS steps FROM walk GROUP BY x);", N_COLLATZ);

    /* -- phase 9: recursive CTE -- integer Mandelbrot escape-count grid -----
     * Pure fixed-point (everything x1000): cell (cx,cy) maps to c = (cx*3500/G -
     * 2500, cy*2000/G - 1000), iterate z=z^2+c until |z|^2>4 or 40 steps. The
     * cross-join anchor seeds a G x G grid; the recursion walks every cell. */
    {
        char *sql = sqlite3_mprintf(
            "WITH RECURSIVE"
            " ax(cx) AS (SELECT 0 UNION ALL SELECT cx+1 FROM ax WHERE cx<%d-1),"
            " ay(cy) AS (SELECT 0 UNION ALL SELECT cy+1 FROM ay WHERE cy<%d-1),"
            " grid(cx,cy,zr,zi,it) AS ("
            "   SELECT cx, cy, 0, 0, 0 FROM ax JOIN ay"
            "   UNION ALL"
            "   SELECT cx, cy,"
            "     (zr*zr - zi*zi)/1000 + (cx*3500/%d - 2500),"
            "     (2*zr*zi)/1000 + (cy*2000/%d - 1000),"
            "     it+1"
            "   FROM grid"
            "   WHERE it<40 AND (zr*zr + zi*zi) < 4000000)"
            " SELECT count(*), sum(it), max(it) FROM ("
            "   SELECT cx, cy, max(it) AS it FROM grid GROUP BY cx, cy);",
            N_MANDEL, N_MANDEL, N_MANDEL, N_MANDEL);
        if (sql) { q("mandelbrot", sql); sqlite3_free(sql); } else g_rc = 1;
    }

    /* -- phase 10: JSON -- sqlite's core json.c, a real parser/serializer:
     * build documents from the table, extract paths, walk arrays with the
     * json_each table-valued function, mutate with json_set/json_remove, and
     * re-aggregate with json_group_array. Strictly integer-valued JSON (the
     * build is -DSQLITE_OMIT_FLOATING_POINT: a real would trap, loudly).
     * This is the string-heavy counterweight to the integer phases above --
     * parse/serialize churn is where memcmp/strlen/memcpy actually run. */
    x("CREATE TABLE j(id INTEGER PRIMARY KEY, doc TEXT);");
    xf("INSERT INTO j(id,doc)"
       " SELECT id, json_object('id', id, 'k', k, 'v', v, 's', s,"
       "        'tags', json_array(k%%7, k%%11, k%%13),"
       "        'sub', json_object('a', id%%251, 'b', v%%257))"
       " FROM t WHERE id<=%d;", N_JSON);
    q("json-build", "SELECT count(*), sum(length(doc)) FROM j;");
    for (int r = 0; r < N_REPS; r++)
        q("json-extract",
          "SELECT sum(json_extract(doc,'$.v')%1000000007),"
          "       sum(json_extract(doc,'$.sub.a')),"
          "       sum(json_array_length(doc,'$.tags')),"
          "       sum(json_extract(doc,'$.tags[1]')) FROM j;");
    q("json-each",
      "SELECT count(*), sum(je.value%1000000007), sum(je.key) FROM j,"
      " json_each(j.doc,'$.tags') je;");
    x("UPDATE j SET doc=json_set(doc,'$.sub.c', id*3, '$.tags[3]', id%17) WHERE id%3=0;");
    x("UPDATE j SET doc=json_remove(doc,'$.s') WHERE id%5=0;");
    q("json-mutate",
      "SELECT count(*), sum(length(doc)),"
      "       sum(json_extract(doc,'$.sub.c') IS NOT NULL),"
      "       sum(json_type(doc,'$.tags')='array') FROM j;");
    q("json-agg",
      "SELECT length(json_group_array(json(doc))), count(*) FROM j WHERE id%97<3;");

    sqlite3_close(DB);
    bench_emit(g_rc ? "FAILED\n" : "done.\n");
    return g_rc;
}

/* ------------------------------------------------------------ build shims --- */

#ifdef PURV_GUEST
/* Freestanding guest: output rides the write host-call; rt.c's _start calls
 * cmain, then issues the exit host-call. */
extern long host_write(int fd, const void *buf, long len);
static void bench_emit(const char *s) {
    long n = 0; while (s[n]) n++;
    host_write(1, s, n);
}
int cmain(void) { return bench_run(); }

#else
/* Native: output to stdout, and time the workload itself (clock_gettime around
 * bench_run, excluding process startup) for the benchmark driver to parse. */
#include <stdio.h>
#include <time.h>
static void bench_emit(const char *s) { fputs(s, stdout); }
int main(void) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int rc = bench_run();
    clock_gettime(CLOCK_MONOTONIC, &b);
    fflush(stdout);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    fprintf(stderr, "BENCH wall_ms=%ld\n", ms);
    return rc;
}
#endif
