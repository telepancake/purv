#!/usr/bin/env python3
"""Flatten atoom headers into one baked, self-contained header (flag-aware).

Unlike a textual concat, this evaluates the baked RVE_E_* flags as it goes:
dead #if branches (e.g. all the RVE_E_HOOK and disabled-extension code) are
dropped, #include directives are only followed in live branches, and each
engine header is inlined at most once (include-guard semantics). The result is
emu/purv.h: RV32IMC + Zicsr + Zifencei, with everything else compiled out.
"""
import os, re, sys

INC = sys.argv[1]
OUT = sys.argv[2]

MACROS = {
    "RVE_E_M": 1, "RVE_E_C": 1, "RVE_E_ZICSR": 1, "RVE_E_ZIFENCEI": 1,
    "RVE_E_A": 0, "RVE_E_B": 0,
    "RVE_E_ZBA": 0, "RVE_E_ZBB": 0, "RVE_E_ZBC": 0, "RVE_E_ZBS": 0,
    "RVE_E_HOOK": 0,
}

# Files we provide ourselves; never inline upstream copies.
visited = {"RiscvEmulatorConfig.h", "RiscvEmulatorImplementationSpecific.h"}

inc_re   = re.compile(r'^\s*#\s*include\s+["<](RiscvEmulator[^">]+)[">]')
sysinc_re= re.compile(r'^\s*#\s*include\s+<([^>]+)>')
if_re    = re.compile(r'^\s*#\s*if\s+(.*)')
ifdef_re = re.compile(r'^\s*#\s*ifdef\s+(\w+)')
ifndef_re= re.compile(r'^\s*#\s*ifndef\s+(\w+)')
elif_re  = re.compile(r'^\s*#\s*elif\s+(.*)')
else_re  = re.compile(r'^\s*#\s*else\b')
endif_re = re.compile(r'^\s*#\s*endif\b')

def evaluate(expr):
    expr = expr.split("//")[0].split("/*")[0]
    expr = expr.replace("&&", " and ").replace("||", " or ")
    # substitute identifiers -> macro value or 0 (standard cpp behaviour)
    def sub(m):
        name = m.group(0)
        if name in ("and", "or", "not"):
            return name
        return str(MACROS.get(name, 0))
    expr = re.sub(r'[A-Za-z_]\w*', sub, expr)
    try:
        return bool(eval(expr, {"__builtins__": {}}, {}))
    except Exception as e:
        raise SystemExit(f"cannot evaluate #if expr {expr!r}: {e}")

define_re = re.compile(r'^\s*#\s*define\s+(\w+)(?:\s+(\S+))?')

def strip_guard(text):
    return re.sub(r'^\s*/\*.*?\*/\s*', '', text, count=1, flags=re.S)   # copyright only

def inline(name, out):
    if name in visited:
        return
    visited.add(name)
    with open(os.path.join(INC, name)) as f:
        text = strip_guard(f.read())
    out.append(f"/* ===== {name} ===== */")
    stack = []          # list of bools: is this branch active?
    taken = []          # has any branch in this if-group been taken?
    def active():
        return all(stack)
    for line in text.splitlines():
        m = if_re.match(line) or ifdef_re.match(line) or ifndef_re.match(line)
        if m:
            if if_re.match(line):
                val = evaluate(if_re.match(line).group(1)) if active() else False
            elif ifdef_re.match(line):
                val = (ifdef_re.match(line).group(1) in MACROS) if active() else False
            else:
                val = (ifndef_re.match(line).group(1) not in MACROS) if active() else False
            taken.append(val)
            stack.append(val)
            continue
        if elif_re.match(line):
            parent = all(stack[:-1])
            val = (parent and not taken[-1] and evaluate(elif_re.match(line).group(1)))
            stack[-1] = val
            taken[-1] = taken[-1] or val
            continue
        if else_re.match(line):
            parent = all(stack[:-1])
            stack[-1] = parent and not taken[-1]
            continue
        if endif_re.match(line):
            if stack:
                stack.pop(); taken.pop()
            continue
        if not active():
            continue
        mi = inc_re.match(line)
        if mi:
            inline(mi.group(1), out)
            continue
        if sysinc_re.match(line):
            continue
        md = define_re.match(line)
        if md:
            name, val = md.group(1), md.group(2)
            if val is not None:
                try: MACROS[name] = int(val, 0)
                except ValueError: pass
            # drop include-guard and baked-config defines; keep real constants
            if name.endswith("_H_") or name.startswith("RVE_E_"):
                continue
        out.append(line)

BAKED = """\
/* ---- Baked build configuration: RV32IMC + Zicsr + Zifencei ----------------
 * These flags are compiled in. Disabled-extension code (A, B/Zb*, the generic
 * RVE_E_HOOK instrumentation) has been stripped from this amalgamation, so
 * redefining them has no effect. To change the ISA, regenerate from upstream.
 */
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
"""

HOOKS = """\
/* ---- Implementation-specific hooks ----------------------------------------
 * Define these in your program (see purv.c). This is atoom's whole "API": the
 * engine reaches your memory map and trap policy only through these calls.
 */
void RiscvEmulatorLoad(uint32_t address, void *destination, uint8_t length);
void RiscvEmulatorStore(uint32_t address, const void *source, uint8_t length);
void RiscvEmulatorIllegalInstruction(RiscvEmulatorState_t *state);
void RiscvEmulatorUnknownCSR(RiscvEmulatorState_t *state);
void RiscvEmulatorHandleECALL(RiscvEmulatorState_t *state);
void RiscvEmulatorHandleEBREAK(RiscvEmulatorState_t *state);
"""

out = [
 "/*",
 " * purv.h - single-header RISC-V (RV32IMC + Zicsr/Zifencei) emulator core.",
 " *",
 " * Amalgamated from atoomnetmarc/RISC-V-emulator (Apache-2.0, (c) Marc Ketel),",
 " * pinned commit 633526d4, with the ISA flags baked in and dead branches",
 " * stripped. Define the implementation-specific hooks (see purv.c) then call",
 " * RiscvEmulatorInit() / RiscvEmulatorLoop().",
 " */",
 "#ifndef PURV_H_",
 "#define PURV_H_",
 "",
 "#include <stdint.h>",
 "#include <string.h>",
 "",
 BAKED,
 "",
 "/* The engine passes a hook-context pointer to many helpers; with RVE_E_HOOK",
 " * baked off it is unused. Scope the warning away for the vendored engine only",
 " * (mirrors upstream's own pragma around RiscvEmulatorHook). */",
 '#pragma GCC diagnostic push',
 '#pragma GCC diagnostic ignored "-Wunused-parameter"',
]
inline("RiscvEmulatorType.h", out)
inline("RiscvEmulatorTypeHook.h", out)
out.append("")
out.append(HOOKS)
inline("RiscvEmulatorDefine.h", out)
inline("RiscvEmulatorHook.h", out)
inline("RiscvEmulatorExtension.h", out)
inline("RiscvEmulatorTrap.h", out)
inline("RiscvEmulator.h", out)
out.append("")
out.append("#pragma GCC diagnostic pop")
out.append("")
out.append("#endif /* PURV_H_ */")

# collapse 3+ blank lines to 1 for readability
text = "\n".join(out) + "\n"
text = re.sub(r'\n{3,}', '\n\n', text)
with open(OUT, "w") as f:
    f.write(text)
print("wrote", OUT, "lines:", text.count("\n"))
allh = {f for f in os.listdir(INC) if f.endswith(".h")}
print("not inlined:", sorted(allh - visited))
