"""MSG2 message files (.bm2, and the message section embedded in .bf scripts).

Layout (big-endian), worked out against the leaked test_mes.msg / explanation_eng.msg sources:

  0x00  u32 0, "MSG2", u16 version(1), u16 0, u32 file_size, u32 0x20, 12 bytes 0
  0x20  u32 speaker_count, u32 speaker_table_off, u32 message_count, u32 message_table_off
        speaker table: speaker_count x {u32 a, u32 name_len, u32 name_off, u32 b}
        speaker names (Shift-JIS, NUL terminated)
        message table: message_count x 0x40 {char name[32], u32 type, u32 size, u32 off, u16 speaker, 14 pad}
        message bodies, in table order:
            u32 page_count, page_count x u32 page_off (relative to body start), page bytes
  Page bytes: Shift-JIS text, 0x00 = line break, control tags = FF, u16 id, u16 argc,
              argc x {u16 type, u32 value}   (type 1 = integer)
"""
import struct

TAG_NAMES = {}  # id -> name, filled from data/msgtags.csv by load_tag_names()


def load_tag_names(path):
    TAG_NAMES.clear()
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("id,"):
            continue
        i, name = line.split(",")[:2]
        TAG_NAMES[int(i, 0)] = name


class Message:
    def __init__(self, name, type_, speaker, pages, raw_pad=b"", raw_body=None):
        self.name, self.type, self.speaker, self.pages, self.raw_pad = name, type_, speaker, pages, raw_pad
        self.raw_body = raw_body  # set when the body is not a well-formed page list; kept byte-exact


