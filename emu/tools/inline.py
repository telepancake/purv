#!/usr/bin/env python3
"""Inline purv.c's single-call-site static handlers into the public API functions.

The flattened engine is a pure tree: every `static void RiscvEmulator*` handler
is called exactly once, so the Loop dispatches down two or three levels of tiny
functions. This pass collapses that tree -- each call `H(args);` is replaced by
H's body (parameters substituted by the call arguments; a `return;` becomes a
`goto` to a unique label at the end of the inlined block, so it exits the handler
correctly even from inside a nested switch/loop) and H's definition is removed --
until only the non-static public functions remain (chiefly one top-down
RiscvEmulatorLoop).

It is a behavior-preserving source transform (verified against the riscv-tests
conformance suite). Output is not pretty; run tools/compact.py afterwards to
reformat. Used by `make regen`.
"""
import re, sys

IDENT = r'[A-Za-z_]\w*'

def find_funcs(src):
    """Return list of dicts for every top-level RiscvEmulator function definition."""
    funcs = []
    for m in re.finditer(r'^(static\s+)?([A-Za-z_][\w ]*?[\w*])\s+(RiscvEmulator\w+)\s*\(([^;{]*?)\)\s*\{',
                         src, re.M):
        if m.group(1) is None:
            # non-static (public) -- record but never inline/remove it
            pass
        body_start = m.end()                       # just after '{'
        depth, i = 1, body_start
        while depth and i < len(src):
            c = src[i]
            if c == '{': depth += 1
            elif c == '}': depth -= 1
            i += 1
        funcs.append(dict(static=bool(m.group(1)), ret=m.group(2).strip(),
                          name=m.group(3), params=m.group(4).strip(),
                          start=m.start(), body=src[body_start:i-1], end=i))
    return funcs

def param_map(params, args):
    """Map each parameter name to the call argument, casting *scalar* params to
    their declared type so the implicit argument->parameter conversion that a real
    call performs is preserved (e.g. int16_t imm passed to a uint32_t parameter).
    Pointer params need no cast -- the handler casts at each use."""
    if params.strip() in ('', 'void'):
        return {}
    pmap = {}
    for decl, arg in zip(split_commas(params), args):
        arg = arg.strip()
        simple = re.match(r'^[A-Za-z_]\w*$', arg)  # a bare identifier needs no parens
        name = re.findall(IDENT, decl)[-1]
        ptype = re.sub(r'\b' + name + r'\b', '', decl).replace('const', '').strip()
        if '*' in decl:                            # pointer: no cast (handler casts at use)
            pmap[name] = arg if simple else '(' + arg + ')'
        else:                                      # scalar: cast to the param type
            pmap[name] = '(%s)%s' % (ptype, arg) if simple else '((%s)(%s))' % (ptype, arg)
    return pmap

def split_commas(s):
    """Split on top-level commas (respecting parens)."""
    out, depth, cur = [], 0, ''
    for c in s:
        if c == '(' : depth += 1
        elif c == ')': depth -= 1
        if c == ',' and depth == 0:
            out.append(cur); cur = ''
        else:
            cur += c
    if cur.strip() != '' or out:
        out.append(cur)
    return [x.strip() for x in out]

def find_call(src, name, search_from=0):
    """Find a statement-form call `name(args);` -> (stmt_start, stmt_end, args_list)."""
    for m in re.finditer(r'\b' + re.escape(name) + r'\s*\(', src):
        if m.start() < search_from:
            continue
        i = m.end()                                # just after '('
        depth, args = 1, ''
        while depth and i < len(src):
            c = src[i]
            if c == '(': depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0: break
            args += c
            i += 1
        j = i + 1
        while j < len(src) and src[j] in ' \t\n': j += 1
        if j < len(src) and src[j] == ';':         # statement call
            return m.start(), j + 1, split_commas(args)
    return None

def substitute(body, pmap):
    """Simultaneously replace whole-word param names with their argument text."""
    if not pmap:
        return body
    rx = re.compile(r'\b(' + '|'.join(re.escape(k) for k in pmap) + r')\b')
    return rx.sub(lambda m: pmap[m.group(1)], body)

