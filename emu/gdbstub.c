/*
 * gdbstub.c - GDB Remote Serial Protocol server for the purv emulator.
 *
 * Built only when PURV_GDBSTUB is defined. It serves the RSP on a connected fd
 * handed in by a launcher (no listening here) and drives the engine through the
 * public purv.h surface: registers and PC via the accessors, memory via the same
 * RiscvEmulatorLoad/Store hooks the engine uses, execution one instruction at a
 * time via RiscvEmulatorLoop. Software breakpoints are tracked here (gdb's Z0/z0)
 * rather than by patching guest code. A target description is served so a stock
 * riscv:rv32 gdb knows the 33-register layout (x0..x31, pc) without extra setup.
 */
#ifdef PURV_GDBSTUB

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>

#include "purv.h"
#include "gdbstub.h"

#define GDB_NREG    33                       /* x0..x31 (32) + pc */
#define GDB_MAX_BP  64
#define GDB_BUF     4200                     /* must hold the largest packet body + frame */

static const char hexd[] = "0123456789abcdef";

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ----------------------------------------------------------------- raw fd I/O */

static int gdb_read_byte(int fd) {
    uint8_t c;
    ssize_t n;
    do { n = read(fd, &c, 1); } while (n < 0 && errno == EINTR);
    return n == 1 ? (int)c : -1;
}

static int gdb_write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int gdb_pollin(int fd) {
    struct pollfd pf;
    pf.fd = fd; pf.events = POLLIN; pf.revents = 0;
    return poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN);
}

/* ----------------------------------------------------------- packet framing */

/* Read one packet body into buf (NUL-terminated), ack it, and return its length;
 * -1 on EOF. Bytes outside a $...#cc frame (acks, stray interrupts) are skipped. */
static int gdb_recv(int fd, char *buf, size_t max) {
    for (;;) {
        int c;
        do { c = gdb_read_byte(fd); if (c < 0) return -1; } while (c != '$');
        uint8_t sum = 0;
        size_t len = 0;
        while ((c = gdb_read_byte(fd)) != '#') {
            if (c < 0) return -1;
            if (len + 1 < max) buf[len++] = (char)c;
            sum += (uint8_t)c;
        }
        int h1 = gdb_read_byte(fd), h2 = gdb_read_byte(fd);
        if (h1 < 0 || h2 < 0) return -1;
        buf[len] = '\0';
        if ((uint8_t)((hexval(h1) << 4) | hexval(h2)) != sum) {
            gdb_write_all(fd, "-", 1);       /* bad checksum: ask for a resend */
            continue;
        }
        gdb_write_all(fd, "+", 1);
        return (int)len;
    }
}

/* Frame data as $data#cc, send it, and wait for gdb's '+' (resend on '-'). */
static void gdb_send(int fd, const char *data) {
    char frame[GDB_BUF];
    size_t len = strlen(data), k = 0;
    uint8_t sum = 0;
    frame[k++] = '$';
    for (size_t i = 0; i < len && k + 3 < sizeof frame; i++) {
        frame[k++] = data[i];
        sum += (uint8_t)data[i];
    }
    frame[k++] = '#';
    frame[k++] = hexd[sum >> 4];
    frame[k++] = hexd[sum & 0xf];
    for (;;) {
        if (gdb_write_all(fd, frame, k) < 0) return;
        int a = gdb_read_byte(fd);
        if (a == '-') continue;              /* gdb wants a resend */
        return;                              /* '+' (or EOF): done */
    }
}

/* --------------------------------------------------------------- hex helpers */

static uint32_t parse_hex(const char **s) {
    uint32_t v = 0; int d;
    while ((d = hexval((unsigned char)**s)) >= 0) { v = (v << 4) | (uint32_t)d; (*s)++; }
    return v;
}

/* Registers travel as little-endian target bytes in hex (8 chars per word). */
static uint32_t reg_from_hex(const char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int hi = hexval((unsigned char)p[i * 2]), lo = hexval((unsigned char)p[i * 2 + 1]);
        if (hi < 0 || lo < 0) break;
        v |= (uint32_t)((hi << 4) | lo) << (8 * i);
    }
    return v;
}

static void reg_to_hex(char *o, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        uint8_t b = (uint8_t)(v >> (8 * i));
        *o++ = hexd[b >> 4]; *o++ = hexd[b & 0xf];
    }
}

/* ------------------------------------------------------- registers & memory */

static uint32_t reg_get(RiscvEmulatorState_t *st, uint32_t i) {
    /* gdb's pc is where execution is paused = the next instruction to run, which
     * is the engine's "next" program counter (Get/SetProgramCounter both seed it). */
    return i < 32 ? RiscvEmulatorGetRegister(st, (int)i)
                  : RiscvEmulatorGetNextProgramCounter(st);
}

