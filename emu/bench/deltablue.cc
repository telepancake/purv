/*
 * deltablue.cc - the classic DeltaBlue incremental one-way constraint solver
 * benchmark (Sannella/Freeman-Benson), a clean-room C++ implementation of the
 * well-known algorithm. Where richards.cc is queues and virtual dispatch,
 * DeltaBlue is a *dynamic object graph*: constraints are added and removed at
 * run time, plans are extracted by walking the graph, and nearly every step is
 * a virtual call on a small heap object. The standard two tests run each
 * iteration -- a chain of equality constraints re-planned end to end, and a
 * scale/offset projection re-planned under edits -- and every propagated value
 * is checked, so a wrong result fails loudly on either build.
 *
 * Two builds, the bench.c pattern: freestanding RV32 guest (-DPURV_GUEST) and
 * a native, self-timed host binary. BENCH_SCALE (percent) dials iterations.
 */
#include <stddef.h>

#ifndef BENCH_SCALE
#define BENCH_SCALE 100
#endif
#define ITERATIONS ((40 * BENCH_SCALE) / 100)
#define N_VARS 200   /* chain length / projection size per test */

static void bench_emit(const char *s);
static void emit_u32(unsigned long v) {
    char b[12]; int i = 11; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    bench_emit(b + i);
}
static void fail(const char *why) { bench_emit("FAILED: "); bench_emit(why); bench_emit("\n"); }

/* ---- strengths: total order, REQUIRED strongest ---------------------------- */
enum Strength {
    REQUIRED = 0, STRONG_PREFERRED, PREFERRED, STRONG_DEFAULT,
    NORMAL, WEAK_DEFAULT, WEAKEST
};
static inline bool stronger(Strength a, Strength b) { return a < b; }
static inline bool weaker(Strength a, Strength b)   { return a > b; }
static inline Strength weakest_of(Strength a, Strength b) { return weaker(a, b) ? a : b; }

/* ---- a growable pointer list (no STL in the freestanding build) ------------ */
#ifdef PURV_GUEST
extern "C" { void *malloc(size_t); void free(void *); }
#else
#include <stdlib.h>
#endif

template <typename T> struct List {
    T **items; int count, cap;
    List() : items(0), count(0), cap(0) {}
    ~List() { free(items); }
    void push(T *v) {
        if (count == cap) {
            int ncap = cap ? cap * 2 : 8;
            T **ni = (T **)malloc(sizeof(T *) * ncap);
            for (int i = 0; i < count; i++) ni[i] = items[i];
            free(items); items = ni; cap = ncap;
        }
        items[count++] = v;
    }
    T *pop() { return count ? items[--count] : 0; }
    void remove(T *v) {
        int j = 0;
        for (int i = 0; i < count; i++) if (items[i] != v) items[j++] = items[i];
        count = j;
    }
};

class Constraint;

class Variable {
public:
    long        value;
    List<Constraint> constraints;   /* all constraints that use this variable */
    Constraint *determined_by;
    int         mark;
    Strength    walk_strength;
    bool        stay;
    Variable(long v) : value(v), determined_by(0), mark(0),
                       walk_strength(WEAKEST), stay(true) {}
};

class Planner;

class Constraint {
public:
    Strength strength;
    Constraint(Strength s) : strength(s) {}
    virtual ~Constraint() {}
    virtual bool is_satisfied() = 0;
    virtual void mark_unsatisfied() = 0;
    virtual void add_to_graph() = 0;
    virtual void remove_from_graph() = 0;
    virtual void choose_method(int mark) = 0;
    virtual void mark_inputs(int mark) = 0;
    virtual bool inputs_known(int mark) = 0;
    virtual Variable *output() = 0;
    virtual void execute() = 0;
    virtual void recalculate() = 0;
    virtual bool is_input() { return false; }

    void add(Planner *p);
    void destroy(Planner *p);
    Constraint *satisfy(int mark, Planner *p);
};

class Planner {
public:
    Planner() : current_mark(0) {}
    int new_mark() { return ++current_mark; }

