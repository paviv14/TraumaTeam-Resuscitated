# Trauma Team Resuscitated

An **unofficial**, free PC port of **Trauma Team** (Atlus, Wii, 2010, USA), made by static
recompilation: the game's own code turned into a native Windows program. Sharp up to 4K and beyond,
widescreen and ultrawide, keyboard and mouse, any controller, or a real Wii Remote - and an
experimental VR mode.

> Not affiliated with, endorsed by or connected to Atlus, Sega or Nintendo. **No game data is
> included** - not the disc, not its files, not code made from it. You need your own copy of the game:
> Setup builds the port on your PC from your disc. It is free and always will be; if someone sold it
> to you, please ask for a refund.

**Status: beta.** The game is playable on PC, but you will meet bugs - please
[report them](#reporting-bugs).

## Installing (Windows 10 / 11, 64-bit)

You need:

* **Your own Trauma Team (USA, `SUOE01`) disc image** - `.iso`, `.rvz`, `.wbfs`, `.wia` or `.gcz`, or a
  folder you extracted with Dolphin. Other regions are not supported yet.
* A graphics card with OpenGL 3.3 (any from the last ten years), about **8 GB** of free space while
  installing, and an internet connection for the tools Setup downloads (about 400 MB).

Then:

1. Download this repository: the green **Code** button > **Download ZIP**, or the source zip of the
   [latest release](../../releases/latest). Unzip it somewhere with a short path, such as `C:\Games\TraumaTeam`.
2. Double-click **`Setup.bat`**, pick your disc image and press **Install**. Setup checks that the disc
   is the right one, downloads a compiler, turns the game's code into a PC program and builds it.
   This takes 10 to 30 minutes, once.
3. Play with the **Trauma Team Resuscitated** shortcut it makes (or `bin\Resuscitated.exe`).

Optional in Setup: *Show keyboard and controller button prompts in the tutorial pictures
(Experimental)* redraws the tutorial pictures with your buttons instead of the Wii Remote (one more
download of about 400 MB, about 5 more minutes).

**Texture pack (optional):** put the pack's files in a folder named `textures` inside `bin`. The game
picks it up by itself; the first run with it is slower while the textures are prepared.

## Playing

| | Keyboard and mouse | Controller |
|---|---|---|
| Pointer | mouse | right stick |
| A / B | left click / right click | A / B (Cross / Circle) |
| Nunchuk stick | W A S D | left stick |
| Options | **F1** | **Back + Start** |

Every motion of the Wii version has its own button, and the on-screen prompts follow the device you
use. Full list: [docs/CONTROLS.md](docs/CONTROLS.md). All controls can be changed in the options (F1).

* **F11** fullscreen, **F9** saves a picture of the moment (for bug reports), **F1** options:
  resolution, widescreen / ultrawide, controls, texture pack, sound.
* A real **Wii Remote** (with Nunchuk) paired to Windows over Bluetooth works with pointing, motion,
  rumble and its speaker; it needs a sensor bar, as on a Wii.

## Reporting bugs

Bug reports are very welcome - visual glitches, wrong sounds, controls that feel off, crashes, all of it.

1. When something goes wrong, press **F9** in the game.
2. Run **`Report a bug.bat`** in the game folder. It puts a zip with the game's logs, your settings and
   that picture on your Desktop (your Windows user name is removed) and opens the
   [bug report form](../../issues/new?template=bug_report.yml).
3. Say what happened and where, and drag the zip in.

Please don't attach files from the game disc or link to downloads of the game.

## Building it yourself (developers)

Setup does all of this on Windows. By hand you need your extracted disc, Python 3.9+, gcc (Linux) or
mingw-w64, and SDL2:

```
python -m recomp.lift <disc>/DATA/sys/main.dol data/symbols.txt generated   # your disc's code -> C
cd recomp
make -f build.mk -j                     # Linux: ../build/ttpc
make -f build.mk -j TARGET=windows      # Windows: ../build-win/Resuscitated.exe (needs third_party/, see docs/RECOMP.md)
```

Run it next to your extracted disc (it looks for `disc/DATA`, `../disc/DATA`, ...; or set `TTPC_DISC`).
The generated code and the program you build contain the game's code: keep them to yourself.
How the recompiler works, status and history: [docs/RECOMP.md](docs/RECOMP.md).

# Modding toolkit

The script and dialogue layer is fully readable **and editable**.

## What works

| | |
|---|---|
| Flowscripts (`.bf`, Atlus FLW0, same family as Persona 3/4) | parse, decompile to pseudo-code, edit as text, rebuild |
| Messages (`.bm2`, Atlus MSG2) | parse, edit as text, rebuild |
| Round-trip | all **2,791** `.bf` and **1,345** `.bm2` on the disc rebuild **byte-identical** |
| In-game | edited opening line (`comic/msg/eng/100_010.bm2`) confirmed on screen in Dolphin 2609a, booting the extracted disc via `DATA/sys/main.dol` |
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

1. Extract the disc in Dolphin (right-click the game > Properties > Filesystem > Extract Entire Disc).
2. Copy the rebuilt `.bf`/`.bm2` over the original (keep the original as `.orig`).
3. In Dolphin, File > Open > `<extracted>/DATA/sys/main.dol`. Dolphin boots the folder as a disc.

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
docs/RECOMP.md      native port: how the recompiler works, status, roadmap
recomp/             PowerPC -> C static recompiler + runtime (C)
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
