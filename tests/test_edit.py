"""End-to-end edit: change dialogue + add a script call in a .flow, rebuild, re-parse.

    python tests/test_edit.py <path-to-a-.bf-with-messages>
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ttre import setup
from ttre.flow import Flow
from ttre.msg2 import Msg2
from ttre.asm import disassemble, assemble
from ttre.commands import Commands
from ttre.decompile import decompile

setup(); C = Commands()
orig = open(sys.argv[1], "rb").read()
text = disassemble(Flow.parse(orig), C)
lines = text.splitlines()
# 1) rewrite the first dialogue page
i = next(k for k, l in enumerate(lines) if l.startswith("  \"") and "\\n" in l)
lines[i] = '  "Edited by ttre: this line is longer than the original one, to force every offset to move.\\n<wait>"'
# 2) add a 60-frame WAIT at the start of the first procedure
j = next(k for k, l in enumerate(lines) if l.startswith(".proc"))
assert lines[j + 1].strip() == "PROC"
lines[j + 2:j + 2] = ["    PUSHIS 60", "    COMM WAIT"]
new = assemble("\n".join(lines) + "\n", C).build()
f = Flow.parse(new)
assert f.build() == new, "rebuilt file does not re-parse to itself"
m = Msg2.parse(f.msg)
assert any(b"Edited by ttre" in p for msg in m.messages for p in msg.pages)
assert f.code[1][0] == "PUSHIS" and C.name(f.code[2][1]) == "WAIT"
# labels/procs must still point at the same instructions (shifted by the 2 new ones)
o = Flow.parse(orig)
assert [p[1] + (2 if p[1] > 0 else 0) for p in o.procs][1:] == [p[1] for p in f.procs][1:]
print(f"ok: {len(orig)} -> {len(new)} bytes")
print(decompile(f, C)[:300])
