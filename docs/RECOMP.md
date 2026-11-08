# Native PC port: static recompilation

## How it works

```
main.dol --(recomp/lift.py)--> generated/*.c  --(gcc/clang)--> build/ttpc
                                      +  recomp/runtime/  (CPU state, memory, hardware, HLE)
```

* Every guest function (14,277 from dtk + 36 mid-function entry points) becomes `void f_XXXXXXXX(CPU *c)`.
  Branches are gotos, calls are C calls, `blr` is `return`, `bctr` switch tables become `switch`.
* Guest memory is a flat host buffer indexed by physical address (MEM1 24 MB, MEM2 64 MB); loads and
  stores byte-swap. Hardware registers (`0x0C......`, `0x0D......`) go to `mmio_read/mmio_write`.
* Generated functions are **weak**: `recomp/runtime/hle.c` replaces any of them with a native version
  (High-Level Emulation). This is how hardware the PC does not have gets replaced.
* Generated code is derived from the game binary and is **never committed** (`generated/`, `build/` ignored).

## Build

```
python -m recomp.lift <disc>/DATA/sys/main.dol <dtk>/config/symbols.txt generated
make -C recomp -f build.mk -j
./build/ttpc <disc>/DATA/sys/main.dol [timeout-seconds]
```
(`symbols.txt` comes from `dtk dol split` on main.dol; see FINDINGS.md.)

## Status - milestone 1: boots natively through low-level SDK init

* All 190 instruction kinds used by the game are translated, including paired singles and
  quantized loads/stores. The full game compiles to a 21 MB native binary in ~2 minutes.
* Boot runs `__start` -> C runtime init -> `OSInit`: cache/arena setup, EXI bus, RTC/SRAM reads,
  memory protection (RealMode/rfi), syscall setup, thread system, IPC driver init.
  **161 distinct game functions execute natively**, no crashes.
* Stops at the first IOS request: the main thread sleeps waiting for an IPC reply interrupt that
  nothing delivers yet.

Fixed along the way: dtk-missed switch tables (any data pointer into a function is a jump target),
.sdata overlapping the DOL bss range, `sc` (sync-only syscall), `rfi` for RealMode, EXI transfers.

## Status - milestone 2: system services, threads and interrupts

* **IOS stand-in** (`runtime/ios.c`): replaces `__ios_Ipc2`. Answers ES (title ID, data dir), FS/NAND
  (host folder `nand/`), STM, DI (disc drive, correct 1=success codes), KD. Async replies are queued and
  delivered as IPC interrupts (the DVD driver arms timeout alarms after issuing requests).
* **Threads** (`runtime/threads.c`): every guest OSContext runs on a host fiber; SelectThread/
  OSSaveContext are wrapped (original body kept as `orig_*`), OSLoadContext switches fibers or unwinds.
* **Interrupts**: polled on every loop back-edge when MSR[EE] is set. VI retrace at 60 Hz, decrementer
  (OSAlarm), GPU finish/token (headless: detected in the GX FIFO stream), IPC replies.
* Audio interface sample counter runs at 48 kHz.
* **699 game functions run natively**; the game creates its own worker thread and enters init.
* Next blocker: the AX audio library boots its DSP mixing program and waits for the DSP's handshake
  mail (`fn_80246120`/`fn_80246700` polling `0x0C005004`). Plan: HLE AX (native mixer), as Dolphin's
  AX HLE does, instead of emulating the DSP.

## Status - milestone 3: audio, Bluetooth and GPU front end

* **Audio DSP stand-in** (`runtime/dsp.c`): plays the DSP side of the mailbox protocol (boot ROM
  greeting 0x8071FEED, 10-mail program boot, task INIT 0xDCD10000 + DSP interrupt). **Audio DMA**
  "plays" buffers at 48 kHz and raises AI interrupts; AX now sends its per-frame command lists
  (0xBABE0080 + address) - processing them (mixing) is the AX HLE still to write.
* **Bluetooth**: an idle HCI controller inside the IOS stand-in answers the stack's 15 setup commands;
  WPAD initialises with no Wii Remotes. Input for the port will be injected at the WPAD level.
* **GPU front end** (`runtime/gx.c`): the command stream is written into the game's FIFO memory at the
  PI write pointer (ready for the renderer). A headless "GPU" honours FIFO breakpoints (holding later
  events until released), draw-done and draw-sync tokens.
* Interrupt causes now follow device status (one dispatch services one source), nested delivery is
  blocked per thread, and a crash handler replaces the old call-depth counter.
