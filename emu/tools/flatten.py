#!/usr/bin/env python3
"""Generate the purv engine as a real header + implementation pair.

atoom ships header-only (~45 self-guarded headers of static-inline code). This
splits it into a conventional library:

  purv.h   interface: baked config, all types, memory-map origins, and the
           public prototypes (RiscvEmulatorInit/Loop + the hooks you provide).
  purv.c   implementation: every instruction-execution body, #include "purv.h".

Both are produced by flattening atoom in dependency order while evaluating the
baked RVE_E_* flags, so disabled extensions (A, B/Zb*) and the RVE_E_HOOK
instrumentation are stripped out entirely. The type closure routes to purv.h;
everything else (decode macros, extensions, trap, Init/Loop) routes to purv.c.

  usage: flatten.py <atoom/include> <out_dir>
"""
import os, re, sys

INC = sys.argv[1]
OUTDIR = sys.argv[2]

MACROS = {
    "RVE_E_M": 1, "RVE_E_C": 1, "RVE_E_ZICSR": 1, "RVE_E_ZIFENCEI": 1,
    "RVE_E_A": 0, "RVE_E_B": 0,
    "RVE_E_ZBA": 0, "RVE_E_ZBB": 0, "RVE_E_ZBC": 0, "RVE_E_ZBS": 0,
    "RVE_E_HOOK": 0,
}

# Memory-map origins live in the public header (the host needs RAM_ORIGIN); the
# implementation must not redefine them.
ORIGIN_DEFINES = {"IALIGN", "IO_ORIGIN", "UART_ORIGIN", "ROM_ORIGIN", "RAM_ORIGIN"}

inc_re    = re.compile(r'^\s*#\s*include\s+["<](RiscvEmulator[^">]+)[">]')
sysinc_re = re.compile(r'^\s*#\s*include\s+<([^>]+)>')
if_re     = re.compile(r'^\s*#\s*if\s+(.*)')
ifdef_re  = re.compile(r'^\s*#\s*ifdef\s+(\w+)')
ifndef_re = re.compile(r'^\s*#\s*ifndef\s+(\w+)')
elif_re   = re.compile(r'^\s*#\s*elif\s+(.*)')
else_re   = re.compile(r'^\s*#\s*else\b')
endif_re  = re.compile(r'^\s*#\s*endif\b')
define_re = re.compile(r'^\s*#\s*define\s+(\w+)(?:\s+(\S+))?')

def evaluate(expr):
    expr = expr.split("//")[0].split("/*")[0]
    expr = expr.replace("&&", " and ").replace("||", " or ")
    expr = re.sub(r'[A-Za-z_]\w*',
                  lambda m: m.group(0) if m.group(0) in ("and", "or", "not")
                  else str(MACROS.get(m.group(0), 0)), expr)
    try:
        return bool(eval(expr, {"__builtins__": {}}, {}))
    except Exception as e:
        raise SystemExit(f"cannot evaluate #if {expr!r}: {e}")

def strip_copyright(text):
    return re.sub(r'^\s*/\*.*?\*/\s*', '', text, count=1, flags=re.S)

def inline(name, out, visited, drop_origin_defines=False):
    if name in visited:
        return
    visited.add(name)
    with open(os.path.join(INC, name)) as f:
        text = strip_copyright(f.read())
    out.append(f"/* ===== {name} ===== */")
    stack, taken = [], []
    active = lambda: all(stack)
    for line in text.splitlines():
        m = if_re.match(line) or ifdef_re.match(line) or ifndef_re.match(line)
        if m:
            if if_re.match(line):
                val = evaluate(if_re.match(line).group(1)) if active() else False
            elif ifdef_re.match(line):
                val = (ifdef_re.match(line).group(1) in MACROS) if active() else False
            else:
                val = (ifndef_re.match(line).group(1) not in MACROS) if active() else False
            taken.append(val); stack.append(val); continue
        if elif_re.match(line):
            parent = all(stack[:-1])
            val = parent and not taken[-1] and evaluate(elif_re.match(line).group(1))
            stack[-1] = val; taken[-1] = taken[-1] or val; continue
        if else_re.match(line):
            stack[-1] = all(stack[:-1]) and not taken[-1]; continue
        if endif_re.match(line):
            if stack: stack.pop(); taken.pop()
            continue
        if not active():
            continue
        mi = inc_re.match(line)
        if mi:
            inline(mi.group(1), out, visited, drop_origin_defines); continue
        if sysinc_re.match(line):
            continue
        md = define_re.match(line)
        if md:
            name2, val = md.group(1), md.group(2)
            if val is not None:
                try: MACROS[name2] = int(val, 0)
                except ValueError: pass
            if name2.endswith("_H_") or name2.startswith("RVE_E_"):
                continue
            if drop_origin_defines and name2 in ORIGIN_DEFINES:
                continue
        out.append(line)

