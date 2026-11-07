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