    void incremental_add(Constraint *c) {
        int mark = new_mark();
        for (Constraint *o = c->satisfy(mark, this); o; )
            o = o->satisfy(mark, this);
    }
    void incremental_remove(Constraint *c) {
        Variable *out = c->output();
        c->mark_unsatisfied();
        c->remove_from_graph();
        List<Constraint> unsat;
        remove_propagate_from(out, unsat);
        /* re-add unsatisfied constraints strongest first */
        for (int s = REQUIRED; s <= WEAKEST; s++)
            for (int i = 0; i < unsat.count; i++)
                if (unsat.items[i]->strength == (Strength)s)
                    incremental_add(unsat.items[i]);
    }
    bool add_propagate(Constraint *c, int mark) {
        List<Constraint> todo; todo.push(c);
        while (Constraint *d = todo.pop()) {
            if (d->output()->mark == mark) return false;   /* cycle */
            d->recalculate();
            add_consuming(d->output(), d, todo);
        }
        return true;
    }
    /* extract a plan: every satisfied constraint downstream of the sources,
     * in topological order, then execute it n times */
    void extract_plan_from(List<Constraint> &sources, List<Constraint> &plan) {
        int mark = new_mark();
        List<Constraint> hot;
        for (int i = 0; i < sources.count; i++) hot.push(sources.items[i]);
        while (Constraint *c = hot.pop()) {
            Variable *out = c->output();
            if (out->mark != mark && c->inputs_known(mark)) {
                plan.push(c);
                out->mark = mark;
                add_consuming(out, c, hot);
            }
        }
    }

private:
    int current_mark;
    void add_consuming(Variable *v, Constraint *skip, List<Constraint> &to) {
        for (int i = 0; i < v->constraints.count; i++) {
            Constraint *c = v->constraints.items[i];
            if (c != skip && c->is_satisfied()) to.push(c);
        }
    }
    void remove_propagate_from(Variable *out, List<Constraint> &unsat) {
        out->determined_by = 0;
        out->walk_strength = WEAKEST;
        out->stay = true;
        List<Variable> todo; todo.push(out);
        while (Variable *v = todo.pop()) {
            Constraint *determining = v->determined_by;
            for (int i = 0; i < v->constraints.count; i++) {
                Constraint *c = v->constraints.items[i];
                if (!c->is_satisfied()) unsat.push(c);
                else if (c != determining) { c->recalculate(); todo.push(c->output()); }
            }
        }
    }
};

void Constraint::add(Planner *p) {
    add_to_graph();
    p->incremental_add(this);
}
void Constraint::destroy(Planner *p) {
    if (is_satisfied()) p->incremental_remove(this);
    else remove_from_graph();
    delete this;
}
/* try to satisfy: choose a method, steal the output from whatever weaker
 * constraint currently determines it, propagate; return the loser to retry */
Constraint *Constraint::satisfy(int mark, Planner *p) {
    choose_method(mark);
    if (!is_satisfied()) {
        if (strength == REQUIRED) fail("required constraint unsatisfiable");
        return 0;
    }
    mark_inputs(mark);
    Variable *out = output();
    Constraint *overridden = out->determined_by;
    if (overridden) overridden->mark_unsatisfied();
    out->determined_by = this;
    if (!p->add_propagate(this, mark)) { fail("cycle"); return 0; }
    out->mark = mark;
    return overridden;
}

/* ---- unary constraints ------------------------------------------------------ */
class UnaryConstraint : public Constraint {
protected:
    Variable *out;
    bool satisfied;
public:
    UnaryConstraint(Variable *v, Strength s) : Constraint(s), out(v), satisfied(false) {}
    void add_to_graph()      { out->constraints.push(this); satisfied = false; }
    void remove_from_graph() { out->constraints.remove(this); satisfied = false; }
    bool is_satisfied()      { return satisfied; }
    void mark_unsatisfied()  { satisfied = false; }
    void choose_method(int mark) {
        satisfied = out->mark != mark && stronger(strength, out->walk_strength);
    }
    void mark_inputs(int)         {}
    bool inputs_known(int)        { return true; }
    Variable *output()            { return out; }
    void recalculate() {
        out->walk_strength = strength;
        out->stay = !is_input();
        if (out->stay) execute();
    }
};
class StayConstraint : public UnaryConstraint {
public:
    StayConstraint(Variable *v, Strength s) : UnaryConstraint(v, s) {}
    void execute() {}
};
class EditConstraint : public UnaryConstraint {
public:
    EditConstraint(Variable *v, Strength s) : UnaryConstraint(v, s) {}
    bool is_input() { return true; }
    void execute()  {}
};