BANNER_H = """\
/*
 * purv.h - RISC-V (RV32IMC + Zicsr/Zifencei) emulator: public interface.
 *
 * Amalgamated from atoomnetmarc/RISC-V-emulator (Apache-2.0, (c) Marc Ketel),
 * pinned commit 633526d4. Types + config + prototypes; the bodies live in
 * purv.c. The flags below are baked into both files (disabled-extension and
 * RVE_E_HOOK code is stripped), so redefining them has no effect.
 */
#ifndef PURV_H_
#define PURV_H_

#include <stdint.h>
#include <string.h>

#define RVE_E_M        1
#define RVE_E_C        1
#define RVE_E_ZICSR    1
#define RVE_E_ZIFENCEI 1
#define RVE_E_A        0
#define RVE_E_B        0
#define RVE_E_ZBA      0
#define RVE_E_ZBB      0
#define RVE_E_ZBC      0
#define RVE_E_ZBS      0
#define RVE_E_HOOK     0

/* Architectural memory-map origins (resolved for RV32IMC). */
#define IALIGN      16
#define IO_ORIGIN   0x02000000
#define UART_ORIGIN 0x10000000
#define ROM_ORIGIN  0x20000000
#define RAM_ORIGIN  0x80000000
"""

HOOKS = """\
/* ---- Implementation-specific hooks ----------------------------------------
 * Define these in your host (see main.c). This is atoom's whole "API": the
 * engine reaches your memory map and trap policy only through these calls.
 */
void RiscvEmulatorLoad(uint32_t address, void *destination, uint8_t length);
void RiscvEmulatorStore(uint32_t address, const void *source, uint8_t length);
void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *state);
void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *state);
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *state);
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *state);

/* ---- Public API ---- */
void RiscvEmulatorInit(RiscvEmulatorState_t *state, uint32_t ram_length);
void RiscvEmulatorLoop(RiscvEmulatorState_t *state);
"""

# ---- purv.h : config + memory map + type closure + prototypes -------------
h = [BANNER_H]
hv = {"RiscvEmulatorConfig.h", "RiscvEmulatorImplementationSpecific.h"}
inline("RiscvEmulatorType.h", h, hv)
inline("RiscvEmulatorTypeHook.h", h, hv)
h.append("")
h.append(HOOKS)
h.append("#endif /* PURV_H_ */")

# ---- purv.c : decode macros + extensions + trap + Init/Loop bodies ---------
BANNER_C = """\
/*
 * purv.c - RISC-V (RV32IMC + Zicsr/Zifencei) emulator: implementation.
 *
 * Generated from atoomnetmarc/RISC-V-emulator (Apache-2.0, (c) Marc Ketel),
 * pinned commit 633526d4. Instruction-execution bodies for purv.h. Build this
 * together with your host (e.g. main.c). See tools/flatten.py.
 */
#include "purv.h"

/* The engine threads a hook-context pointer through many helpers; with
 * RVE_E_HOOK baked off it is unused. Scope the warning away for this generated
 * file (mirrors upstream's own pragma around RiscvEmulatorHook). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
"""
c = [BANNER_C]
# Pre-mark the header-provided files visited so the impl doesn't re-emit them.
cv = {"RiscvEmulatorConfig.h", "RiscvEmulatorImplementationSpecific.h"}
cv |= {f for f in os.listdir(INC) if f.startswith("RiscvEmulatorType")}
inline("RiscvEmulator.h", c, cv, drop_origin_defines=True)
c.append("")
c.append("#pragma GCC diagnostic pop")

def write(path, lines):
    text = re.sub(r'\n{3,}', '\n\n', "\n".join(lines) + "\n")
    with open(path, "w") as f:
        f.write(text)
    return text.count("\n")

hlines = write(os.path.join(OUTDIR, "purv.h"), h)

# Give Init/Loop external linkage so the host can call them across TUs.
ctext = re.sub(r'\n{3,}', '\n\n', "\n".join(c) + "\n")
ctext = ctext.replace("static inline void RiscvEmulatorInit(", "void RiscvEmulatorInit(")
ctext = ctext.replace("static inline void RiscvEmulatorLoop(", "void RiscvEmulatorLoop(")
with open(os.path.join(OUTDIR, "purv.c"), "w") as f:
    f.write(ctext)

print("wrote purv.h:", hlines, "lines; purv.c:", ctext.count("\n"), "lines")
allh = {f for f in os.listdir(INC) if f.endswith(".h")}
print("not inlined:", sorted(allh - (hv | cv)))