* **1,227 functions run natively, 4 threads, 2 complete frames** through the game's frame pipeline.
* Next blocker: frame 3. The game's retrace callback (`fn_8000C660`) flips the framebuffer only when
  the draw-sync token equals last+1; the job queue (`fn_8000C5C0`/`fn_8000C720`) then stalls. Needs
  closer study of how the game paces GPU completion vs. VI retrace.

## Status - milestone 4: frames flow, audio heartbeat, disc reads

* **Frame pipeline runs**: the GPU front end models the GPU read position (stops exactly at armed
  breakpoints, PE events fire as it passes them) and paces frame completion to the 60 Hz retrace -
  the game's flip logic requires draw-sync token == last + 1 at each retrace. 650+ frames per 30 s.
* **AX heartbeat**: every command list is acknowledged like the AX program does (yield 0xDCD10002 +
  DSP interrupt); the NW4R sound system runs and saves its settings. Mixing still to do.
* **Disc** (`runtime/disc.c`): fst.bin is loaded to the top of MEM1 and published at 0x80000038 like
  the apploader does; drive reads (DI 0x71/0x8D/0x70) are served from an extracted disc folder
  (`TTPC_DISC=<disc>/DATA`). The game loads gffData.bin, font.brfnt and the Home Button menu, and
  reaches the title-screen task (`TitleCalc`).
* **1,302 functions run natively.** Next file requested: `sound/eng/sys/00000000/static.brsar`.
* Debug aids: `RECOMP_LOG=<addrs>` at lift time logs function entries (`rt_logfn` in hle.c);
  `TTPC_SEEN=<file>` dumps every game function that ran; `TTPC_PEEK` / `TTPC_WATCH` at timeout.

## Status - milestone 5: boots to its first screens, rendering continuously (headless)

* Runs indefinitely without stalls: ~3.5 MB/s of GPU commands (417 MB in 2 minutes), 2,171 functions.
* Game screens reached: `RatingCalc/RatingDraw` (boot rating notice), `TitleCalc`, fades, background,
  sprites, message balloons, comic panels (`itfComaBack/Chara`), bust-up portraits.
* Assets loaded through the disc stand-in: font, effects, Home Button menu, sound archive
  (`static.brsar`), title texture `title/hanken.tpx`, effect sprites, first 3D model
  (`model/tool/0501-01-loupe.brres`).
* Fixes that got here:
  - GPU events only advance at interrupt polls (never inside a register write);
  - PE status bits read back as 0 (write-1-to-clear only) - the token handler's read-modify-write
    acknowledge was wiping a pending draw-done;
  - a reused thread object starts a fresh fiber (context points at an entry, not SelectThread's
    resume point);
  - nested interrupts allowed whenever MSR[EE] is set (SelectThread may idle inside an interrupt);
  - diagnostics: exact per-thread backtraces at timeout, GPU state dump, safe stack walking.
* Next: (1) **renderer** - translate the GX FIFO the game is producing so the screens become visible;
  (2) **input** - inject a virtual Wii Remote at the WPAD level so the game can be advanced past
  "press A"; (3) AX mixing for sound.

## Status - milestone 6: renders the boot sequence to the main menu

* **Software renderer** (`runtime/video.c`): decodes the GX FIFO (CP/XF/BP registers, vertex formats,
  display lists), transforms, rasterizes with the TEV combiner, all GameCube/Wii texture formats incl.
  palettes and CMPR, alpha test, blending, depth; EFB->XFB copies are saved as frames
  (`TTPC_FRAMES=<dir> TTPC_FRAME_EVERY=N`). `TTPC_SCALE=S` renders at S x internal resolution
  (6 = 3840x2880): geometry is sharp, the 2D art is still the original textures.
* Runs natively through: wrist-strap safety screens -> NAND/save-data check (creates `save.dat`,
  `banner.bin`) -> Atlus logo -> title -> **NEW GAME / CONTINUE main menu** with animated background.
  2,422 functions.
* Fix that got here: IOS replies now arrive after a short latency (200 us). The game stores an async
  NAND call's return value *after* issuing it; a reply delivered before the call returned was
  overwritten, so a missing save file looked like a failed close (-8) -> "error accessing Wii system
  memory" screen.
* Debug aid: build with `-DTTPC_WATCHPOINT`, run with `TTPC_WP=<addr>` to log every store to a word.
* Next: real-time GPU backend (window, 4K, widescreen, texture filtering), input (virtual Wii Remote),
  AX mixing, near-plane clipping/lighting.

