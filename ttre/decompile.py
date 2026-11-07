"""Read-only pseudo-code view of a flowscript (for understanding; edit the .flow text instead).

Stack conventions (verified against Atlus's leaked test.scr):
  - command arguments are pushed last-first, so the first value popped is the first argument
  - binary operators: the value on top of the stack is the left operand
  - IF jumps to its label when the condition is FALSE
  - a command that returns a value is followed by PUSHREG
"""
import struct
from .flow import Flow
from .msg2 import Msg2

BIN = {"ADD": "+", "SUB": "-", "MUL": "*", "DIV": "/", "EQ": "==", "NEQ": "!=", "S": "<", "L": ">",
       "SE": "<=", "LE": ">=", "OR": "||", "AND": "&&"}


def _linear(f: Flow, cmds):
    """Turn bytecode into a flat statement list."""
    pos = f.code_positions()
    pmark, lmark = {}, {}
    for nm, wi in f.procs:
        pmark.setdefault(pos[wi], []).append(nm)
    for k, (nm, wi) in enumerate(f.labels):
        lmark.setdefault(pos[wi], []).append(k)
    msgnames = f.message_names()
    lab = lambda k: f.labels[k][0] if k < len(f.labels) else f"L{k}"
    out = []
    st = []
    pending = None

    def flush():
        nonlocal pending
        if pending:
            out.append(("stmt", pending)); pending = None

    def pop():
        return st.pop() if st else "?"
    for k, (name, arg, extra) in enumerate(f.code):
        if k in pmark or k in lmark:
            flush()
        for nm in pmark.get(k, []):
            out.append(("proc", nm))
        for li in lmark.get(k, []):
            out.append(("label", lab(li)))
        if name != "PUSHREG":
            flush()
        if name == "PUSHI":
            st.append(str(struct.unpack(">i", struct.pack(">I", extra))[0]))
        elif name == "PUSHF":
            st.append(f"{struct.unpack('>f', struct.pack('>I', extra))[0]:g}f")
        elif name == "PUSHIS":
            st.append(str(struct.unpack(">h", struct.pack(">H", arg))[0]))
        elif name == "PUSHREG":
            st.append(pending or "RESULT"); pending = None
        elif name in ("PUSHLIX", "PUSHLFX", "PUSHIX", "PUSHIF"):
            st.append({"PUSHLIX": "var", "PUSHLFX": "fvar", "PUSHIX": "gvar", "PUSHIF": "gfvar"}[name] + str(arg))
        elif name in ("POPLIX", "POPLFX", "POPIX", "POPFX"):
            v = {"POPLIX": "var", "POPLFX": "fvar", "POPIX": "gvar", "POPFX": "gfvar"}[name] + str(arg)
            out.append(("stmt", f"{v} = {pop()};"))
        elif name == "PUSHSTR":
            st.append('"' + f.string_at(arg).decode("cp932", "replace") + '"')
        elif name == "COMM":
            n = cmds.argc(arg)
            args = [pop() for _ in range(n)]
            cname = cmds.name(arg)
            for p in cmds.msg_args(arg):
                if p < len(args) and args[p].lstrip("-").isdigit():
                    i = int(args[p])
                    if 0 <= i < len(msgnames) and msgnames[i]:
                        args[p] = msgnames[i]
            pending = f"{cname}({', '.join(args)})"
        elif name in BIN:
            a = pop(); b = pop()
            if name in ("ADD", "SUB", "MUL") and a.lstrip("-").isdigit() and b.lstrip("-").isdigit():
                st.append(str({"ADD": int(a) + int(b), "SUB": int(a) - int(b), "MUL": int(a) * int(b)}[name]))
            else:
                st.append(f"({a} {BIN[name]} {b})")
        elif name == "NOT":
            st.append(f"!{pop()}")
        elif name == "MINUS":
            st.append(f"-{pop()}")
        elif name == "IF":
            out.append(("if", pop(), lab(arg)))
        elif name == "GOTO":
            out.append(("goto", lab(arg)))
        elif name in ("CALL", "JUMP", "RUN"):
            tgt = f.procs[arg][0] if arg < len(f.procs) else f"proc{arg}"
            out.append(("stmt", f"{tgt}();" if name == "CALL" else f"{name.lower()} {tgt};"))
        elif name == "END":
            out.append(("return",))
        elif name == "PROC":
            pass
        else:
            out.append(("stmt", f"/* {name} {arg} */"))
    flush()
    for li in lmark.get(len(f.code), []):
        out.append(("label", lab(li)))
    return [(s[0], s[1] + ";") if s[0] == "stmt" and not s[1].endswith(";") else s for s in out]


def _structure(stmts):
    refs = {}
    for s in stmts:
        if s[0] in ("if", "goto"):
            t = s[-1]; refs[t] = refs.get(t, 0) + 1
    where = {s[1]: i for i, s in enumerate(stmts) if s[0] == "label"}
    lines = []

    def emit(i, end, ind):
        while i < end:
            s = stmts[i]
            if s[0] == "if" and s[2] in where and i < where[s[2]] <= end:
                j = where[s[2]]
                body_end = j
                last = stmts[j - 1] if j - 1 > i else None
                if last and last[0] == "goto" and last[1] == s[2]:   # redundant goto to the same label
                    refs[s[2]] -= 1; body_end = j - 1
                    last = None
                if last and last[0] == "goto" and last[1] in where and j < where[last[1]] <= end:
                    k = where[last[1]]
                    refs[s[2]] -= 1; refs[last[1]] -= 1
                    lines.append(f"{ind}if {s[1]} {{")
                    emit(i + 1, j - 1, ind + "    ")
                    lines.append(f"{ind}}} else {{")
                    emit(j + 1, k, ind + "    ")
                    lines.append(f"{ind}}}")
                    i = k
                    continue
                refs[s[2]] -= 1
                lines.append(f"{ind}if {s[1]} {{")
                emit(i + 1, body_end, ind + "    ")
                lines.append(f"{ind}}}")
                i = j
                continue
            if s[0] == "proc":
                lines.append(f"\nprocedure {s[1]}() {{"); ind = "    "
            elif s[0] == "label":
                if refs.get(s[1], 0) > 0:
                    lines.append(f"{ind[:-4] if len(ind) >= 4 else ''}{s[1]}:")
            elif s[0] == "goto":
                lines.append(f"{ind}goto {s[1]};")
            elif s[0] == "if":
                lines.append(f"{ind}if !{s[1]} goto {s[2]};")
            elif s[0] == "return":
                nxt = stmts[i + 1][0] if i + 1 < len(stmts) else "proc"
                lines.append(f"{ind}return;")
                if nxt == "proc" and ind == "    ":
                    lines.append("}")
            else:
                lines.append(f"{ind}{s[1]}")
            i += 1
    emit(0, len(stmts), "")
    return "\n".join(lines).strip("\n") + "\n"


def decompile(f: Flow, cmds) -> str:
    return _structure(_linear(f, cmds))


def message_script(f_or_msg) -> str:
    m = Msg2.parse(f_or_msg.msg if isinstance(f_or_msg, Flow) else f_or_msg)
    return m.to_text()
