/*
 * guest.c - the freestanding test program. Opens an in-memory SQLite database,
 * runs real SQL (DDL, inserts, an ordered SELECT, and aggregates), and prints
 * each result row. Its only contact with the outside world is host_write (for
 * output) via the ecall host-call ABI -- everything else is pure computation
 * inside the emulated machine.
 */
#include "sqlite3.h"

extern long host_write(int fd, const void *buf, long len);

static void w(const char *s) {
    long n = 0; while (s[n]) n++;
    host_write(1, s, n);
}

static int row(void *u, int ncol, char **val, char **name) {
    (void)u; (void)name;
    for (int i = 0; i < ncol; i++) {
        if (i) w(" | ");
        w(val[i] ? val[i] : "NULL");
    }
    w("\n");
    return 0;
}

static int run(sqlite3 *db, const char *sql) {
    char *err = 0;
    int rc = sqlite3_exec(db, sql, row, 0, &err);
    if (rc != SQLITE_OK) { w("SQL error: "); w(err ? err : "?"); w("\n"); }
    return rc;
}

int cmain(void) {
    sqlite3 *db;

    sqlite3_initialize();                       /* required: SQLITE_OMIT_AUTOINIT */
    w("sqlite "); w(sqlite3_libversion()); w("  (in-memory, freestanding on purv)\n");

    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { w("open failed\n"); return 1; }

    run(db,
        "CREATE TABLE fruit(id INTEGER PRIMARY KEY, name TEXT, qty INTEGER);"
        "INSERT INTO fruit(name,qty) VALUES"
        "  ('apple',5),('banana',3),('cherry',12),('date',7),('elderberry',1);");

    w("\n-- rows, ordered by qty desc --\n");
    run(db, "SELECT id, name, qty FROM fruit ORDER BY qty DESC;");

    w("\n-- aggregates --\n");
    run(db, "SELECT count(*), sum(qty), min(qty), max(name) FROM fruit;");

    w("\n-- a join / computed column --\n");
    run(db, "SELECT name, qty, qty*qty AS sq FROM fruit WHERE qty BETWEEN 3 AND 10 ORDER BY name;");

    sqlite3_close(db);
    w("\ndone.\n");
    return 0;
}
