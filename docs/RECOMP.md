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
