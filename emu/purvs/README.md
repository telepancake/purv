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

Propagation is **operation-aware** — only additive offsetting keeps a pointer:

```
  result of rd, by op and operand provenance (N=int, P=pointer, B=bad):

  op           N,N | P,N  | N,P  | P,P same | P,P diff | any B
  ------------ ----+------+------+----------+----------+------
  add           N  |  P   |  P   |    B     |    B     |   B
  sub (a-b)     N  |  P   |  B   |    N     |    B     |   B
  slt/sltu      N  |  N   |  N   |    N     |    B     |   B
  and/or/xor    N  | P/B* | P/B* |    B     |    B     |   B
  sll/srl/sra   N  |  B   |  B   |    B     |    B     |   B
  mul/div/rem   N  |  B   |  B   |    B     |    B     |   B
```

Reasoning per op:
- **add**: `ptr + int` is a valid offset (`P`); `ptr + ptr` is meaningless (`B`).
- **sub** (non-commutative): `ptr - int` is an offset (`P`); `int - ptr` is
  nonsense (`B`); `ptr - ptr` same object is a scalar difference (`N`), across
  objects is `B`.
- **slt/sltu**: the result is a boolean (`N`), unless it compares two different
  objects, which is meaningless (`B`).
- **and/or/xor** (`P/B*`): masking a pointer with a constant keeps the tag **iff
  the constant only touches the low (page-local) bits** — i.e. alignment
  (`p & ~15`), or setting/toggling low bits — since that just adjusts the address
  and the dereference is still bounds-checked. Touching high bits is forging
  (`B`); mixing two pointers' bits is `B`.
- **shifts, mul/div/rem**: not addressing — any pointer in poisons the result
  (`B`), even though the address may still look valid.
- **B is sticky**: any bad operand gives `B`.

## Memory and the callbacks

The engine has no memory; it reaches the memory system only through three hooks:
`RiscvEmulatorFetch` (instruction read), `RiscvEmulatorLoad` (data read),
`RiscvEmulatorStore` (data write). **The tag of a memory cell is the memory
system's business, learned by asking on load and recorded on store — it is never
maintained by the emulator.** Tagged memory (`g_mem_tag`, one tag per word) lives
behind those callbacks and is touched only there.

The register tags are the driver's parallel datapath. At a load/store the driver
hands the memory system the base-pointer's tag (for the check) and, on a store,
the value's tag (to record on the cell); a load returns the cell's tag back into
the destination register. The engine computes the real effective address and
passes it to the callback — the driver never recomputes it. So a pointer stored
to memory and loaded back recovers its tag, while bytes the program never wrote
simply read as `NOTAG` (the memory system's initial state, not a guess).

## Checks

`malloc` is a syscall that allocates, records `[base,size)`, and returns a
pointer **with a fresh object tag**. `free` retires the object. Inside the
load/store callback the memory system checks the base pointer's tag against the
object *before* performing the access:

- `NOTAG` → allowed (untracked memory: stack, globals, MMIO)
- object id → must be live and the access must fall within `[base,size)`
- `BAD` → rejected

So out-of-bounds, cross-object, and use-after-free accesses are all caught, and a
violation refuses the access (the load fills zero / the store is dropped).

## Demo

```sh
make test
```

```
== ok ==       in-bounds writes succeeded                         exit=0
== diff ==     same-object difference is a plain count; loop ok    exit=0
== align ==    (p & ~15) keeps the tag; in-bounds write ok         exit=0
== oob ==      out of bounds store ... object 1: [..,..)           exit=134
== uaf ==      use-after-free / dead object store ... (freed)      exit=134
== subdiff ==  ptr - ptr across objects -> bad scalar -> caught     exit=134
== addp ==     ptr + ptr -> bad -> caught                           exit=134
== rsub ==     int - ptr -> bad -> caught                           exit=134
== scale ==    shifted pointer (valid address) -> bad -> caught     exit=134
== cross ==    pointer built from two objects -> bad -> caught      exit=134
```

## Notes / limits

- The shadow datapath decodes 32-bit instructions, so codelets are built
  `-march=rv32im` (no compressed). The engine still executes RV32IMC.
- The per-op rules live in `alu_add` / `alu_sub` / `alu_cmp` / `alu_bitwise` /
  `alu_other` in main.c, dispatched by funct3/funct7. The table is the contract.
- "Page-local" for and/or/xor is the low `PAGE_BITS` (12 -> 4 KB) bits, which
  covers any realistic alignment; a larger alignment would need a bigger bound.
- Tags are word-granular in memory; sub-word stores clear the word's tag.
- This is a prototype: the safety is entirely host-side precisely because the
  engine is unmodified. Moving tags into the engine (tag-aware instructions,
  hardware "bad tag" faults) is the natural next step.
