"""FLW0 flowscripts (.bf) - Atlus's Persona-family script bytecode, big-endian Wii variant.

  0x00  u8 file_type, u8 compressed, u16 user_id, u32 strings_offset, "FLW0", u32 unk0c,
        u32 section_count(5), u16 local_int_count, u16 local_float_count, 8 bytes unk18
  0x20  5 x {u32 type, u32 elem_size, u32 elem_count, u32 offset}   sections are contiguous:
          0 procedures  {char name[48], u32 code_index, u32 0}
          1 labels      {char name[48], u32 code_index, u32 0}
          2 code        u32 words: (u16 opcode << 16) | u16 operand; PUSHI/PUSHF take a 2nd word
          3 messages    an embedded MSG2 blob (see msg2.py)
          4 strings     NUL-terminated strings referenced by PUSHSTR <byte offset>
        anything after the string pool is kept as a trailer
"""
import struct
from .msg2 import Msg2

OPS = ["PUSHI", "PUSHF", "PUSHIX", "PUSHIF", "PUSHREG", "POPIX", "POPFX", "PROC", "COMM", "END",
       "JUMP", "CALL", "RUN", "GOTO", "ADD", "SUB", "MUL", "DIV", "MINUS", "NOT", "OR", "AND",
       "EQ", "NEQ", "S", "L", "SE", "LE", "IF", "PUSHIS", "PUSHLIX", "PUSHLFX", "POPLIX", "POPLFX",
       "PUSHSTR"]
OPNUM = {n: i for i, n in enumerate(OPS)}
LABEL_OPS = {"GOTO", "IF"}          # operand = label index
PROC_OPS = {"JUMP", "CALL", "RUN"}  # operand = procedure index


class Flow:
    def __init__(self):
        self.file_type = 0; self.compressed = 0; self.user_id = 0; self.unk0c = 0
        self.local_int = 0; self.local_float = 0; self.unk18 = b"\0" * 8
        self.elem_sizes = (56, 56, 4, 1, 1)
        self.procs = []   # [name, code_index]
        self.labels = []  # [name, code_index]
        self.code = []    # list of instructions: [opname, operand, extra_word_or_None]
        self.msg = b""    # raw MSG2 blob (may be empty)
        self.strings = b""
        self.trailer = b""

    # ---------------- binary ----------------
    @classmethod
    def parse(cls, b):
        if b[8:12] != b"FLW0":
            raise ValueError("not a FLW0 file")
        f = cls()
        f.file_type, f.compressed, f.user_id, stroff = struct.unpack(">BBHI", b[:8])
        f.unk0c, n, f.local_int, f.local_float = struct.unpack(">IIHH", b[12:24])
        f.unk18 = b[24:32]
        secs = {}
        for i in range(n):
            t, es, ec, o = struct.unpack(">4I", b[0x20 + 16 * i:0x30 + 16 * i])
            secs[t] = (es, ec, o)
        f.elem_sizes = tuple(secs[t][0] for t in range(5))

        def table(t):
            es, ec, o = secs[t]
            out = []
            for k in range(ec):
                e = b[o + k * es:o + (k + 1) * es]
                assert struct.unpack(">I", e[-4:])[0] == 0
                out.append([e[:es - 8].split(b"\0")[0].decode("latin1"), struct.unpack(">I", e[es - 8:es - 4])[0]])
            return out
        f.procs, f.labels = table(0), table(1)
        es, ec, o = secs[2]
        w = struct.unpack(f">{ec}I", b[o:o + 4 * ec])
        f._word_index = {}
        i = 0
        while i < len(w):
            op, arg = w[i] >> 16, w[i] & 0xFFFF
            f._word_index[i] = len(f.code)
            name = OPS[op] if op < len(OPS) else f"OP{op}"
            if name in ("PUSHI", "PUSHF"):
                f.code.append([name, arg, w[i + 1]]); i += 2
            else:
                f.code.append([name, arg, None]); i += 1
        es, ec, o = secs[3]; f.msg = b[o:o + ec]
        es, ec, o = secs[4]; f.strings = b[o:o + ec]
        f.trailer = b[o + ec:]
        assert stroff == o
        return f

    def words(self):
        out = []
        for name, arg, extra in self.code:
            op = OPNUM[name] if name in OPNUM else int(name[2:])
            out.append((op << 16) | (arg & 0xFFFF))
            if extra is not None:
                out.append(extra)
        return out

    def build(self):
        w = self.words()
        es = self.elem_sizes

        def table(items, size):
            r = b""
            for nm, idx in items:
                nb = nm.encode("latin1")
                r += nb + b"\0" * (size - 8 - len(nb)) + struct.pack(">II", idx, 0)
            return r
        parts = [table(self.procs, es[0]), table(self.labels, es[1]),
                 struct.pack(f">{len(w)}I", *w), self.msg, self.strings]
        counts = [len(self.procs), len(self.labels), len(w), len(self.msg), len(self.strings)]
        off = 0x20 + 16 * 5
        sec = b""
        offs = []
        for t in range(5):
            sec += struct.pack(">4I", t, es[t], counts[t], off)
            offs.append(off)
            off += len(parts[t])
        hdr = struct.pack(">BBHI", self.file_type, self.compressed, self.user_id, offs[4]) + b"FLW0"
        hdr += struct.pack(">IIHH", self.unk0c, 5, self.local_int, self.local_float) + self.unk18
        return hdr + sec + b"".join(parts) + self.trailer

    # ---------------- helpers ----------------
    def code_positions(self):
        """word index -> instruction index, for label/proc targets."""
        pos = {}
        wi = 0
        for k, (name, arg, extra) in enumerate(self.code):
            pos[wi] = k
            wi += 2 if extra is not None else 1
        pos[wi] = len(self.code)
        return pos

    def string_at(self, off):
        return self.strings[off:].split(b"\0")[0]

    def message_names(self):
        if len(self.msg) < 0x30:
            return []
        try:
            return [m.name for m in Msg2.parse(self.msg).messages]
        except Exception:
            return []
