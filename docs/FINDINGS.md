# Findings

## Disc (USA)

* Developer leftovers on the disc: `.svn` folders (empty), `script/test.scr` + `test_mes.msg`
  (Atlus script source), `storyflow/msg/*.msg` (message source), `conv.bat` files showing the
  internal tools `hospital/planner/data/bin/msgconv` and `scr2bin`. The Japanese title was *Hospital.*
* Both English and Japanese script/message sets ship on the USA disc.

## main.dol (dtk analysis, 14,277 functions)

| Range | Contents | Size |
|---|---|---|
| 0x80004000-0x80007360 | .init / startup | ~13 KB |
| 0x80007360-0x80206000 | Atlus game code (with inlined NW4R headers) | 9,430 funcs, ~1.98 MB |
| 0x80206000-0x80219000 | MSL C library, CodeWarrior runtime, MetroTRK | 343 funcs, ~76 KB |
| 0x80219000-0x80296000 | Revolution SDK (OS, GX, DVD, NAND, WPAD, Bluetooth) | 1,968 funcs, ~492 KB |
| 0x80296000-0x80335BE0 | Nintendo libraries, almost certainly NW4R + Home Button menu | 2,498 funcs, ~624 KB |

* SDK build: `release build: Feb 27 2009` (WPAD Jun 22 2009). Compiler: Metrowerks CodeWarrior.
* No symbol map shipped. 97 task names (`autInspection`, `escpMainCalcProc`, `evtComicMessageDraw`...)
  are registered as strings and anchor the game's subsystems.
* Debug tools still compiled in: ModelViewer, ModelHitEditor, MessageDebug, SpriteTest, SoundTest,
  comic and effect editors.
* Sound: NW4R snd with `static.brsar`, manager around 0x80024000-0x80027000.

## Script VM

* Command dispatch table at **0x80335D70**: 480 x `{handler, argc}`, 347 non-null.
* Argument getters: `0x8002B920` (int), `0x8002B9F0` (float), `0x8002BAE0` (string).
  Return value setter: `0x8002BB20`. `0x8002BB60` returns the frames spent in the current command,
  which is how blocking commands (WAIT, SYNC...) work: return 0 to be called again next frame.
* Flag array: `BIT_ON/BIT_OFF/BIT_CHK` operate on a bitfield at `(*(r13-0x51E0)) + 0x80`.

## Message format (MSG2)

See `ttre/msg2.py`. Control tags are `FF, u16 id, u16 argc, argc x {u16 type, u32 value}`.
Known: 0x00 wait, 0x09 TELOP, 0x0B se_play, 0x0E color (type-2 arg).
Frequent unknown: 0x04 (2 args, starts most pages), 0x10 (arg -1, ends most pages),
0x02, 0x03, 0x11 (voice id?), 0x12 (voice bank/line?).