class Msg2:
    def __init__(self):
        self.version = 1
        self.hdr_extra = b"\0" * 14  # bytes 0x0a-0x0b and 0x14-0x1f kept verbatim
        self.speakers = []  # list of (a, name_bytes, b)
        self.messages = []
        self.trailer = b""

    # ---------------- binary ----------------
    @classmethod
    def parse(cls, b):
        if b[4:8] != b"MSG2":
            raise ValueError("not a MSG2 file")
        m = cls()
        m.version = struct.unpack(">H", b[8:10])[0]
        m.hdr_extra = b[10:12] + b[0x14:0x20]
        size = struct.unpack(">I", b[0xC:0x10])[0]
        sc, so, mc, mo = struct.unpack(">4I", b[0x20:0x30])
        for k in range(sc):
            a, ln, off, bb = struct.unpack(">4I", b[so + 16 * k:so + 16 * k + 16])
            m.speakers.append((a, b[off:off + ln], bb))
        for k in range(mc):
            e = b[mo + 0x40 * k:mo + 0x40 * (k + 1)]
            name = e[:32].split(b"\0")[0].decode("latin1")
            t, sz, off = struct.unpack(">3I", e[32:44])
            spk = struct.unpack(">h", e[44:46])[0]
            pad = e[46:]
            body = b[off:off + sz]
            msg = Message(name, t, spk, [], pad)
            try:
                n = struct.unpack(">I", body[:4])[0]
                offs = list(struct.unpack(f">{n}I", body[4:4 + 4 * n]))
                pages = [body[offs[i]:(offs[i + 1] if i + 1 < n else sz)] for i in range(n)]
                if offs != cls._page_offsets(pages) or any(o > sz for o in offs):
                    raise ValueError
                msg.pages = pages
            except (struct.error, ValueError):
                msg.raw_body = body
            m.messages.append(msg)
        m.trailer = b[size:]
        return m

    @staticmethod
    def _page_offsets(pages):
        o = 4 + 4 * len(pages)
        res = []
        for p in pages:
            res.append(o)
            o += len(p)
        return res

    def build(self):
        sc, mc = len(self.speakers), len(self.messages)
        so = 0x30
        names_off = so + 16 * sc
        spk_tab = b""
        names = b""
        for a, nm, bb in self.speakers:
            spk_tab += struct.pack(">4I", a, len(nm), names_off + len(names), bb)
            names += nm
        mo = names_off + len(names)
        body_off = mo + 0x40 * mc
        table = b""
        bodies = b""
        for msg in self.messages:
            if msg.raw_body is not None:
                body = msg.raw_body
            else:
                offs = self._page_offsets(msg.pages)
                body = struct.pack(f">I{len(offs)}I", len(offs), *offs) + b"".join(msg.pages)
            nm = msg.name.encode("latin1")
            table += nm + b"\0" * (32 - len(nm)) + struct.pack(">3Ih", msg.type, len(body),
                                                                 body_off + len(bodies), msg.speaker) + msg.raw_pad
            bodies += body
        size = body_off + len(bodies)
        hdr = (b"\0\0\0\0MSG2" + struct.pack(">H", self.version) + self.hdr_extra[:2] +
               struct.pack(">II", size, 0x20) + self.hdr_extra[2:14])
        hdr += struct.pack(">4I", sc, so, mc, mo)
        return hdr + spk_tab + names + table + bodies + self.trailer

    # ---------------- text ----------------
    @staticmethod
    def page_to_text(p):
        out = []
        i = 0
        while i < len(p):
            c = p[i]
            if c == 0xFF and i + 5 <= len(p):
                tid, argc = struct.unpack(">HH", p[i + 1:i + 5])
                if i + 5 + 6 * argc <= len(p):
                    args = []
                    for k in range(argc):
                        t, v = struct.unpack(">Hi", p[i + 5 + 6 * k:i + 11 + 6 * k])
                        args.append(str(v) if t == 1 else f"{t}:{v}")
                    nm = TAG_NAMES.get(tid, f"tag_{tid:02X}")
                    out.append("<" + " ".join([nm] + args) + ">")
                    i += 5 + 6 * argc
                    continue
            if c == 0:
                out.append("\\n"); i += 1; continue
            if 0x20 <= c < 0x7F:
                ch = chr(c)
                out.append({"\\": "\\\\", '"': '\\"', "<": "\\<"}.get(ch, ch)); i += 1; continue
            if (0x81 <= c <= 0x9F or 0xE0 <= c <= 0xFC) and i + 1 < len(p):
                try:
                    out.append(p[i:i + 2].decode("cp932")); i += 2; continue
                except UnicodeDecodeError:
                    pass
            out.append(f"\\x{c:02X}"); i += 1
        return '"' + "".join(out) + '"'

    @staticmethod
    def text_to_page(s):
        assert s[0] == '"' and s[-1] == '"', s
        s = s[1:-1]
        rev = {v: k for k, v in TAG_NAMES.items()}
        out = bytearray()
        i = 0
        while i < len(s):
            ch = s[i]
            if ch == "\\":
                nx = s[i + 1]
                if nx == "n": out.append(0); i += 2
                elif nx == "x": out.append(int(s[i + 2:i + 4], 16)); i += 4
                else: out += nx.encode("cp932"); i += 2
            elif ch == "<":
                j = s.index(">", i)
                parts = s[i + 1:j].split()
                nm = parts[0]
                tid = rev[nm] if nm in rev else int(nm[4:], 16)
                out += b"\xFF" + struct.pack(">HH", tid, len(parts) - 1)
                for a in parts[1:]:
                    t, v = (a.split(":") if ":" in a else ("1", a))
                    out += struct.pack(">Hi", int(t), int(v))
                i = j + 1
            else:
                out += ch.encode("cp932"); i += 1
        return bytes(out)

    def to_text(self):
        L = [f".msg2 version={self.version} extra={self.hdr_extra.hex()}"]
        for a, nm, bb in self.speakers:
            L.append(f".speaker a={a} b={bb} " + self.page_to_text(nm))
        for m in self.messages:
            pad = f" pad={m.raw_pad.hex()}" if any(m.raw_pad) else ""
            raw = f" raw={m.raw_body.hex()}" if m.raw_body is not None else ""
            kind = {0: "message", 1: "select"}.get(m.type, f"type{m.type}")
            L.append(f'.{kind} "{m.name}" speaker={m.speaker}{pad}{raw}')
            for p in m.pages:
                L.append("  " + self.page_to_text(p))
        if self.trailer:
            L.append(f".trailer {self.trailer.hex()}")
        L.append(".endmsg2")
        return "\n".join(L) + "\n"

    @classmethod
    def from_text(cls, lines):
        m = cls()
        cur = None
        for raw in lines:
            line = raw.strip()
            if not line or line.startswith(";"):
                continue
            if line.startswith(".msg2"):
                kv = dict(x.split("=") for x in line.split()[1:])
                m.version = int(kv["version"]); m.hdr_extra = bytes.fromhex(kv["extra"])
            elif line.startswith(".speaker"):
                head, _, txt = line.partition(' "')
                kv = dict(x.split("=") for x in head.split()[1:])
                m.speakers.append((int(kv["a"]), cls.text_to_page('"' + txt), int(kv["b"])))
            elif line.startswith((".message", ".select", ".type")):
                kind, _, rest = line.partition(' "')
                kind = kind[1:]
                name, _, rest = rest.partition('"')
                t = {"message": 0, "select": 1}.get(kind) if kind in ("message", "select") else int(kind[4:])
                kv = dict(x.split("=") for x in rest.split())
                cur = Message(name, t, int(kv["speaker"]), [],
                              bytes.fromhex(kv["pad"]) if "pad" in kv else b"\0" * 18,
                              bytes.fromhex(kv["raw"]) if "raw" in kv else None)
                m.messages.append(cur)
            elif line.startswith(".trailer"):
                m.trailer = bytes.fromhex(line.split()[1])
            elif line.startswith(".endmsg2"):
                break
            else:
                cur.pages.append(cls.text_to_page(line))
        return m
