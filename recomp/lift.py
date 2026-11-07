"""Static recompiler: main.dol (PowerPC/Gekko) -> C.

    python -m recomp.lift <main.dol> <dtk symbols.txt> <out_dir>

Every guest function becomes `void f_XXXXXXXX(CPU *c)`. Branches inside a function become gotos;
calls become C calls; `blr` becomes `return`. Indirect calls (bctrl, function pointers, vtables)
go through rt_call(c, addr), a binary search over every known entry point. Branches that land in the
middle of another function (CodeWarrior's __save_gpr/__restore_gpr, shared epilogues) get their own
entry function that starts at that address.
"""
import os
import re
import struct
import sys
from collections import defaultdict

from .ppc import decode


class Dol:
    def __init__(self, data):
        self.d = data
        off = struct.unpack(">18I", data[:72]); ad = struct.unpack(">18I", data[0x48:0x90])
        sz = struct.unpack(">18I", data[0x90:0xD8])
        self.secs = [(i, ad[i], off[i], sz[i]) for i in range(18) if sz[i]]
        self.entry = struct.unpack(">I", data[0xE0:0xE4])[0]
        self.bss = struct.unpack(">II", data[0xD8:0xE0])

    def word(self, a):
        for i, A, O, S in self.secs:
            if A <= a < A + S:
                return struct.unpack(">I", self.d[O + a - A:O + a - A + 4])[0]
        return None

    def is_text(self, a):
        return any(A <= a < A + S for i, A, O, S in self.secs if i < 7)


def load_symbols(path):
    funcs, jumptables = {}, []
    for l in open(path):
        m = re.match(r"(\S+) = \.(text|init):0x([0-9A-F]+); // type:function size:0x([0-9A-F]+)", l)
        if m:
            funcs[int(m[3], 16)] = (int(m[4], 16), m[1])
        m = re.match(r"(jumptable_\S+) = \.\w+:0x([0-9A-F]+); // type:object size:0x([0-9A-F]+)", l)
        if m:
            jumptables.append((int(m[2], 16), int(m[3], 16)))
    return funcs, jumptables


# ---------------------------------------------------------------- code generation helpers
def R(n): return f"c->r[{n}]"
def RA0(n): return "0" if n == 0 else f"c->r[{n}]"
def F0(n): return f"c->f[{n}].ps0"
def F1(n): return f"c->f[{n}].ps1"


# Interrupt delivery point, emitted on every backward branch (loops): cheap flag test.
POLL = "if (g_irq_pending) rt_poll(c); "


def cr0(expr):
    return f"SET_CR0((s32)({expr}));"


def mask(mb, me):
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m


def bc_cond(BO, BI):
    """C expression for a conditional branch; also decrements CTR when BO says so."""
    parts = []
    pre = ""
    if not (BO & 4):
        pre = "c->ctr--; "
        parts.append("(c->ctr != 0)" if not (BO & 2) else "(c->ctr == 0)")
    if not (BO & 16):
        bit = f"CR_BIT({BI})"
        parts.append(bit if BO & 8 else f"!{bit}")
    return pre, (" && ".join(parts) if parts else "1")


# Functions whose original body must stay callable from a native replacement: the body is emitted
# as orig_XXXXXXXX and recomp/runtime provides f_XXXXXXXX.
WRAP = {
    0x802222E0,  # SelectThread   (thread scheduler: fiber switching)
    0x8021B5D0,  # OSSaveContext  (records which fiber owns a context)
}


