/*
 * richards.cc - Martin Richards' operating-system-simulation benchmark, the
 * classic object-oriented workload (an idle task, two symmetric worker tasks,
 * two device handlers and their driver tasks passing packets through queues).
 * This is a clean-room C++ implementation of the well-known algorithm; it is
 * dominated by virtual dispatch, small-object new/delete and pointer chasing --
 * exactly the C++ flavour the sqlite bench (plain C) does not exercise.
 *
 * Two builds, the bench.c pattern: freestanding RV32 guest (-DPURV_GUEST,
 * output via the write host-call, new/delete via rt-cxx.cc) and a native,
 * self-timed host binary. The scheduler's packet/hold counts are checked
 * against the published expected values every iteration, so a wrong result
 * fails loudly on either build. BENCH_SCALE (percent) dials the iterations.
 */
#include <stddef.h>

#ifndef BENCH_SCALE
#define BENCH_SCALE 100
#endif
#define ITERATIONS ((300 * BENCH_SCALE) / 100)

static void bench_emit(const char *s);
static void emit_u32(unsigned long v) {
    char b[12]; int i = 11; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    bench_emit(b + i);
}

/* ---- the simulation ------------------------------------------------------- */

enum { IDLER = 0, WORKER = 1, HANDLER_A = 2, HANDLER_B = 3, DEVICE_A = 4, DEVICE_B = 5, NTASKS = 6 };
enum { KIND_DEV = 0, KIND_WORK = 1 };
enum { BUFSIZE = 4 };

struct Packet {
    Packet *link;
    int id;                 /* destination task */
    int kind;
    int a1;
    int a2[BUFSIZE];
    Packet(Packet *l, int i, int k) : link(l), id(i), kind(k), a1(0) {
        for (int j = 0; j < BUFSIZE; j++) a2[j] = 0;
    }
};

static Packet *append(Packet *pkt, Packet *list) {
    pkt->link = 0;
    if (!list) return pkt;
    Packet *p = list;
    while (p->link) p = p->link;
    p->link = pkt;
    return list;
}

class Scheduler;

class Task {
public:
    Task(Scheduler *s, int id, int pri, Packet *queue);
    virtual ~Task() {}
    virtual Task *run(Packet *pkt) = 0;

    Task   *link;
    int     id, pri;
    Packet *queue;
    /* task state: packet-pending / task-waiting / task-holding bits */
    bool    packet_pending, task_waiting, task_holding;
protected:
    Scheduler *sched;
};

class Scheduler {
public:
    Scheduler() : task_list(0), current(0), current_id(0), qpkt_count(0), hold_count(0) {
        for (int i = 0; i < NTASKS; i++) blocks[i] = 0;
    }
    void add(Task *t) {
        t->link = task_list;
        task_list = t;
        blocks[t->id] = t;
    }
    void schedule() {
        current = task_list;
        while (current) {
            if (current->task_holding || (current->task_waiting && !current->packet_pending)) {
                current = current->link;                 /* not runnable: next task */
            } else {
                current_id = current->id;
                Packet *pkt = 0;
                if (current->task_waiting) {   /* waiting-with-packet: deliver it */
                    pkt = current->queue;
                    current->queue = pkt->link;
                    current->packet_pending = current->queue != 0;
                    current->task_waiting = false;
                }
                current = current->run(pkt);
            }
        }
    }
    /* the scheduler services tasks return to transfer control */
    Task *hold_self() {
        hold_count++;
        current->task_holding = true;
        return current->link;
    }
    Task *wait_self() {
        current->task_waiting = true;
        return current;
    }
    Task *release(int id) {
        Task *t = blocks[id];
        if (!t) return 0;
        t->task_holding = false;
        return t->pri > current->pri ? t : current;
    }
    Task *queue_pkt(Packet *pkt) {
        Task *t = blocks[pkt->id];
        if (!t) return 0;
        qpkt_count++;
        pkt->link = 0;
        pkt->id = current_id;
        if (!t->queue && !t->packet_pending) {
            t->queue = pkt;
            t->packet_pending = true;
            if (t->pri > current->pri) return t;
        } else {
            t->queue = append(pkt, t->queue);
        }
        return current;
    }

    int qpkt_count, hold_count;
private:
    Task *task_list, *current;
    int   current_id;
    Task *blocks[NTASKS];
};

Task::Task(Scheduler *s, int i, int p, Packet *q)
    : link(0), id(i), pri(p), queue(q),
      /* initial states as in the original: idle (no queue) starts running,
       * worker/handlers start waiting-with-packet, devices set waiting below */
      packet_pending(q != 0), task_waiting(q != 0), task_holding(false), sched(s) {
    s->add(this);
}