/* ---- binary constraints ------------------------------------------------------ */
enum { NONE = 0, FORWARD = 1, BACKWARD = 2 };

class BinaryConstraint : public Constraint {
protected:
    Variable *v1, *v2;
    int direction;
public:
    BinaryConstraint(Variable *a, Variable *b, Strength s)
        : Constraint(s), v1(a), v2(b), direction(NONE) {}
    bool is_satisfied()     { return direction != NONE; }
    void mark_unsatisfied() { direction = NONE; }
    void add_to_graph() {
        v1->constraints.push(this); v2->constraints.push(this); direction = NONE;
    }
    void remove_from_graph() {
        v1->constraints.remove(this); v2->constraints.remove(this); direction = NONE;
    }
    void choose_method(int mark) {
        if (v1->mark == mark)
            direction = (v2->mark != mark && stronger(strength, v2->walk_strength)) ? FORWARD : NONE;
        else if (v2->mark == mark)
            direction = (v1->mark != mark && stronger(strength, v1->walk_strength)) ? BACKWARD : NONE;
        else if (weaker(v1->walk_strength, v2->walk_strength))
            direction = stronger(strength, v1->walk_strength) ? BACKWARD : NONE;
        else
            direction = stronger(strength, v2->walk_strength) ? FORWARD : NONE;
    }
    void mark_inputs(int mark)  { input()->mark = mark; }
    bool inputs_known(int mark) {
        Variable *i = input();
        return i->mark == mark || i->stay || i->determined_by == 0;
    }
    Variable *output() { return direction == FORWARD ? v2 : v1; }
    Variable *input()  { return direction == FORWARD ? v1 : v2; }
    void recalculate() {
        Variable *in = input(), *out = output();
        out->walk_strength = weakest_of(strength, in->walk_strength);
        out->stay = in->stay;
        if (out->stay) execute();
    }
};
class EqualityConstraint : public BinaryConstraint {
public:
    EqualityConstraint(Variable *a, Variable *b, Strength s) : BinaryConstraint(a, b, s) {}
    void execute() { output()->value = input()->value; }
};
/* v2 = v1 * scale + offset (forward); v1 = (v2 - offset) / scale (backward) */
class ScaleConstraint : public BinaryConstraint {
    Variable *scale, *offset;
public:
    ScaleConstraint(Variable *src, Variable *sc, Variable *off, Variable *dst, Strength s)
        : BinaryConstraint(src, dst, s), scale(sc), offset(off) {}
    void add_to_graph() {
        BinaryConstraint::add_to_graph();
        scale->constraints.push(this); offset->constraints.push(this);
    }
    void remove_from_graph() {
        BinaryConstraint::remove_from_graph();
        scale->constraints.remove(this); offset->constraints.remove(this);
    }
    void mark_inputs(int mark) {
        BinaryConstraint::mark_inputs(mark);
        scale->mark = mark; offset->mark = mark;
    }
    void execute() {
        if (direction == FORWARD) v2->value = v1->value * scale->value + offset->value;
        else                      v1->value = (v2->value - offset->value) / scale->value;
    }
    void recalculate() {
        Variable *in = input(), *out = output();
        out->walk_strength = weakest_of(strength, in->walk_strength);
        out->stay = in->stay && scale->stay && offset->stay;
        if (out->stay) execute();
    }
};

/* ---- the two standard tests -------------------------------------------------- */

/* re-plan a chain of equalities and pump edits through the extracted plan */
static int chain_test(int n) {
    Planner planner;
    Variable **v = (Variable **)malloc(sizeof(Variable *) * (n + 1));
    for (int i = 0; i <= n; i++) v[i] = new Variable(0);
    for (int i = 0; i < n; i++)
        (new EqualityConstraint(v[i], v[i + 1], REQUIRED))->add(&planner);
    (new StayConstraint(v[n], STRONG_DEFAULT))->add(&planner);

    EditConstraint *edit = new EditConstraint(v[0], PREFERRED);
    edit->add(&planner);
    List<Constraint> sources; sources.push(edit);
    List<Constraint> plan;
    planner.extract_plan_from(sources, plan);

    int ok = 1;
    for (long val = 1; val <= 100; val++) {
        v[0]->value = val;
        for (int i = 0; i < plan.count; i++) plan.items[i]->execute();
        if (v[n]->value != val) { fail("chain propagation"); ok = 0; break; }
    }
    edit->destroy(&planner);
    for (int i = 0; i <= n; i++) delete v[i];
    free(v);
    return ok;
}