static void reg_set(RiscvEmulatorState_t *st, uint32_t i, uint32_t v) {
    if (i < 32) RiscvEmulatorSetRegister(st, (int)i, v);
    else        RiscvEmulatorSetProgramCounter(st, v);
}

/* -------------------------------------------------------------- breakpoints */

static uint32_t g_bp[GDB_MAX_BP];
static int      g_nbp;

static int bp_hit(uint32_t addr) {
    for (int i = 0; i < g_nbp; i++) if (g_bp[i] == addr) return 1;
    return 0;
}
static void bp_add(uint32_t addr) {
    if (bp_hit(addr) || g_nbp >= GDB_MAX_BP) return;
    g_bp[g_nbp++] = addr;
}
static void bp_del(uint32_t addr) {
    for (int i = 0; i < g_nbp; i++)
        if (g_bp[i] == addr) { g_bp[i] = g_bp[--g_nbp]; return; }
}

/* --------------------------------------------------------- target description */

/* A minimal target.xml so a stock riscv:rv32 gdb adopts exactly this register
 * file (ABI names, so $sp/$ra/$a0 resolve). Built once. */
static const char *target_xml(void) {
    static const char *name[32] = {
        "zero","ra","sp","gp","tp","t0","t1","t2","fp","s1","a0","a1","a2","a3",
        "a4","a5","a6","a7","s2","s3","s4","s5","s6","s7","s8","s9","s10","s11",
        "t3","t4","t5","t6" };
    static char xml[2048];
    static int built;
    if (!built) {
        size_t k = (size_t)snprintf(xml, sizeof xml,
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
            "<target version=\"1.0\">\n"
            "<architecture>riscv:rv32</architecture>\n"
            "<feature name=\"org.gnu.gdb.riscv.cpu\">\n");
        for (int i = 0; i < 32; i++)
            k += (size_t)snprintf(xml + k, sizeof xml - k,
                "<reg name=\"%s\" bitsize=\"32\" regnum=\"%d\"/>\n", name[i], i);
        snprintf(xml + k, sizeof xml - k,
            "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\" regnum=\"32\"/>\n"
            "</feature>\n</target>\n");
        built = 1;
    }
    return xml;
}

/* ----------------------------------------------------------- execution control */

enum { STOP_TRAP, STOP_INT, STOP_EXIT };

/* Step the engine; for a continue, keep going until the next pc is a breakpoint,
 * gdb sends an interrupt (0x03), or the program ends. */
static int gdb_run(RiscvEmulatorState_t *st, int fd, int cont, const int *halted) {
    unsigned tick = 0;
    for (;;) {
        RiscvEmulatorLoop(st);
        if (*halted) return STOP_EXIT;
        if (!cont) return STOP_TRAP;
        if (bp_hit(RiscvEmulatorGetNextProgramCounter(st))) return STOP_TRAP;
        if ((++tick & 0x3ff) == 0 && gdb_pollin(fd) && gdb_read_byte(fd) == 0x03)
            return STOP_INT;
    }
}

/* ----------------------------------------------------------------- q packets */

static void handle_q(int fd, const char *buf) {
    if (!strncmp(buf, "qSupported", 10)) {
        gdb_send(fd, "PacketSize=1000;qXfer:features:read+");
    } else if (!strcmp(buf, "qAttached")) {
        gdb_send(fd, "1");
    } else if (!strcmp(buf, "qC")) {
        gdb_send(fd, "QC1");
    } else if (!strcmp(buf, "qfThreadInfo")) {
        gdb_send(fd, "m1");
    } else if (!strcmp(buf, "qsThreadInfo")) {
        gdb_send(fd, "l");
    } else if (!strncmp(buf, "qXfer:features:read:", 20)) {
        const char *p = buf + 20;
        const char *colon = strchr(p, ':');
        if (!colon || strncmp(p, "target.xml", 10) != 0) { gdb_send(fd, "E00"); return; }
        const char *q = colon + 1;
        uint32_t off = parse_hex(&q);
        if (*q == ',') q++;
        uint32_t want = parse_hex(&q);
        const char *xml = target_xml();
        uint32_t total = (uint32_t)strlen(xml);
        if (off >= total) { gdb_send(fd, "l"); return; }
        uint32_t chunk = total - off;
        if (chunk > want) chunk = want;
        if (chunk > GDB_BUF - 8) chunk = GDB_BUF - 8;
        char out[GDB_BUF];
        out[0] = (off + chunk < total) ? 'm' : 'l';
        memcpy(out + 1, xml + off, chunk);
        out[1 + chunk] = '\0';
        gdb_send(fd, out);
    } else {
        gdb_send(fd, "");                    /* unsupported query */
    }
}