class Lifter:
    def __init__(self, dol, funcs, jumptables):
        self.dol = dol
        self.funcs = dict(funcs)
        self.starts = sorted(self.funcs)
        self.jt_targets = defaultdict(set)   # func start -> jump-table targets inside it
        for a, size in jumptables:
            for k in range(0, size, 4):
                t = dol.word(a + k)
                if t and dol.is_text(t):
                    self.jt_targets[self.owner(t)].add(t)
        # dtk misses some switch tables: also accept any word in a data section that points inside a
        # function (not at its start) as a possible computed-jump target of that function.
        for i, A, O, S in dol.secs:
            if i < 7:
                continue
            for k in range(0, S - 3, 4):
                t = struct.unpack(">I", dol.d[O + k:O + k + 4])[0]
                if dol.is_text(t) and t & 3 == 0:
                    o = self.owner(t)
                    if o is not None and o != t:
                        self.jt_targets[o].add(t)
        self.entries = {}  # entry addr -> (containing func start, end)
        for s in self.starts:
            self.entries[s] = (s, s + self.funcs[s][0])
        self._find_extra_entries()

    def owner(self, a):
        import bisect
        i = bisect.bisect_right(self.starts, a) - 1
        if i >= 0:
            s = self.starts[i]
            if s <= a < s + self.funcs[s][0]:
                return s
        return None

    def _find_extra_entries(self):
        for s in list(self.starts):
            size = self.funcs[s][0]
            for a in range(s, s + size, 4):
                w = self.dol.word(a)
                ins = decode(w, a)
                if ins.name in ("b", "bc"):
                    t = ins.target
                    inside = s <= t < s + size
                    if (ins.name == "b" and ins.LK) or not inside:
                        if t not in self.entries:
                            o = self.owner(t)
                            if o is not None:
                                self.entries[t] = (o, o + self.funcs[o][0])

    # ------------------------------------------------------------ one function
    def lift(self, entry):
        start, end = self.entries[entry]
        insns = [decode(self.dol.word(a), a) for a in range(entry, end, 4)]
        labels = set(self.jt_targets.get(start, set()))
        for ins in insns:
            if ins.name in ("b", "bc") and entry <= ins.target < end and not (ins.name == "b" and ins.LK):
                labels.add(ins.target)
        jt = sorted(t for t in labels if t in self.jt_targets.get(start, set()) and t >= entry)
        head = f"void orig_{entry:08X}(CPU *c)" if entry in WRAP else f"WEAK void f_{entry:08X}(CPU *c)"
        out = [head + " {", f"    u32 ea; (void)ea; TRACE(0x{entry:08X}u);"]
        for ins in insns:
            if ins.addr in labels:
                out.append(f"L_{ins.addr:08X}:")
            out.append("    " + self.emit(ins, entry, end, jt))
        out.append("}")
        return "\n".join(out)

    def call(self, t, nxt):
        if t in self.entries:
            return f"c->lr = 0x{nxt:08X}u; f_{t:08X}(c);"
        return f"c->lr = 0x{nxt:08X}u; rt_call(c, 0x{t:08X}u);"

    def tail(self, t):
        if t in self.entries:
            return f"f_{t:08X}(c); return;"
        return f"rt_call(c, 0x{t:08X}u); return;"

    def emit(self, i, entry, end, jt):
        n = i.name; a = i.addr; nxt = a + 4
        rD, rA, rB, rS = i.f["rD"], i.f["rA"], i.f["rB"], i.f["rS"]
        Rc = i.f["Rc"]
        rc = (lambda e: " " + cr0(e)) if Rc else (lambda e: "")
        inside = lambda t: entry <= t < end
        # ---------------- branches
        if n == "b":
            if i.LK:
                return self.call(i.target, nxt)
            if inside(i.target):
                return f"{POLL}goto L_{i.target:08X};" if i.target <= a else f"goto L_{i.target:08X};"
            return self.tail(i.target)
        if n == "bc":
            pre, cond = bc_cond(i.BO, i.BI)
            if i.LK:
                return f"{pre}if ({cond}) {{ {self.call(i.target, nxt)} }}"
            if inside(i.target):
                body = (POLL if i.target <= a else "") + f"goto L_{i.target:08X};"
            else:
                body = self.tail(i.target)
            return f"{pre}if ({cond}) {{ {body} }}"
        if n == "bclr":
            pre, cond = bc_cond(i.BO, i.BI)
            if i.LK:
                return f"{pre}if ({cond}) {{ u32 t = c->lr; c->lr = 0x{nxt:08X}u; rt_call(c, t); }}"
            return f"{pre}if ({cond}) return;" if cond != "1" else f"{pre}return;"
        if n == "bcctr":
            pre, cond = bc_cond(i.BO | 4, i.BI)
            if i.LK:
                return f"if ({cond}) {{ c->lr = 0x{nxt:08X}u; rt_call(c, c->ctr); }}"
            if jt:
                cases = " ".join(f"case 0x{t:08X}u: goto L_{t:08X};" for t in jt)
                return f"if ({cond}) {{ switch (c->ctr) {{ {cases} default: rt_call(c, c->ctr); return; }} }}"
            return f"if ({cond}) {{ rt_call(c, c->ctr); return; }}"
        if n == "sc":
            return f"rt_syscall(c, 0x{a:08X}u);"
        if n == "rfi":
            return "rt_rfi(c); return;"
        if n in ("tw", "twi"):
            return f"rt_trap(c, 0x{a:08X}u);"
        # ---------------- integer arithmetic
        S, U = i.f.get("SIMM", 0), i.f.get("UIMM", 0)
        if n == "addi": return f"{R(rD)} = {RA0(rA)} + {S & 0xFFFFFFFF}u;"
        if n == "addis": return f"{R(rD)} = {RA0(rA)} + 0x{(S << 16) & 0xFFFFFFFF:X}u;"
        if n == "addic" or n == "addic.":
            s = f"{{ u64 t = (u64){R(rA)} + {S & 0xFFFFFFFF}u; {R(rD)} = (u32)t; c->ca = (u8)(t >> 32); }}"
            return s + (" " + cr0(R(rD)) if n == "addic." else "")
        if n == "subfic":
            return f"{{ u64 t = (u64)(u32)~{R(rA)} + {S & 0xFFFFFFFF}u + 1; {R(rD)} = (u32)t; c->ca = (u8)(t >> 32); }}"
        if n == "mulli": return f"{R(rD)} = (u32)((s32){R(rA)} * {S});"
        if n == "add": return f"{R(rD)} = {R(rA)} + {R(rB)};" + rc(R(rD))
        if n == "subf": return f"{R(rD)} = {R(rB)} - {R(rA)};" + rc(R(rD))
        if n == "neg": return f"{R(rD)} = (u32)-(s32){R(rA)};" + rc(R(rD))
        if n == "addc": return f"{{ u64 t = (u64){R(rA)} + {R(rB)}; {R(rD)} = (u32)t; c->ca = (u8)(t >> 32); }}" + rc(R(rD))
        if n == "subfc": return f"{{ u64 t = (u64)(u32)~{R(rA)} + {R(rB)} + 1; {R(rD)} = (u32)t; c->ca = (u8)(t >> 32); }}" + rc(R(rD))
        if n == "adde": return f"{{ u64 t = (u64){R(rA)} + {R(rB)} + c->ca; {R(rD)} = (u32)t; c->ca = (u8)(t >> 32); }}" + rc(R(rD))
        if n == "subfe": return f"{{ u64 t = (u64)(u32)~{R(rA)} + {R(rB)} + c->ca; {R(rD)} = (u32)t; c->ca = (u8)(t >> 32); }}" + rc(R(rD))
        if n == "addze": return f"{{ u64 t = (u64){R(rA)} + c->ca; {R(rD)} = (u32)t; c->ca = (u8)(t >> 32); }}" + rc(R(rD))
        if n == "subfze": return f"{{ u64 t = (u64)(u32)~{R(rA)} + c->ca; {R(rD)} = (u32)t; c->ca = (u8)(t >> 32); }}" + rc(R(rD))
        if n == "addme": return f"{{ u64 t = (u64){R(rA)} + c->ca + 0xFFFFFFFFull; {R(rD)} = (u32)t; c->ca = (u8)(t >> 32); }}" + rc(R(rD))
        if n == "subfme": return f"{{ u64 t = (u64)(u32)~{R(rA)} + c->ca + 0xFFFFFFFFull; {R(rD)} = (u32)t; c->ca = (u8)(t >> 32); }}" + rc(R(rD))
        if n == "mullw": return f"{R(rD)} = (u32)((s32){R(rA)} * (s32){R(rB)});" + rc(R(rD))
        if n == "mulhw": return f"{R(rD)} = (u32)(((s64)(s32){R(rA)} * (s32){R(rB)}) >> 32);" + rc(R(rD))
        if n == "mulhwu": return f"{R(rD)} = (u32)(((u64){R(rA)} * {R(rB)}) >> 32);" + rc(R(rD))
        if n == "divw": return f"{R(rD)} = rt_divw({R(rA)}, {R(rB)});" + rc(R(rD))
        if n == "divwu": return f"{R(rD)} = {R(rB)} ? {R(rA)} / {R(rB)} : 0;" + rc(R(rD))
        # ---------------- compares
        if n in ("cmpi", "cmp", "cmpli", "cmpl"):
            f_ = i.f["crfD"]
            if n == "cmpi": return f"SET_CRF_S({f_}, (s32){R(rA)}, {S});"
            if n == "cmp": return f"SET_CRF_S({f_}, (s32){R(rA)}, (s32){R(rB)});"
            if n == "cmpli": return f"SET_CRF_U({f_}, {R(rA)}, {U}u);"
            return f"SET_CRF_U({f_}, {R(rA)}, {R(rB)});"
        # ---------------- logic / shifts / rotates  (rA = dest, rS = source)
        if n == "andi.": return f"{R(rA)} = {R(rS)} & 0x{U:X}u; " + cr0(R(rA))
        if n == "andis.": return f"{R(rA)} = {R(rS)} & 0x{U << 16:X}u; " + cr0(R(rA))
        if n == "ori": return f"{R(rA)} = {R(rS)} | 0x{U:X}u;"
        if n == "oris": return f"{R(rA)} = {R(rS)} | 0x{U << 16:X}u;"
        if n == "xori": return f"{R(rA)} = {R(rS)} ^ 0x{U:X}u;"
        if n == "xoris": return f"{R(rA)} = {R(rS)} ^ 0x{U << 16:X}u;"
        binops = {"and": "{s} & {b}", "andc": "{s} & ~{b}", "or": "{s} | {b}", "orc": "{s} | ~{b}",
                  "xor": "{s} ^ {b}", "nor": "~({s} | {b})", "nand": "~({s} & {b})", "eqv": "~({s} ^ {b})"}
        if n in binops:
            return f"{R(rA)} = {binops[n].format(s=R(rS), b=R(rB))};" + rc(R(rA))
        if n == "extsb": return f"{R(rA)} = (u32)(s32)(s8){R(rS)};" + rc(R(rA))
        if n == "extsh": return f"{R(rA)} = (u32)(s32)(s16){R(rS)};" + rc(R(rA))
        if n == "cntlzw": return f"{R(rA)} = {R(rS)} ? (u32)__builtin_clz({R(rS)}) : 32;" + rc(R(rA))
        if n == "slw": return f"{R(rA)} = ({R(rB)} & 0x20) ? 0 : {R(rS)} << ({R(rB)} & 0x1F);" + rc(R(rA))
        if n == "srw": return f"{R(rA)} = ({R(rB)} & 0x20) ? 0 : {R(rS)} >> ({R(rB)} & 0x1F);" + rc(R(rA))
        if n == "sraw": return f"{R(rA)} = rt_sraw(c, {R(rS)}, {R(rB)});" + rc(R(rA))
        if n == "srawi":
            sh = i.f["SH"]
            return (f"{{ s32 v = (s32){R(rS)}; c->ca = (v < 0) && (v & 0x{(1 << sh) - 1:X}); "
                    f"{R(rA)} = (u32)(v >> {sh}); }}" + rc(R(rA)))
        if n in ("rlwinm", "rlwimi", "rlwnm"):
            m = mask(i.MB, i.ME)
            sh = f"({R(rB)} & 31)" if n == "rlwnm" else str(i.SH)
            rot = f"ROTL({R(rS)}, {sh})"
            if n == "rlwimi":
                return f"{R(rA)} = ({rot} & 0x{m:08X}u) | ({R(rA)} & 0x{~m & 0xFFFFFFFF:08X}u);" + rc(R(rA))
            return f"{R(rA)} = {rot} & 0x{m:08X}u;" + rc(R(rA))
        # ---------------- loads / stores
        d = i.f.get("d", 0)
        ea_d = f"ea = {RA0(rA)} + {d & 0xFFFFFFFF}u;"
        ea_x = f"ea = {RA0(rA)} + {R(rB)};"
        upd = f" {R(rA)} = ea;"
        L = {"lwz": "R32(ea)", "lbz": "R8(ea)", "lhz": "R16(ea)", "lha": "(u32)(s32)(s16)R16(ea)"}
        Sx = {"stw": "W32(ea, {v});", "stb": "W8(ea, (u8){v});", "sth": "W16(ea, (u16){v});"}
        for base, expr in L.items():
            if n == base: return f"{ea_d} {R(rD)} = {expr};"
            if n == base + "u": return f"{ea_d} {R(rD)} = {expr};{upd}"
            if n == base + "x": return f"{ea_x} {R(rD)} = {expr};"
            if n == base + "ux": return f"{ea_x} {R(rD)} = {expr};{upd}"
        for base, expr in Sx.items():
            e = expr.format(v=R(rS))
            if n == base: return f"{ea_d} {e}"
            if n == base + "u": return f"{ea_d} {e}{upd}"
            if n == base + "x": return f"{ea_x} {e}"
            if n == base + "ux": return f"{ea_x} {e}{upd}"
        if n == "lwbrx": return f"{ea_x} {R(rD)} = BSWAP32(R32(ea));"
        if n == "lhbrx": return f"{ea_x} {R(rD)} = BSWAP16(R16(ea));"
        if n == "stwbrx": return f"{ea_x} W32(ea, BSWAP32({R(rS)}));"
        if n == "sthbrx": return f"{ea_x} W16(ea, BSWAP16((u16){R(rS)}));"
        if n == "lwarx": return f"{ea_x} {R(rD)} = R32(ea);"
        if n == "stwcx.": return f"{ea_x} W32(ea, {R(rS)}); c->cr = (c->cr & 0x0FFFFFFFu) | 0x20000000u | (c->so ? 0x10000000u : 0);"
        if n == "lmw": return f"{ea_d} for (int k = {rD}; k < 32; k++, ea += 4) c->r[k] = R32(ea);"
        if n == "stmw": return f"{ea_d} for (int k = {rS}; k < 32; k++, ea += 4) W32(ea, c->r[k]);"
        if n in ("lswi", "stswi"):
            return f"rt_unimpl(c, 0x{a:08X}u, \"{n}\");"
        # floating point loads/stores
        if n in ("lfs", "lfsu", "lfsx", "lfsux"):
            ea = ea_x if n.endswith("x") else ea_d
            return f"{ea} {{ double v = (double)RF32(ea); {F0(rD)} = v; {F1(rD)} = v; }}" + (upd if "u" in n[3:] else "")
        if n in ("lfd", "lfdu", "lfdx", "lfdux"):
            ea = ea_x if n.endswith("x") else ea_d
            return f"{ea} {F0(rD)} = RF64(ea);" + (upd if "u" in n[3:] else "")
        if n in ("stfs", "stfsu", "stfsx", "stfsux"):
            ea = ea_x if n.endswith("x") else ea_d
            return f"{ea} WF32(ea, (float){F0(rS)});" + (upd if "u" in n[4:] else "")
        if n in ("stfd", "stfdu", "stfdx", "stfdux"):
            ea = ea_x if n.endswith("x") else ea_d
            return f"{ea} WF64(ea, {F0(rS)});" + (upd if "u" in n[4:] else "")
        if n == "stfiwx": return f"{ea_x} W32(ea, (u32)FBITS({F0(rS)}));"
        # paired-single quantized loads/stores
        if n in ("psq_l", "psq_lu", "psq_lx", "psq_lux"):
            ea = ea_x if n.endswith("x") else ea_d
            return f"{ea} rt_psq_l(c, ea, {rD}, {i.W}, {i.I});" + (upd if "u" in n[5:] else "")
        if n in ("psq_st", "psq_stu", "psq_stx", "psq_stux"):
            ea = ea_x if n.endswith("x") else ea_d
            return f"{ea} rt_psq_st(c, ea, {rS}, {i.W}, {i.I});" + (upd if "u" in n[6:] else "")
        # ---------------- special registers / cache / sync
        if n == "mfspr": return f"{R(rD)} = rt_mfspr(c, {i.spr});"
        if n == "mtspr": return f"rt_mtspr(c, {i.spr}, {R(rS)});"
        if n == "mftb": return f"{R(rD)} = rt_mftb(c, {i.spr});"
        if n == "mfmsr": return f"{R(rD)} = c->msr;"
        if n == "mtmsr": return f"rt_mtmsr(c, {R(rS)});"
        if n in ("mfsr", "mtsr"): return "/* segment register ignored */;"
        if n == "mfcr": return f"{R(rD)} = c->cr;"
        if n == "mtcrf":
            m = 0
            for k in range(8):
                if i.CRM & (0x80 >> k):
                    m |= 0xF << (28 - 4 * k)
            return f"c->cr = (c->cr & 0x{~m & 0xFFFFFFFF:08X}u) | ({R(rS)} & 0x{m:08X}u);"
        if n == "mcrf":
            return f"SET_CRF({i.crfD}, GET_CRF({i.crfS}));"
        if n == "mcrxr": return f"SET_CRF({i.crfD}, (c->so << 3) | (c->ov << 2) | (c->ca << 1)); c->so = c->ov = c->ca = 0;"
        crops = {"crxor": "{a} ^ {b}", "cror": "{a} | {b}", "crand": "{a} & {b}", "crnor": "!({a} | {b})",
                 "creqv": "!({a} ^ {b})", "crandc": "{a} & !{b}", "crorc": "{a} | !{b}", "crnand": "!({a} & {b})"}
        if n in crops:
            e = crops[n].format(a=f"CR_BIT({i.crbA})", b=f"CR_BIT({i.crbB})")
            return f"SET_CR_BIT({i.crbD}, {e});"
        if n in ("sync", "isync", "eieio", "dcbst", "dcbf", "dcbi", "dcbt", "dcbtst", "icbi", "tlbie"):
            return ";"
        if n in ("dcbz", "dcbz_l"):
            return f"{ea_x} rt_dcbz(c, ea);"
        if n == "mffs": return f"FSETBITS({F0(rD)}, (u64)c->fpscr);"
        if n == "mtfsf": return f"c->fpscr = (u32)FBITS({F0(rB)});"
        if n in ("mtfsb0", "mtfsb1", "mtfsfi"): return "/* fpscr bit op ignored */;"
        if n == "mcrfs": return f"SET_CRF({i.crfD}, 0);"
        # ---------------- floating point (double / single)
        fA, fB, fC, fD = F0(rA), F0(rB), F0(i.f["rC"]), F0(rD)
        dbl = {"fadd": f"{fA} + {fB}", "fsub": f"{fA} - {fB}", "fmul": f"{fA} * {fC}", "fdiv": f"{fA} / {fB}",
               "fmadd": f"{fA} * {fC} + {fB}", "fmsub": f"{fA} * {fC} - {fB}",
               "fnmadd": f"-({fA} * {fC} + {fB})", "fnmsub": f"-({fA} * {fC} - {fB})",
               "fsqrt": f"sqrt({fB})", "fsel": f"({fA} >= 0.0) ? {fC} : {fB}", "frsqrte": f"1.0 / sqrt({fB})"}
        if n in dbl: return f"{fD} = {dbl[n]};"
        sgl = {k + "s": v for k, v in dbl.items() if k not in ("fsel", "frsqrte")}
        sgl["fres"] = f"1.0 / {fB}"
        if n in sgl: return f"{{ double v = (double)(float)({sgl[n]}); {fD} = v; {F1(rD)} = v; }}"
        if n == "fmr": return f"{fD} = {fB};"
        if n == "fneg": return f"{fD} = -{fB};"
        if n == "fabs": return f"{fD} = fabs({fB});"
        if n == "fnabs": return f"{fD} = -fabs({fB});"
        if n == "frsp": return f"{{ double v = (double)(float){fB}; {fD} = v; {F1(rD)} = v; }}"
        if n in ("fctiw", "fctiwz"):
            return f"FSETBITS({fD}, 0xFFF8000000000000ull | (u32)rt_fctiw({fB}, {1 if n == 'fctiwz' else 0}));"
        if n in ("fcmpu", "fcmpo"): return f"SET_CRF_F({i.crfD}, {fA}, {fB});"
        # ---------------- paired singles
        p = lambda e0, e1: f"{{ double v0 = (double)(float)({e0}); double v1 = (double)(float)({e1}); {fD} = v0; {F1(rD)} = v1; }}"
        A0, A1, B0, B1, C0, C1 = F0(rA), F1(rA), F0(rB), F1(rB), F0(i.f["rC"]), F1(i.f["rC"])
        ps = {"ps_add": (f"{A0}+{B0}", f"{A1}+{B1}"), "ps_sub": (f"{A0}-{B0}", f"{A1}-{B1}"),
              "ps_mul": (f"{A0}*{C0}", f"{A1}*{C1}"), "ps_div": (f"{A0}/{B0}", f"{A1}/{B1}"),
              "ps_madd": (f"{A0}*{C0}+{B0}", f"{A1}*{C1}+{B1}"), "ps_msub": (f"{A0}*{C0}-{B0}", f"{A1}*{C1}-{B1}"),
              "ps_nmadd": (f"-({A0}*{C0}+{B0})", f"-({A1}*{C1}+{B1})"),
              "ps_nmsub": (f"-({A0}*{C0}-{B0})", f"-({A1}*{C1}-{B1})"),
              "ps_muls0": (f"{A0}*{C0}", f"{A1}*{C0}"), "ps_muls1": (f"{A0}*{C1}", f"{A1}*{C1}"),
              "ps_madds0": (f"{A0}*{C0}+{B0}", f"{A1}*{C0}+{B1}"), "ps_madds1": (f"{A0}*{C1}+{B0}", f"{A1}*{C1}+{B1}"),
              "ps_sum0": (f"{A0}+{B1}", C1), "ps_sum1": (C0, f"{A0}+{B1}"),
              "ps_sel": (f"({A0}>=0.0)?{C0}:{B0}", f"({A1}>=0.0)?{C1}:{B1}"),
              "ps_res": (f"1.0/{B0}", f"1.0/{B1}"), "ps_rsqrte": (f"1.0/sqrt({B0})", f"1.0/sqrt({B1})"),
              "ps_merge00": (A0, B0), "ps_merge01": (A0, B1), "ps_merge10": (A1, B0), "ps_merge11": (A1, B1)}
        if n in ps: return p(*ps[n])
        if n == "ps_mr": return f"{fD} = {B0}; {F1(rD)} = {B1};"
        if n == "ps_neg": return f"{fD} = -{B0}; {F1(rD)} = -{B1};"
        if n == "ps_abs": return f"{fD} = fabs({B0}); {F1(rD)} = fabs({B1});"
        if n == "ps_nabs": return f"{fD} = -fabs({B0}); {F1(rD)} = -fabs({B1});"
        if n in ("ps_cmpu0", "ps_cmpo0"): return f"SET_CRF_F({i.crfD}, {A0}, {B0});"
        if n in ("ps_cmpu1", "ps_cmpo1"): return f"SET_CRF_F({i.crfD}, {A1}, {B1});"
        if n == "UNK":
            return f"rt_unimpl(c, 0x{a:08X}u, \"0x{i.word:08X}\");"
        return f"rt_unimpl(c, 0x{a:08X}u, \"{n}\");"


