# Unused content in Trauma Team (SUOE01)

Findings from the US disc and main.dol. Nothing here is game data; addresses are for SUOE01.

## Scene ("sequence") table

The game runs as numbered sequences: a table of (init, is-done, exit) thunks at 0x803413B0,
switched by `f_80007760(seq, a, b)`; `f_80007700` boots sequence 1 (title). Retail only ever
switches between 1 (title), 14 (game) and 15 (title again). The rest are developer scenes:

| # | name (from its task names) | state in the port |
|---|---|---|
| 0 | StartMenu | empty: its task returns 0 (the menu was compiled out) |
| 3 | ModelTest | runs |
| 4 | ScriptTest (/script/test.bf) | |
| 5 | effect viewer | runs (shows "error: details unknown" without a file) |
| 6 | SoundTest | runs |
| 7 | ModelViewer | runs |
| 9 | SpriteViewer | runs (same error box) |
| 10 | SELECT TEST: ItfList, Sprite, StoryFlow, LMap, OpeInterface, ComicEventTest, ComicEvent, ScnAnm_Camera, Conference, ComaBack, ComaChara, EpisodeSelect | runs |
| 11/12 | BustupTest, BackGroundTest | black |
| 16/17 | MovieTest, ModelHitEditor | black |
| 22 | effect editor (gffEdit) | runs |
| 23 | TestMHTest | runs |
| 24 | MessageDebug (/msg_work_us/*.bm2: not on the disc) | |
| 25 | staff roll | needs context |
| 26 | comic event editor | runs |
| 28 | **DNALogicPuzzle** — cut minigame prototype | runs |
| 29 | **MedicinePuzzle** — cut minigame prototype (RGB colour mixing) | runs |
| 30 | **VirusActionGame** — cut minigame prototype | runs |
| 31 | HiuraTest | black |
| 32 | stgSelectTest: STAGE SELECT (difficulty, mode, chapter, story; play all or one part) | runs |
| 40 | autSelectTest (forensics parts) | runs |
| 42/44/45 | endoscope lesion setter, debug stage menu, path data tool | run |
| 49/50 | forensics recorder / name plate | black |
| 51 | diaSelectTest (diagnosis parts) | runs |
| 55 | autCharnel | runs |
| 56 | comic viewer (page id) | runs |

The options menu's "Developer scene" switches the running game to one (hle.c wraps the sequence
task, f_80007690). Booting straight into one (TTPC_SEQ=N) skips the console start-up the title does.

## Chapter 0 missions

`operation2/data/` holds missions named mode + chapter (hex) + episode (2 hex digits). Chapter 0
(su000-su090, em000/em010, en000-en050, or000-or090) has Japanese text only: engine tests
(su000 "SLASH_OK was called"), a text layout test (su090) and early drafts of story missions with
voiced-style dialogue (su003: Gabriel's "last transmission"). The dev STAGE SELECT lists them
(chapter 0, story 0/3/5/7...). Launching one currently stops in the Bluetooth stack: a timer event
reaches the HID module after its callback was cleared (0x8053384C, set by WUD to 0x80268B30) —
next thing to fix.

## Other leftovers

- `.svn` folders, `script/test.*`, `storyflow/msg/*.msg` + `.h` sources and `conv.bat` scripts.
- `operation/data/OPE*.DAT`: data for an older surgery engine.
- Japanese text for most modes (jpn folders) next to the English.
