"""Which text goes with which tutorial picture: from main.dol's per-mode tables of tutorial pages and the
tutorial messages (tutorial/<mode>/eng/NNN.bm2).
    python catalog.py <main.dol> <disc DATA/files/tutorial> <extracted tutorial textures> <out.json>"""
import struct, sys, os, re, json
DOL, TUT, PICS, OUT = sys.argv[1:5]
d = open(DOL, "rb").read()
off = struct.unpack(">18I", d[0:72]); addr = struct.unpack(">18I", d[72:144]); sz = struct.unpack(">18I", d[144:216])


def a2f(a):
    for i in range(18):
        if sz[i] and addr[i] <= a < addr[i] + sz[i]: return off[i] + a - addr[i]


def cstr(a):
    f = a2f(a); return None if f is None else d[f:d.index(b"\0", f)]


pics = {}
for r, _, fs in os.walk(PICS):
    for f in fs:
        if re.match(r"tt_[0-9A-F]{16}_", f): pics.setdefault(os.path.basename(r)[:-4], []).append(f)


def msgs(m, i):
    p = os.path.join(TUT, m, "eng", "%03d.bm2" % i)
    if not os.path.exists(p): return []
    out = []
    for part in open(p, "rb").read().split(b"\x00\x00\x00\x01\x00\x00\x00\x08")[1:]:
        lines = []
        for s in part.split(b"\0"):
            if not s:
                if lines: break
                continue
            t = s.decode("latin1")
            if not t.isprintable(): break
            lines.append(t)
        if lines: out.append(lines)
    return out


def pages(a):   # 0x60-byte page structs: ffffffff, 0, 0, 1.0f, then up to 10 (picture name, frames)
    out = []; f = a2f(a)
    while struct.unpack(">I", d[f:f + 4])[0] == 0xFFFFFFFF:
        names = []
        for k in range(10):
            w = struct.unpack(">I", d[f + 16 + k * 8:f + 20 + k * 8])[0]
            if not w: break
            names.append(cstr(w).decode())
        if not names: break
        out.append(names); f += 0x60
    return out


ARR = {"su": (0x80388EE8, 21), "em": (0x8038A080, 27), "en": (0x8038B0F8, 21), "di": (0x8038C3FC, 16), "au": (0x8038DCD8, 22), "or": (0x8038E7C8, 21)}
data = []
for m, (a, n) in ARR.items():
    fo = a2f(a)
    for i in range(n):
        ms = msgs(m, i)
        if not ms or not ms[0][0].startswith("*"): continue
        st = pages(struct.unpack(">I", d[fo + i * 4:fo + i * 4 + 4])[0])
        if not st: continue
        shots = [[{"name": nm, "hash": f[3:19]} for nm in names for f in sorted(pics.get(nm, []))] for names in st]
        data.append({"mode": m, "key": "%s%02d" % (m, i), "msgs": [{"title": x[0].lstrip("*"), "lines": x[1:]} for x in ms], "shots": shots})
json.dump(data, open(OUT, "w"))
print(len(data), "tutorial topics")
