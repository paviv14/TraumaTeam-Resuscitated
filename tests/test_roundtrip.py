"""Rebuild every script and message file from the game and check it is byte-identical.

    python -m tests.test_roundtrip <path-to-extracted-files>
"""
import sys, glob, os, collections
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ttre.flow import Flow
from ttre.msg2 import Msg2
from ttre.asm import disassemble, assemble
from ttre.commands import Commands
from ttre import setup


def main(root):
    setup()
    C = Commands()
    res = collections.Counter(); fails = []
    for fn in sorted(glob.glob(os.path.join(root, "**", "*.bf"), recursive=True)):
        b = open(fn, "rb").read()
        try:
            ok = assemble(disassemble(Flow.parse(b), C), C).build() == b
        except Exception as e:
            ok = False; fn += f"  ({e!r})"
        res["bf ok" if ok else "bf FAIL"] += 1
        if not ok: fails.append(fn)
    for fn in sorted(glob.glob(os.path.join(root, "**", "*.bm2"), recursive=True)):
        b = open(fn, "rb").read()
        try:
            ok = Msg2.from_text(Msg2.parse(b).to_text().splitlines()).build() == b
        except Exception as e:
            ok = False; fn += f"  ({e!r})"
        res["bm2 ok" if ok else "bm2 FAIL"] += 1
        if not ok: fails.append(fn)
    print(dict(res))
    for f in fails[:20]:
        print("FAIL", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
