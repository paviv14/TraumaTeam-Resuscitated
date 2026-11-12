"""Chapter scripts -> JSON for the Godot VM: bytecode, procedures, labels, messages (text + voice), strings."""
import sys, os, json, glob, struct, re
sys.path.insert(0, '/root/TraumaTeamRE')
from ttre.flow import Flow, OPS
from ttre.msg2 import Msg2
from ttre.commands import Commands
C = Commands()

def page_parts(p):
    """MSG2 page -> {'text': str, 'voice': int|None, 'tags': [...]} (text keeps \n; tags kept in order)."""
    text = ''; voice = None; tags = []; i = 0; raw = bytearray()
    def flush():
        nonlocal raw, text
        if raw: text += bytes(raw).decode('shift_jis', 'replace'); raw = bytearray()
    while i < len(p):
        c = p[i]
        if c == 0xFF and i + 5 <= len(p):
            tid, argc = struct.unpack('>HH', p[i + 1:i + 5])
            if i + 5 + 6 * argc <= len(p):
                args = [struct.unpack('>Hi', p[i + 5 + 6 * k:i + 11 + 6 * k])[1] for k in range(argc)]
                flush()
                if tid == 0x04 and len(args) >= 2: voice = args[1]
                elif tid == 0x0E: text += '[c%d]' % args[0]
                elif tid not in (0x10, 0x00): tags.append([tid] + args)
                i += 5 + 6 * argc; continue
        if c == 0: flush(); text += '\n'; i += 1; continue
        raw.append(c); i += 1
    flush()
    return {'text': text.strip('\n'), 'voice': voice, 'tags': tags}

def msgs_json(blob):
    if len(blob) < 0x30: return []
    m = Msg2.parse(blob)
    out = []
    for x in m.messages:
        pages = [page_parts(p) for p in x.pages] if x.raw_body is None else []
        out.append({'name': x.name, 'speaker': x.speaker, 'pages': pages})
    return out

def flow_json(path):
    f = Flow.parse(open(path, 'rb').read())
    pos = f.code_positions()
    code = []
    for name, arg, extra in f.code:
        if name == 'COMM': code.append(['COMM', arg, C.name(arg), C.argc(arg)])
        elif name in ('PUSHF',): code.append([name, struct.unpack('>f', struct.pack('>I', extra))[0]])
        elif name == 'PUSHI': code.append([name, extra if extra < 0x80000000 else extra - (1 << 32)])
        elif name == 'PUSHIS': code.append([name, arg if arg < 0x8000 else arg - 0x10000])
        else: code.append([name, arg])
    strings = {}
    for mm in re.finditer(rb'[^\0]+', f.strings): strings[str(mm.start())] = mm.group().decode('latin1')
    return {'procs': {n: pos[i] for n, i in f.procs}, 'proc_list': [n for n, _ in f.procs],
            'labels': [pos[i] for _, i in f.labels], 'code': code, 'messages': msgs_json(f.msg), 'strings': strings}

if __name__ == '__main__':
    root, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    n = 0
    for fn in sorted(glob.glob(os.path.join(root, '**', '*.bf'), recursive=True)):
        rel = os.path.relpath(fn, root)[:-3].replace(os.sep, '/')
        d = os.path.join(out, os.path.dirname(rel)); os.makedirs(d, exist_ok=True)
        json.dump(flow_json(fn), open(os.path.join(out, rel + '.json'), 'w'), ensure_ascii=False)
        n += 1
    for fn in sorted(glob.glob(os.path.join(root, '**', '*.bm2'), recursive=True)):
        rel = os.path.relpath(fn, root)[:-4].replace(os.sep, '/')
        os.makedirs(os.path.join(out, os.path.dirname(rel)), exist_ok=True)
        json.dump({'messages': msgs_json(open(fn, 'rb').read())}, open(os.path.join(out, rel + '.msg.json'), 'w'), ensure_ascii=False)
        n += 1
    print(n, 'files')
