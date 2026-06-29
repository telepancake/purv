#!/usr/bin/env python3
"""Drop the section-header table from a static ELF, in place.

A loader (ours, or Linux) executes an ELF using only its *program* headers; the
section-header table is link-time metadata. Removing it (zero e_shoff/e_shnum/
e_shstrndx and truncate to the end of the last PT_LOAD) yields a smaller but
still-valid, still-runnable ELF. Use only on a final, stripped executable.
"""
import struct, sys

f = sys.argv[1]
b = bytearray(open(f, "rb").read())
assert b[:4] == b"\x7fELF" and b[4] == 1, "not an ELF32"

(e_phoff,)      = struct.unpack_from("<I", b, 0x1c)
(e_phentsize,)  = struct.unpack_from("<H", b, 0x2a)
(e_phnum,)      = struct.unpack_from("<H", b, 0x2c)

end = e_phoff + e_phnum * e_phentsize           # at least the program headers
for i in range(e_phnum):
    p = e_phoff + i * e_phentsize
    p_type, p_offset = struct.unpack_from("<II", b, p)
    (p_filesz,)      = struct.unpack_from("<I", b, p + 16)
    if p_type == 1:                              # PT_LOAD
        end = max(end, p_offset + p_filesz)

struct.pack_into("<I", b, 0x20, 0)               # e_shoff
struct.pack_into("<H", b, 0x30, 0)               # e_shnum
struct.pack_into("<H", b, 0x32, 0)               # e_shstrndx
del b[end:]                                      # drop SHT + trailing padding

open(f, "wb").write(b)
print(f"tinyelf: {f} -> {len(b)} bytes")