LABEL = 0

def match_leading_guard(body):
    """If body is `if (COND) { return; } REST` (or `if (COND) return; REST`),
    return (COND, REST); else None. Handles a COND with nested parens."""
    s = body.lstrip()
    if not re.match(r'if\s*\(', s):
        return None
    i = s.index('(')
    depth, j = 0, i
    while j < len(s):
        if s[j] == '(': depth += 1
        elif s[j] == ')':
            depth -= 1
            if depth == 0: break
        j += 1
    cond = s[i + 1:j]
    rest = s[j + 1:].lstrip()
    if rest.startswith('{'):
        depth, k = 0, 0
        while k < len(rest):
            if rest[k] == '{': depth += 1
            elif rest[k] == '}':
                depth -= 1
                if depth == 0: break
            k += 1
        guard, after = rest[1:k].strip(), rest[k + 1:]
    elif ';' in rest:
        semi = rest.index(';')
        guard, after = rest[:semi].strip(), rest[semi + 1:]
    else:
        return None
    return (cond, after) if guard.rstrip(' ;') == 'return' else None

def wrap(body):
    """Turn a handler body into a block to splice at the call site.

    The common case is a leading `if (x0-guard) return;` followed by the write:
    invert it to `if (!guard) { write }` -- no goto. Otherwise, if a return
    remains (the dispatchers, whose returns sit inside a switch), send each to a
    unique end label via goto so it exits from any depth. A plain block otherwise."""
    global LABEL
    g = match_leading_guard(body)
    if g and not re.search(r'\breturn\b', g[1]):
        cond, after = g[0].strip(), g[1].strip()
        if cond.endswith('== 0'):   inv = cond[:-4].rstrip() + ' != 0'
        elif cond.endswith('!= 0'): inv = cond[:-4].rstrip() + ' == 0'
        else:                       inv = '!(' + cond + ')'
        # the `if` is itself a single statement and scopes `after`, so no outer
        # block is needed; clang-format then drops the inner braces if it can.
        return 'if (%s) { %s }' % (inv, after)
    if re.search(r'\breturn\s*;', body):
        LABEL += 1
        lbl = '__inl_%d' % LABEL
        body = re.sub(r'\breturn\s*;', 'goto %s;' % lbl, body)
        return '{' + body + '\n' + lbl + ':;}'
    return '{' + body + '}'

def inline_once(src):
    """Inline one leaf static handler; return (new_src, changed)."""
    funcs = find_funcs(src)
    by_name = {f['name']: f for f in funcs}
    static_names = {f['name'] for f in funcs if f['static']}
    # a leaf static handler calls no other static handler
    for f in funcs:
        if not f['static']:
            continue
        if any(re.search(r'\b' + re.escape(n) + r'\s*\(', f['body']) for n in static_names if n != f['name']):
            continue                               # not a leaf yet
        # find its single call site anywhere in the file (outside its own definition)
        call = None
        for region_start in (f['end'], 0):
            c = find_call(src, f['name'], region_start)
            if c and not (f['start'] <= c[0] < f['end']):
                call = c; break
        if not call:
            continue                               # unreferenced (e.g. the entry) -- skip
        s0, s1, args = call
        pmap = param_map(f['params'], args)
        body = substitute(f['body'], pmap)
        block = wrap(body)
        # splice: remove the function definition and replace the call with the block.
        # do the later edit first so offsets stay valid
        edits = sorted([(f['start'], f['end'], ''), (s0, s1, block)], reverse=True)
        for a, b, rep in edits:
            src = src[:a] + rep + src[b:]
        return src, True
    return src, False

def main(path):
    src = open(path).read()
    n = 0
    while True:
        src, changed = inline_once(src)
        if not changed: break
        n += 1
    open(path, 'w').write(src)
    print(f"inlined {n} handlers into {path}")

if __name__ == '__main__':
    for p in sys.argv[1:]:
        main(p)
