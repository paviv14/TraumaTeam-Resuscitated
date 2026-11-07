"""Text form of a flowscript (.flow), lossless in both directions.

    .flow type=0 compressed=0 user=0 unk0c=0 locals_int=1 locals_float=0 unk18=0000000000000000
    .proc #0 test
        PUSHIS 2
        COMM RUMBLE_START
    .label #0 _1
        GOTO _1
    .strings size=240
    .str @0 "di_step"
    .messages
    ...MSG2 text (see msg2.py)...
    .endmsg2

Procedures and labels carry their table index (#n) because the binary tables are indexed.
Unknown commands are written as COMM 0x1A2; once named in data/commands.csv they print by name.
"""
import struct
from .flow import Flow, OPS, LABEL_OPS, PROC_OPS
from .msg2 import Msg2


def _q(b):
    out = []; i = 0
    while i < len(b):
        c = b[i]
        if 0x20 <= c < 0x7F:
            out.append({"\\": "\\\\", '"': '\\"'}.get(chr(c), chr(c))); i += 1; continue
        if (0x81 <= c <= 0x9F or 0xE0 <= c <= 0xFC) and i + 1 < len(b):
            try:
                out.append(b[i:i + 2].decode("cp932")); i += 2; continue
            except UnicodeDecodeError:
                pass
        out.append(f"\\x{c:02X}"); i += 1
    return '"' + "".join(out) + '"'


def _unq(s):
    assert s[0] == '"' and s[-1] == '"', s
    s = s[1:-1]
    out = bytearray(); i = 0
    while i < len(s):
        if s[i] == "\\":
            nx = s[i + 1]
            if nx == "x": out.append(int(s[i + 2:i + 4], 16)); i += 4; continue
            out += nx.encode("cp932"); i += 2; continue
        out += s[i].encode("cp932"); i += 1
    return bytes(out)


def _f32(word):
    v = struct.unpack(">f", struct.pack(">I", word))[0]
    r = repr(v)
    if struct.unpack(">I", struct.pack(">f", float(r)))[0] == word:
        return r
    return f"0x{word:08X}"


def disassemble(f: Flow, cmds) -> str:
    L = [f".flow type={f.file_type} compressed={f.compressed} user={f.user_id} unk0c={f.unk0c} "
         f"locals_int={f.local_int} locals_float={f.local_float} unk18={f.unk18.hex()}"
         + ("" if f.elem_sizes == (56, 56, 4, 1, 1) else " elem=" + ",".join(map(str, f.elem_sizes)))]
    pos = f.code_positions()
    marks = {}
    for kind, tab in (("proc", f.procs), ("label", f.labels)):
        for idx, (nm, wi) in enumerate(tab):
            marks.setdefault(pos[wi], []).append((kind, idx, nm))
    pnames = [p[0] for p in f.procs]; lnames = [l[0] for l in f.labels]

    def ref(names, i):
        return names[i] if i < len(names) and names.count(names[i]) == 1 and " " not in names[i] else f"#{i}"
    # string pool: canonical = each string at its offset, zero padded
    strs = {}
    o = 0
    while o < len(f.strings):
        e = f.strings.find(b"\0", o)
        e = len(f.strings) if e < 0 else e
        if e > o:
            strs[o] = f.strings[o:e]
        o = e + 1
    for k, (name, arg, extra) in enumerate(f.code):
        for kind, idx, nm in marks.get(k, []):
            L.append(f".{kind} #{idx} {nm}")
        if name == "PUSHI":
            s = f"PUSHI {struct.unpack('>i', struct.pack('>I', extra))[0]}"
        elif name == "PUSHF":
            s = f"PUSHF {_f32(extra)}"
        elif name == "PUSHIS":
            s = f"PUSHIS {struct.unpack('>h', struct.pack('>H', arg))[0]}"
            arg = 0
        elif name == "COMM":
            s = f"COMM {cmds.name(arg)}"; arg = 0
        elif name in LABEL_OPS:
            s = f"{name} {ref(lnames, arg)}"; arg = 0
        elif name in PROC_OPS:
            s = f"{name} {ref(pnames, arg)}"; arg = 0
        elif name == "PUSHSTR":
            s = f"PUSHSTR @{arg} {_q(f.string_at(arg))}"; arg = 0
        else:
            s = name
        if name in ("PUSHI", "PUSHF") and arg:
            s += f" [arg={arg}]"
        elif name not in ("PUSHI", "PUSHF") and arg:
            s += f" {arg}"
        L.append("    " + s)
    for kind, idx, nm in marks.get(len(f.code), []):
        L.append(f".{kind} #{idx} {nm}")
    rebuilt = bytearray(len(f.strings))
    for o, s in strs.items():
        rebuilt[o:o + len(s)] = s
    if bytes(rebuilt) == f.strings:
        L.append(f".strings size={len(f.strings)}")
        for o, s in strs.items():
            L.append(f".str @{o} {_q(s)}")
    else:
        L.append(f".strings_hex {f.strings.hex()}")
    if f.trailer:
        L.append(f".trailer {f.trailer.hex()}")
    if f.msg:
        try:
            m = Msg2.parse(f.msg)
            if m.build() != f.msg:
                raise ValueError
            L.append(".messages")
            L.append(m.to_text().rstrip("\n"))
        except Exception:
            L.append(f".messages_hex {f.msg.hex()}")
    return "\n".join(L) + "\n"


