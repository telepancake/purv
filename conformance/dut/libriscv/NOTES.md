# libriscv — tested, structurally incompatible with the bare-metal ACT4 suite

**Result: libriscv cannot run the ACT4 conformance suite.** This is a property of
what libriscv *is*, not a gap in the adapter — so there is no DUT adapter here,
just this finding and a reproducer (`scripts/run-libriscv.sh`).

libriscv ([libriscv/libriscv](https://github.com/libriscv/libriscv)) is a
**userspace sandbox**: it runs Linux-style RISC-V ELF programs with syscall
emulation, optimised for ultra-low-latency embedding. The ACT4 architectural
tests are the opposite environment — **bare-metal, machine-mode** programs that
set up trap vectors and exercise privileged CSRs. The two don't meet:

1. **Rejects the ELF outright.** libriscv's loader enforces W^X:
   ```
   $ rvlinux I-add-00.elf
   Exception: Insecure ELF has writable executable code
   ```
   ACT4 ELFs link code+data together at `0x80000000` (RWX), which a hardened
   userspace sandbox refuses to load. The CLI has no flag to disable this.

2. **No machine-mode CSRs.** `lib/libriscv/machine.cpp` implements only userspace
   CSRs — `fflags/frm/fcsr`, `cycle/time/instret`, `marchid/mvendorid`. Any
   access to `mstatus` (0x300), `mtvec` (0x305), `mepc` (0x341), `mcause`
   (0x342)… falls through to `trigger_exception(UNIMPLEMENTED/ILLEGAL)`. The ACT4
   prologue programs `mtvec` before the first test instruction, so it faults
   immediately.

3. **ECALL is a Linux syscall, not a trap.** libriscv dispatches `ecall` to its
   `system_call` handler (exit, write, …); it does **not** vector to `mtvec`.
   Exception/trap tests therefore have no mechanism to work.

4. **No memory-mapped HTIF/UART/SYSCON.** There is no device space to host the
   console / termination protocol the other DUT adapters rely on.

Making libriscv pass these tests would mean adding M-mode, a trap-vectoring CSR
file, and a device bus to libriscv itself — i.e. turning a userspace sandbox into
a system emulator. That is out of scope (and counter to libriscv's purpose).

**How libriscv *should* be conformance-tested:** as a userspace RV executor —
run RV32/RV64 user-mode ELF binaries (libriscv ships its own `tests/`), not the
bare-metal architectural suite. The architectural suite suits system/bare-metal
DUTs (Spike, Sail, TinyEMU, mini-rv32ima, atoomnetmarc).