def main(dol_path, sym_path, out):
    dol = Dol(open(dol_path, "rb").read())
    funcs, jts = load_symbols(sym_path)
    L = Lifter(dol, funcs, jts)
    os.makedirs(out, exist_ok=True)
    entries = sorted(L.entries)
    per = 400
    nfiles = 0
    for k in range(0, len(entries), per):
        chunk = entries[k:k + per]
        with open(os.path.join(out, f"code_{k // per:03d}.c"), "w") as fh:
            fh.write('#include "cpu.h"\n#include "funcs.h"\n\n')
            for e in chunk:
                fh.write(L.lift(e) + "\n\n")
        nfiles += 1
    with open(os.path.join(out, "funcs.h"), "w") as fh:
        fh.write("#pragma once\n")
        for e in entries:
            fh.write(f"void f_{e:08X}(CPU *c);\n")
        for e in sorted(WRAP):
            fh.write(f"void orig_{e:08X}(CPU *c);\n")
    with open(os.path.join(out, "functable.c"), "w") as fh:
        fh.write('#include "cpu.h"\n#include "funcs.h"\n\nconst FuncEntry g_funcs[] = {\n')
        for e in entries:
            fh.write(f"    {{0x{e:08X}u, f_{e:08X}}},\n")
        fh.write(f"}};\nconst u32 g_nfuncs = {len(entries)};\n")
        fh.write(f"const u32 g_entry = 0x{dol.entry:08X}u;\n")
    print(f"{len(entries)} entry points ({len(entries) - len(funcs)} mid-function), {nfiles} source files")


if __name__ == "__main__":
    main(*sys.argv[1:])
