"""Gekko/Broadway (PowerPC 750CL) instruction decoder for the static recompiler.

decode(word, addr) -> Insn(name, fields...) ; unknown encodings decode as name="UNK".
Field names follow the PowerPC manuals: rD rS rA rB, frD frA frB frC, crfD, BO BI BD LI, SIMM UIMM d,
SH MB ME, Rc OE LK AA. Paired-single loads/stores use the GQR quantization register field I/W.
"""
from dataclasses import dataclass, field


@dataclass
class Insn:
    addr: int
    word: int
    name: str
    f: dict = field(default_factory=dict)

    def __getattr__(self, k):
        try:
            return self.f[k]
        except KeyError:
            raise AttributeError(k)


def s16(x): return x - 0x10000 if x & 0x8000 else x


def bits(w, a, b):
    """PowerPC bit numbering: bit 0 is the MSB."""
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


# opcode 31 extended (bits 21-30)
X31 = {
    0: "cmp", 4: "tw", 8: "subfc", 10: "addc", 11: "mulhwu", 19: "mfcr", 20: "lwarx", 23: "lwzx", 24: "slw",
    26: "cntlzw", 28: "and", 32: "cmpl", 40: "subf", 54: "dcbst", 55: "lwzux", 60: "andc", 75: "mulhw",
    83: "mfmsr", 86: "dcbf", 87: "lbzx", 104: "neg", 119: "lbzux", 124: "nor", 136: "subfe", 138: "adde",
    144: "mtcrf", 146: "mtmsr", 150: "stwcx.", 151: "stwx", 183: "stwux", 200: "subfze", 202: "addze",
    210: "mtsr", 215: "stbx", 232: "subfme", 234: "addme", 235: "mullw", 246: "dcbtst", 247: "stbux",
    266: "add", 278: "dcbt", 279: "lhzx", 284: "eqv", 306: "tlbie", 311: "lhzux", 316: "xor", 339: "mfspr",
    343: "lhax", 371: "mftb", 375: "lhaux", 407: "sthx", 412: "orc", 439: "sthux", 444: "or", 459: "divwu",
    467: "mtspr", 470: "dcbi", 476: "nand", 491: "divw", 512: "mcrxr", 534: "lwbrx", 535: "lfsx", 536: "srw",
    567: "lfsux", 595: "mfsr", 597: "lswi", 598: "sync", 599: "lfdx", 631: "lfdux", 662: "stwbrx",
    663: "stfsx", 695: "stfsux", 725: "stswi", 727: "stfdx", 759: "stfdux", 790: "lhbrx", 792: "sraw",
    824: "srawi", 854: "eieio", 918: "sthbrx", 922: "extsh", 954: "extsb", 982: "icbi", 983: "stfiwx",
    1014: "dcbz",
}
# opcode 19 extended
X19 = {0: "mcrf", 16: "bclr", 33: "crnor", 50: "rfi", 129: "crandc", 150: "isync", 193: "crxor",
       225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror", 528: "bcctr"}
# opcode 63 extended (10-bit)
X63 = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1", 40: "fneg",
       64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs", 264: "fabs", 583: "mffs", 711: "mtfsf"}
# opcode 63 A-form (5-bit, bits 26-30)
A63 = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul", 26: "frsqrte",
       28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
A59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls",
       28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}
# opcode 4 (paired singles)
X4 = {0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 64: "ps_cmpu1", 72: "ps_mr", 96: "ps_cmpo1",
      136: "ps_nabs", 264: "ps_abs", 528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10",
      624: "ps_merge11", 1014: "dcbz_l"}
A4 = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0", 15: "ps_madds1",
      18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res", 25: "ps_mul", 26: "ps_rsqrte",
      28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub", 31: "ps_nmadd"}
X4_6 = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}  # bits 25-30

D_OPS = {
    7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.", 14: "addi", 15: "addis",
    24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis.",
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
    40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
    48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
    56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu", 3: "twi",
}


def decode(w, addr=0):
    op = w >> 26
    rD = bits(w, 6, 10); rA = bits(w, 11, 15); rB = bits(w, 16, 20); rC = bits(w, 21, 25)
    Rc = w & 1
    F = dict(rD=rD, rS=rD, rA=rA, rB=rB, rC=rC, Rc=Rc, frD=rD, frS=rD, frA=rA, frB=rB, frC=rC)
    name = "UNK"
    if op in D_OPS:
        name = D_OPS[op]
        F.update(SIMM=s16(w & 0xFFFF), UIMM=w & 0xFFFF, d=s16(w & 0xFFFF), crfD=rD >> 2, L=rD & 1)
        if op in (56, 57, 60, 61):  # psq: W(16) I(17-19) d12
            F.update(W=bits(w, 16, 16), I=bits(w, 17, 19), d=(w & 0xFFF) - (0x1000 if w & 0x800 else 0))
    elif op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000: li -= 0x04000000
        AA = (w >> 1) & 1; LK = w & 1
        name = "b"; F.update(LI=li, AA=AA, LK=LK, target=(li if AA else addr + li) & 0xFFFFFFFF)
    elif op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000: bd -= 0x10000
        AA = (w >> 1) & 1; LK = w & 1
        name = "bc"; F.update(BO=rD, BI=rA, BD=bd, AA=AA, LK=LK, target=(bd if AA else addr + bd) & 0xFFFFFFFF)
    elif op == 17:
        name = "sc"
    elif op == 19:
        xo = bits(w, 21, 30)
        name = X19.get(xo, "UNK")
        F.update(BO=rD, BI=rA, LK=w & 1, crbD=rD, crbA=rA, crbB=rB, crfD=rD >> 2, crfS=rA >> 2)
    elif op in (20, 21, 23):
        name = {20: "rlwimi", 21: "rlwinm", 23: "rlwnm"}[op]
        F.update(SH=rB, MB=bits(w, 21, 25), ME=bits(w, 26, 30))
    elif op == 31:
        xo = bits(w, 21, 30)
        OE = bits(w, 21, 21)
        name = X31.get(xo) or X31.get(xo & 0x1FF, "UNK")  # XO-form with OE bit set
        if xo not in X31 and (xo & 0x1FF) in X31 and OE:
            F["OE"] = 1
        spr = (rB << 5) | rA
        F.update(crfD=rD >> 2, L=rD & 1, SH=rB, spr=spr, CRM=bits(w, 12, 19), TO=rD, NB=rB)
    elif op == 59:
        name = A59.get(bits(w, 26, 30), "UNK")
    elif op == 63:
        xo = bits(w, 21, 30)
        name = X63.get(xo) or A63.get(bits(w, 26, 30), "UNK")
        F.update(crfD=rD >> 2, FM=bits(w, 7, 14))
    elif op == 4:
        xo = bits(w, 21, 30)
        if xo in X4:
            name = X4[xo]
        elif bits(w, 25, 30) in X4_6:
            name = X4_6[bits(w, 25, 30)]
            F.update(W=bits(w, 21, 21), I=bits(w, 22, 24))
        else:
            name = A4.get(bits(w, 26, 30), "UNK")
        F.update(crfD=rD >> 2)
    return Insn(addr, w, name, F)
