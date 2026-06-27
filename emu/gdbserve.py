#!/usr/bin/env python3
"""Connection broker for purv's gdb stub.

purv has no listening code of its own: it only serves the GDB Remote Serial
Protocol on a file descriptor handed to it (`--gdb=FD`). This helper does the
listening, accepts one gdb, then execs purv with the connected socket as that
fd. The guest's console output stays on this terminal; only the RSP rides the
socket.

    ./gdbserve.py <port> -- ./purv --user examples/hello.elf

Then, in a riscv-capable gdb (gdb-multiarch, or a riscv*-gdb):

    (gdb) file examples/hello.elf
    (gdb) target remote :<port>
"""
import os
import socket
import sys

if "--" not in sys.argv[2:]:
    sys.exit("usage: gdbserve.py <port> -- <purv> [args...]")

port = int(sys.argv[1])
cmd = sys.argv[sys.argv.index("--") + 1:]
if not cmd:
    sys.exit("usage: gdbserve.py <port> -- <purv> [args...]")

ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("127.0.0.1", port))
ls.listen(1)
print(f"gdbserve: listening on :{port} -- in gdb: target remote :{port}",
      file=sys.stderr)

conn, _ = ls.accept()
ls.close()
os.set_inheritable(conn.fileno(), True)        # survive the exec
os.execvp(cmd[0], cmd + [f"--gdb={conn.fileno()}"])