class IdleTask : public Task {
    int count;
    unsigned control;
public:
    IdleTask(Scheduler *s, int pri, int cnt)
        : Task(s, IDLER, pri, 0), count(cnt), control(1) {}
    Task *run(Packet *) {
        if (--count == 0) return sched->hold_self();
        if ((control & 1) == 0) {
            control >>= 1;
            return sched->release(DEVICE_A);
        }
        control = (control >> 1) ^ 0xd008u;
        return sched->release(DEVICE_B);
    }
};

class WorkerTask : public Task {
    int dest, count;
public:
    WorkerTask(Scheduler *s, int pri, Packet *q)
        : Task(s, WORKER, pri, q), dest(HANDLER_A), count(0) {}
    Task *run(Packet *pkt) {
        if (!pkt) return sched->wait_self();
        dest = dest == HANDLER_A ? HANDLER_B : HANDLER_A;
        pkt->id = dest;
        pkt->a1 = 0;
        for (int i = 0; i < BUFSIZE; i++) {
            if (++count > 26) count = 1;
            pkt->a2[i] = 'A' + count - 1;
        }
        return sched->queue_pkt(pkt);
    }
};

class HandlerTask : public Task {
    Packet *work_in, *dev_in;
public:
    HandlerTask(Scheduler *s, int id, int pri, Packet *q)
        : Task(s, id, pri, q), work_in(0), dev_in(0) {}
    Task *run(Packet *pkt) {
        if (pkt) {
            if (pkt->kind == KIND_WORK) work_in = append(pkt, work_in);
            else                        dev_in  = append(pkt, dev_in);
        }
        if (work_in) {
            Packet *w = work_in;
            int count = w->a1;
            if (count >= BUFSIZE) {
                work_in = w->link;
                return sched->queue_pkt(w);
            }
            if (dev_in) {
                Packet *d = dev_in;
                dev_in = d->link;
                d->a1 = w->a2[count];
                w->a1 = count + 1;
                return sched->queue_pkt(d);
            }
        }
        return sched->wait_self();
    }
};

class DeviceTask : public Task {
    Packet *pending;
public:
    DeviceTask(Scheduler *s, int id, int pri)
        : Task(s, id, pri, 0), pending(0) { task_waiting = true; }
    Task *run(Packet *pkt) {
        if (!pkt) {
            if (!pending) return sched->wait_self();
            Packet *p = pending;
            pending = 0;
            return sched->queue_pkt(p);
        }
        pending = pkt;
        return sched->hold_self();
    }
};

/* Published expected counts for one run of this task set (idle count 10000). */
enum { EXPECT_QPKT = 23246, EXPECT_HOLD = 9297 };

static int richards_once() {
    Scheduler sched;

    new IdleTask(&sched, 0, 10000);

    Packet *wq = new Packet(0, WORKER, KIND_WORK);
    wq = new Packet(wq, WORKER, KIND_WORK);
    new WorkerTask(&sched, 1000, wq);

    Packet *qa = new Packet(0, DEVICE_A, KIND_DEV);
    qa = new Packet(qa, DEVICE_A, KIND_DEV);
    qa = new Packet(qa, DEVICE_A, KIND_DEV);
    new HandlerTask(&sched, HANDLER_A, 2000, qa);

    Packet *qb = new Packet(0, DEVICE_B, KIND_DEV);
    qb = new Packet(qb, DEVICE_B, KIND_DEV);
    qb = new Packet(qb, DEVICE_B, KIND_DEV);
    new HandlerTask(&sched, HANDLER_B, 3000, qb);

    new DeviceTask(&sched, DEVICE_A, 4000);
    new DeviceTask(&sched, DEVICE_B, 5000);

    sched.schedule();

    int ok = sched.qpkt_count == EXPECT_QPKT && sched.hold_count == EXPECT_HOLD;
    /* Note: tasks and packets are deliberately not reclaimed one by one -- the
     * original benchmark leaks them too; allocation is part of the workload. */
    return ok;
}

static int richards_run(void) {
    bench_emit("richards: os simulation, ");
    emit_u32(ITERATIONS); bench_emit(" iterations\n");
    for (int i = 0; i < ITERATIONS; i++)
        if (!richards_once()) { bench_emit("FAILED: bad counts\n"); return 1; }
    bench_emit("qpkt="); emit_u32(EXPECT_QPKT);
    bench_emit(" hold="); emit_u32(EXPECT_HOLD);
    bench_emit(" (verified every iteration)\ndone.\n");
    return 0;
}

/* ------------------------------------------------------------ build shims --- */

#ifdef PURV_GUEST
extern "C" long host_write(int fd, const void *buf, long len);
static void bench_emit(const char *s) {
    long n = 0; while (s[n]) n++;
    host_write(1, s, n);
}
extern "C" int cmain(void) { return richards_run(); }
#else
#include <stdio.h>
#include <time.h>
static void bench_emit(const char *s) { fputs(s, stdout); }
int main(void) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int rc = richards_run();
    clock_gettime(CLOCK_MONOTONIC, &b);
    fflush(stdout);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    fprintf(stderr, "BENCH wall_ms=%ld\n", ms);
    return rc;
}
#endif
