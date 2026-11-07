# TraumaTeamRE

Reverse-engineering and modding toolkit for **Trauma Team** (Atlus, Wii, 2010, USA `SUOE01`).

Long-term goal: a PC recompilation with 4K rendering and quality-of-life improvements.
Current stage: the game's script and dialogue layer is fully readable **and editable**.

## What works

| | |
|---|---|
| Flowscripts (`.bf`, Atlus FLW0, same family as Persona 3/4) | parse, decompile to pseudo-code, edit as text, rebuild |
| Messages (`.bm2`, Atlus MSG2) | parse, edit as text, rebuild |
| Round-trip | all **2,791** `.bf` and **1,345** `.bm2` on the disc rebuild **byte-identical** |
| Script commands | 480-slot dispatch table found in `main.dol` (347 implemented); 29 named, covering 75% of all command calls in the game |

## Quick start (Python 3.9+, no dependencies)

```
python -m ttre extract  <disc>/DATA/files  out        # everything -> text
python -m ttre build    out/diagnostic/eng/110/diaMenu.flow  diaMenu.bf
python -m ttre decompile <file.bf>
python -m ttre verify   <disc>/DATA/files             # byte-exact round-trip check
python -m ttre stats    <disc>/DATA/files             # which commands are used how often
```

`extract` writes, for each script:

* `name.flow` - the **editable** form (assembly + dialogue). Rebuild it with `build`.
* `name.c` - a **read-only** pseudo-code view for understanding the logic.

and for each standalone message file, `name.msg2.txt` (rebuild with `build` too).

### Editing dialogue

Pages are quoted strings inside the `.messages` block of a `.flow` (or in a `.msg2.txt`):

```
.message "MSG_DI10_00_QUESTION_START_00" speaker=-1
  "<tag_04 0 0>Text goes here.\nSecond line.\n<tag_10 -1><wait>"
```

`\n` is a line break, `<...>` are control tags, `\xNN` is a raw byte. Text is Shift-JIS.
Keep line lengths similar to the original: the game does not re-wrap text.

### Editing logic

```
.proc #1 menu00
    PROC
    PUSHIS 1817
    COMM BIT_CHK
    PUSHREG
    ...
```

Arguments are pushed **last-first**. A command that returns a value is followed by `PUSHREG`.
`IF label` jumps when the condition is **false**. Commands print by name once they are in
`data/commands.csv`, otherwise as `CMD_XXX`; both spellings are accepted when building.

### Testing a mod

Copy the rebuilt `.bf`/`.bm2` over the original in an extracted copy of the disc
(Dolphin: right-click the game > Properties > Filesystem > Extract), then load it with a
Riivolution patch or rebuild the image. Always keep the original files.

## Layout

```
ttre/flow.py        FLW0 binary format
ttre/msg2.py        MSG2 binary format + text form
ttre/asm.py         lossless .flow text <-> binary
ttre/decompile.py   pseudo-code view
ttre/commands.py    command database loader
data/commands.csv   id, name, argc, msg_args, handler address in main.dol, confidence, evidence
data/msgtags.csv    message control tag names
data/commtabs.json  raw dispatch table dumped from main.dol
docs/FINDINGS.md    what is known about main.dol and the disc
tests/              round-trip and edit tests (need the game files)
```

## Naming conventions in `data/commands.csv`

* `source` - name taken from Atlus's own leftover `script/test.scr` on the disc
* `inferred` - confirmed by reading the handler's machine code
* `guess` - from usage patterns only; rename freely

To name a command: look up its `handler` address in `main.dol` (Ghidra / dtk), check how it is
used (`stats`, then grep the `.c` files), edit the CSV, re-run `verify`.

## Legal

This repository contains **no game data**. `.gitignore` excludes extracted files.
You need your own copy of the game. Do not distribute files extracted from the disc.