/* scale/offset projection: edit the source, check dests; edit a dest, check src */
static int projection_test(int n) {
    Planner planner;
    Variable *scale = new Variable(10), *offset = new Variable(1000);
    Variable **src = (Variable **)malloc(sizeof(Variable *) * n);
    Variable **dst = (Variable **)malloc(sizeof(Variable *) * n);
    for (int i = 0; i < n; i++) {
        src[i] = new Variable(i);
        dst[i] = new Variable(i);
        (new StayConstraint(src[i], NORMAL))->add(&planner);
        (new ScaleConstraint(src[i], scale, offset, dst[i], REQUIRED))->add(&planner);
    }
    int ok = 1;

    /* set src[17] via an edit; dst[17] must follow */
    {
        EditConstraint *e = new EditConstraint(src[17], PREFERRED);
        e->add(&planner);
        List<Constraint> sources; sources.push(e);
        List<Constraint> plan;
        planner.extract_plan_from(sources, plan);
        src[17]->value = 17;
        for (int i = 0; i < plan.count; i++) plan.items[i]->execute();
        if (dst[17]->value != 1170) { fail("projection 1"); ok = 0; }
        e->destroy(&planner);
    }
    /* set dst[12] via an edit; src[12] must follow backward */
    if (ok) {
        EditConstraint *e = new EditConstraint(dst[12], PREFERRED);
        e->add(&planner);
        List<Constraint> sources; sources.push(e);
        List<Constraint> plan;
        planner.extract_plan_from(sources, plan);
        dst[12]->value = 1050;
        for (int i = 0; i < plan.count; i++) plan.items[i]->execute();
        if (src[12]->value != 5) { fail("projection 2"); ok = 0; }
        e->destroy(&planner);
    }
    /* move the offset via an edit; every dst with a stay-src must recompute */
    if (ok) {
        EditConstraint *e = new EditConstraint(offset, PREFERRED);
        e->add(&planner);
        List<Constraint> sources; sources.push(e);
        List<Constraint> plan;
        planner.extract_plan_from(sources, plan);
        offset->value = 2000;
        for (int i = 0; i < plan.count; i++) plan.items[i]->execute();
        for (int i = 0; i < n; i++)
            if (i != 12 && dst[i]->value != src[i]->value * 10 + 2000) {
                fail("projection 3"); ok = 0; break;
            }
        e->destroy(&planner);
    }
    for (int i = 0; i < n; i++) { delete src[i]; delete dst[i]; }
    free(src); free(dst);
    delete scale; delete offset;
    return ok;
}

static int deltablue_run(void) {
    bench_emit("deltablue: constraint solver, ");
    emit_u32(ITERATIONS); bench_emit(" iterations, ");
    emit_u32(N_VARS); bench_emit(" vars\n");
    for (int i = 0; i < ITERATIONS; i++) {
        if (!chain_test(N_VARS)) return 1;
        if (!projection_test(N_VARS)) return 1;
    }
    bench_emit("chain + projection verified every iteration\ndone.\n");
    return 0;
}

/* ------------------------------------------------------------ build shims --- */

#ifdef PURV_GUEST
extern "C" long host_write(int fd, const void *buf, long len);
static void bench_emit(const char *s) {
    long n = 0; while (s[n]) n++;
    host_write(1, s, n);
}
extern "C" int cmain(void) { return deltablue_run(); }
#else
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static void bench_emit(const char *s) { fputs(s, stdout); }
int main(void) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int rc = deltablue_run();
    clock_gettime(CLOCK_MONOTONIC, &b);
    fflush(stdout);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    fprintf(stderr, "BENCH wall_ms=%ld\n", ms);
    return rc;
}
#endif