def assemble(text: str, cmds) -> Flow:
    f = Flow()
    lines = text.splitlines()
    instrs = []          # (name, rest)
    procs, labels = {}, {}
    strpool = None
    i = 0
    while i < len(lines):
        raw = lines[i]; i += 1
        line = raw.split(";", 1)[0].strip() if not raw.lstrip().startswith((".str ", ".speaker", "PUSHSTR")) else raw.strip()
        if not line:
            continue
        if line.startswith(".flow"):
            kv = dict(x.split("=") for x in line.split()[1:])
            f.file_type, f.compressed, f.user_id = int(kv["type"]), int(kv["compressed"]), int(kv["user"])
            f.unk0c, f.local_int, f.local_float = int(kv["unk0c"]), int(kv["locals_int"]), int(kv["locals_float"])
            f.unk18 = bytes.fromhex(kv["unk18"])
            if "elem" in kv:
                f.elem_sizes = tuple(int(x) for x in kv["elem"].split(","))
        elif line.startswith((".proc", ".label")):
            kind, idx, nm = line.split(None, 2)
            (procs if kind == ".proc" else labels)[int(idx[1:])] = [nm, len(instrs)]
        elif line.startswith(".strings_hex"):
            f.strings = bytes.fromhex(line.split()[1])
        elif line.startswith(".strings "):
            strpool = bytearray(int(line.split("=")[1]))
        elif line.startswith(".str "):
            _, at, q = line.split(None, 2)
            o = int(at[1:]); s = _unq(q)
            if o + len(s) > len(strpool):  # growing the pool for a new string: keep it terminated
                strpool.extend(b"\0" * (o + len(s) + 1 - len(strpool)))
            strpool[o:o + len(s)] = s
        elif line.startswith(".trailer"):
            f.trailer = bytes.fromhex(line.split()[1])
        elif line.startswith(".messages_hex"):
            f.msg = bytes.fromhex(line.split()[1])
        elif line.startswith(".messages"):
            j = i
            while not lines[j].strip().startswith(".endmsg2"):
                j += 1
            f.msg = Msg2.from_text(lines[i:j + 1]).build()
            i = j + 1
        else:
            name, _, rest = line.partition(" ")
            instrs.append((name, rest.strip()))
    if strpool is not None:
        f.strings = bytes(strpool)
    f.procs = [procs[k] for k in range(len(procs))]
    f.labels = [labels[k] for k in range(len(labels))]
    pidx = {nm: k for k, (nm, _) in enumerate(f.procs)}
    lidx = {nm: k for k, (nm, _) in enumerate(f.labels)}

    def target(names, s):
        return int(s[1:]) if s.startswith("#") else names[s]
    # instruction index -> word index
    wpos = []
    w = 0
    for name, rest in instrs:
        wpos.append(w)
        w += 2 if name in ("PUSHI", "PUSHF") else 1
    wpos.append(w)
    for tab in (f.procs, f.labels):
        for e in tab:
            e[1] = wpos[e[1]]
    for name, rest in instrs:
        extra = None; arg = 0
        if name in ("PUSHI", "PUSHF"):
            val, _, opt = rest.partition(" [arg=")
            if opt:
                arg = int(opt.rstrip("]"))
            if name == "PUSHI":
                extra = int(val) & 0xFFFFFFFF
            else:
                extra = int(val, 16) if val.startswith("0x") else struct.unpack(">I", struct.pack(">f", float(val)))[0]
        elif name == "PUSHIS":
            arg = int(rest) & 0xFFFF
        elif name == "COMM":
            arg = cmds.id(rest)
        elif name in LABEL_OPS:
            arg = target(lidx, rest)
        elif name in PROC_OPS:
            arg = target(pidx, rest)
        elif name == "PUSHSTR":
            at, _, q = rest.partition(" ")
            arg = int(at[1:])
        elif rest:
            arg = int(rest, 0)
        f.code.append([name, arg, extra])
    return f