/* -------------------------------------------------------------- serve loop */

void RiscvEmulatorGdbServe(RiscvEmulatorState_t *st, int fd,
                           const int *halted, const int *exitcode) {
    char buf[GDB_BUF], out[GDB_BUF], wmsg[8] = "S05";
    int exited = 0;
    g_nbp = 0;

    for (;;) {
        int n = gdb_recv(fd, buf, sizeof buf);
        if (n < 0) return;                   /* connection closed */
        if (n == 0) { gdb_send(fd, ""); continue; }

        switch (buf[0]) {
        case '?':
            gdb_send(fd, exited ? wmsg : "S05");
            break;
        case 'g': {                          /* read all registers */
            for (uint32_t i = 0; i < GDB_NREG; i++) reg_to_hex(out + i * 8, reg_get(st, i));
            out[GDB_NREG * 8] = '\0';
            gdb_send(fd, out);
            break;
        }
        case 'G': {                          /* write all registers */
            const char *p = buf + 1;
            for (uint32_t i = 0; i < GDB_NREG && (int)(p - buf) + 8 <= n + 1; i++, p += 8)
                reg_set(st, i, reg_from_hex(p));
            gdb_send(fd, "OK");
            break;
        }
        case 'p': {                          /* read one register */
            const char *p = buf + 1;
            uint32_t i = parse_hex(&p);
            if (i < GDB_NREG) { reg_to_hex(out, reg_get(st, i)); out[8] = '\0'; gdb_send(fd, out); }
            else gdb_send(fd, "00000000");
            break;
        }
        case 'P': {                          /* write one register */
            const char *p = buf + 1;
            uint32_t i = parse_hex(&p);
            if (*p == '=') p++;
            if (i < GDB_NREG) reg_set(st, i, reg_from_hex(p));
            gdb_send(fd, "OK");
            break;
        }
        case 'm': {                          /* read memory */
            const char *p = buf + 1;
            uint32_t addr = parse_hex(&p);
            if (*p == ',') p++;
            uint32_t len = parse_hex(&p);
            if (len > (GDB_BUF - 8) / 2) len = (GDB_BUF - 8) / 2;
            char *o = out;
            for (uint32_t i = 0; i < len; i++) {
                uint8_t b = 0;
                RiscvEmulatorLoad(addr + i, &b, 1);
                *o++ = hexd[b >> 4]; *o++ = hexd[b & 0xf];
            }
            *o = '\0';
            gdb_send(fd, out);
            break;
        }
        case 'M': {                          /* write memory */
            const char *p = buf + 1;
            uint32_t addr = parse_hex(&p);
            if (*p == ',') p++;
            uint32_t len = parse_hex(&p);
            if (*p == ':') p++;
            for (uint32_t i = 0; i < len && hexval((unsigned char)p[0]) >= 0; i++, p += 2) {
                uint8_t b = (uint8_t)((hexval((unsigned char)p[0]) << 4) | hexval((unsigned char)p[1]));
                RiscvEmulatorStore(addr + i, &b, 1);
            }
            gdb_send(fd, "OK");
            break;
        }
        case 'c':                            /* continue [addr] */
        case 's': {                          /* step [addr] */
            if (exited) { gdb_send(fd, wmsg); break; }
            if (buf[1]) { const char *p = buf + 1; RiscvEmulatorSetProgramCounter(st, parse_hex(&p)); }
            int r = gdb_run(st, fd, buf[0] == 'c', halted);
            if (r == STOP_EXIT) {
                exited = 1;
                snprintf(wmsg, sizeof wmsg, "W%02x", (unsigned)(*exitcode) & 0xff);
                gdb_send(fd, wmsg);
            } else {
                gdb_send(fd, r == STOP_INT ? "S02" : "S05");
            }
            break;
        }
        case 'Z':                            /* insert breakpoint */
        case 'z': {                          /* remove breakpoint */
            if (buf[1] != '0' && buf[1] != '1') { gdb_send(fd, ""); break; }  /* sw/hw only */
            const char *p = buf + 2;
            if (*p == ',') p++;
            uint32_t addr = parse_hex(&p);
            if (buf[0] == 'Z') bp_add(addr); else bp_del(addr);
            gdb_send(fd, "OK");
            break;
        }
        case 'q':
            handle_q(fd, buf);
            break;
        case 'H':                            /* set thread for step/continue: only one */
        case 'T':                            /* is thread alive: yes */
        case '!':                            /* extended mode */
            gdb_send(fd, "OK");
            break;
        case 'D':                            /* detach */
            gdb_send(fd, "OK");
            return;
        case 'k':                            /* kill */
            return;
        default:                             /* unsupported: empty reply */
            gdb_send(fd, "");
            break;
        }
    }
}

#endif /* PURV_GDBSTUB */