## Status - milestone 7: playable window on Windows and Linux (480p, mouse + keyboard)

* **Window** (`runtime/platform.c`, SDL2): every displayed frame is presented at 640x480 (window 2x,
  F11 fullscreen). Frame skipping keeps the game at full speed when drawing falls behind.
* **Virtual Wii Remote + Nunchuk** (`runtime/wiimote.c`): replaces `WPADProbe` (fn_802533C0) and
  `KPADReadEx` (fn_8025F830, 0xB0-byte KPADStatus) - mouse = pointer, clicks = A/B, arrows = D-pad,
  WASD = Nunchuk stick (full mapping in the play folder's README). Headless tests can script input:
  `TTPC_INPUT="1500+6:A;1800+6:A" TTPC_POINTER="-0.3,0.3"`.
  With it the game shows the Nunchuk notice, and NEW GAME -> difficulty select -> disclaimer works.
* **Renderer speed**: textures decoded once per frame into a cache, TEV/blend state decoded once per
  draw, rows spread across cores (OpenMP). ~10 -> ~26 fps on a 2-core VM; game logic alone runs 60.
* **Windows build** (`make -f build.mk TARGET=windows`, mingw-w64): `runtime/port.c` provides fibers
  (Win32 fibers / ucontext), non-unwinding jumps (`__builtin_setjmp`), clock, memory, crash handler.
  Native replacements are no longer weak symbols (PE has none): the lifter scans `recomp/runtime` for
  `f_XXXXXXXX` / `HLE()` definitions and leaves those bodies out. SDL2.dll comes from the
  `pysdl2-dll` wheel, headers from the SDL repo (in `third_party/`, not committed).
* The exe finds the disc by itself (`disc/DATA`, `../disc/DATA`, `../../disc/DATA` from the working
  folder or the exe); `TTPC_DISC` overrides; no timeout unless a second argument / `TTPC_TIMEOUT`.
* Next: what follows the disclaimer (black - likely the opening movie: THP video), sound (AX),
  controllers / real Wii Remote, then the GPU backend for speed and 4K.

## Status - milestone 8: GPU rendering, correct colors

* **OpenGL 3.3 backend** (`runtime/gl.c`): EFB as an off-screen framebuffer, XFB copies blitted to the
  window, an uber fragment shader running the TEV combiner and alpha test from uniforms, GX blend /
  depth / color-mask / scissor mapped to GL state. Vertices are still transformed on the CPU and mapped
  through the GX viewport into clip space (perspective-correct, GL clips, depth clamp on).
  `TTPC_SOFTWARE=1` keeps the CPU rasterizer; shared state lives in `runtime/gx_state.h`.
* **Colors**: TEV swap tables (BP 0xF6-0xFD, selected per stage by the alpha-combiner bits 0-3) were
  missing - green showed red's value (skin turned yellow, cyan text dark blue). Now matches Dolphin.
* Texture cache revalidates by hashing source bytes once per frame; GL textures re-upload only on change.
* Tested under Xvfb + Mesa llvmpipe (Linux) and Wine (Windows exe).

## Roadmap to "opening sequence -> main menu in 4K"

| # | Piece | Approach |
|---|---|---|
| 2 | **IOS** (files, NAND saves, ES, STM, disc) | HLE the SDK IPC layer (`IOS_Open/Ioctl/Read...`, `__ios_Ipc2`): answer requests natively, run callbacks |
| 3 | **Threads + interrupts** | Each guest OS thread on a host fiber; HLE `OSLoadContext/OSSleepThread/OSWakeupThread`; deliver VI/DSP/PI interrupts from a host timer |
| 4 | **Disc access** | DVD driver HLE reading from the extracted `DATA/files` |
| 5 | **Graphics** | Translate the GX command FIFO to a modern API. Adapt Dolphin's VideoCommon (GPL) or write a GX->Vulkan/D3D12 layer; render at 4K + widescreen |
| 6 | **Audio** | HLE AX (the SDK mixer) to SDL audio |
| 7 | **Input** | One virtual Wii Remote fed by mouse+keyboard, gamepad (stick or gyro pointer), or a real Wii Remote over Bluetooth |
| 8 | Enhanced assets | Upscaled comic art / textures through a texture-replacement hook in the renderer |

Pieces 2-4 get the game logic running headless; 5 makes the opening visible.
