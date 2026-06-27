# purvs — secure (tagged-memory) purv

A pointer-safety variant of purv. The engine (`purvs.c`/`purvs.h`) is an
unmodified copy of purv — **the CPU has no idea tags exist**. All the safety
lives in a parallel *shadow datapath* in `main.c`: every register and every
memory word carries a tag the program can't see or touch.

```
purvs/
  purvs.h, purvs.c   the engine (a copy of purv; tag-oblivious)
  main.c             the secure host: shadow tags + object table + checks
  examples/safe.c    demo: ok / oob / uaf / cross
```

## Tags

| tag        | meaning                                                        |
|------------|---------------------------------------------------------------|
| `NOTAG`    | value the CPU made up — immediates, `sp`, arithmetic of untagged |
| object id  | a pointer into a specific allocation (handed out by `malloc`)  |
| `BAD`      | corrupted provenance (e.g. a pointer built from two objects)   |

Propagation, decoded in parallel for each instruction:

```
tag  op  NOTAG      -> tag        (pointer +/- integer offset stays in-object)
tag  op  same tag   -> tag
tag  op  other tag  -> BAD        (cross-object arithmetic)
BAD  op  anything   -> BAD        (sticky)
```

Tags ride through memory too: storing a pointer tags that memory word; loading
it back recovers the tag.

## Checks

`malloc` is a syscall that allocates, records `[base,size)`, and returns a
pointer **with a fresh object tag**. `free` retires the object. On every
load/store the host checks the base pointer's tag *before* the engine touches
memory:

- `NOTAG` → allowed (untracked memory: stack, globals)
- object id → must be live and the access must fall within `[base,size)`
- `BAD` → rejected

So out-of-bounds, cross-object, and use-after-free accesses are all caught.

## Demo

```sh
make test
```

```
== ok ==     ok: in-bounds writes succeeded                       exit=0
== oob ==    out of bounds store ... object 1: [..,..)            exit=134
== uaf ==    use-after-free / dead object store ... (freed)       exit=134
== cross ==  bad-provenance pointer store ... pointer tag: BAD    exit=134
```

## Notes / limits

- The shadow datapath decodes 32-bit instructions, so codelets are built
  `-march=rv32im` (no compressed). The engine still executes RV32IMC.
- Tag combine treats same-object arithmetic as valid and different-object as
  `BAD` (provenance), which is what catches cross-object use; a stricter
  "any two tags → BAD" is a one-line change in `tag_combine`.
- Tags are word-granular in memory; sub-word stores clear the word's tag.
- This is a prototype: the safety is entirely host-side precisely because the
  engine is unmodified. Moving tags into the engine (tag-aware instructions,
  hardware "bad tag" faults) is the natural next step.
