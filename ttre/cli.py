"""Command line:  python -m ttre <command> ...

  extract  <game_files_dir> <out_dir>   every .bf -> .flow (editable) + .c (readable) ; every .bm2 -> .msg2.txt
  build    <in.flow|in.msg2.txt> <out>  rebuild a .bf / .bm2 from its text form
  decompile <in.bf>                      print readable pseudo-code
  disasm   <in.bf>                       print the editable .flow text
  verify   <game_files_dir>              check every file rebuilds byte-identical
  stats    <game_files_dir>              command usage counts across all scripts
"""
import collections
import glob
import os
import sys

from . import setup
from .asm import assemble, disassemble
from .commands import Commands
from .decompile import decompile
from .flow import Flow
from .msg2 import Msg2


def _files(root, ext):
    return sorted(glob.glob(os.path.join(root, "**", "*" + ext), recursive=True))


def cmd_extract(root, out):
    C = Commands()
    n = errs = 0
    for fn in _files(root, ".bf"):
        rel = os.path.relpath(fn, root)[:-3]
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        f = Flow.parse(open(fn, "rb").read())
        open(dst + ".flow", "w", encoding="utf-8").write(disassemble(f, C))
        try:
            open(dst + ".c", "w", encoding="utf-8").write(f"// decompiled from {rel}.bf (read-only view)\n" + decompile(f, C))
        except Exception as e:  # never let the readable view block the editable one
            errs += 1
            open(dst + ".c", "w", encoding="utf-8").write(f"// decompile failed: {e!r}\n")
        n += 1
    for fn in _files(root, ".bm2"):
        rel = os.path.relpath(fn, root)[:-4]
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst + ".msg2.txt", "w", encoding="utf-8").write(Msg2.parse(open(fn, "rb").read()).to_text())
        n += 1
    print(f"extracted {n} files to {out}" + (f" ({errs} decompile errors)" if errs else ""))


def cmd_build(src, out):
    text = open(src, encoding="utf-8").read()
    if src.endswith(".flow"):
        data = assemble(text, Commands()).build()
    else:
        data = Msg2.from_text(text.splitlines()).build()
    open(out, "wb").write(data)
    print(f"wrote {out} ({len(data)} bytes)")


def cmd_stats(root):
    C = Commands()
    use = collections.Counter(); files = collections.Counter()
    for fn in _files(root, ".bf"):
        f = Flow.parse(open(fn, "rb").read())
        seen = set()
        for name, arg, _ in f.code:
            if name == "COMM":
                use[arg] += 1; seen.add(arg)
        for a in seen:
            files[a] += 1
    print("id     uses   files  name")
    for a, c in use.most_common():
        print(f"0x{a:03X} {c:6d} {files[a]:6d}  {C.name(a)}")


def main(argv=None):
    argv = argv or sys.argv[1:]
    setup()
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__); return 0
    cmd, args = argv[0], argv[1:]
    if cmd == "extract": cmd_extract(*args)
    elif cmd == "build": cmd_build(*args)
    elif cmd == "decompile": print(decompile(Flow.parse(open(args[0], "rb").read()), Commands()), end="")
    elif cmd == "disasm": print(disassemble(Flow.parse(open(args[0], "rb").read()), Commands()), end="")
    elif cmd == "verify":
        sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        from tests.test_roundtrip import main as v
        return v(args[0])
    elif cmd == "stats": cmd_stats(*args)
    else:
        print(__doc__); return 1
    return 0
