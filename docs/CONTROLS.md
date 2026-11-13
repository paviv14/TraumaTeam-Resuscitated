# Controls: the decided layout (13 Nov 2026)

Decided in the controls planner (artifact "Trauma Team controls planner"), checked for overlaps and settled.
Principle: no motion emulation where it can be avoided. Each motion is answered at the game function that
checks it, so one input can mean different things in different tools without clashing.

## Always

| Wii | Keyboard & mouse | Controller (Xbox; PlayStation in brackets) | Real Wii Remote |
|---|---|---|---|
| Pointer | mouse | right stick (RB re-centres) | IR pointer |
| A | left click / Space / Enter | A (Cross) | A |
| B | right click / Shift / X | B (Circle) only | B |
| A + B (grab) | left + right click | A + RT (Cross + R2) | A + B |
| RT alone | - | nothing: only the B half of A+RT and LT+RT | - |
| Z / C | E / Q | LT (L2) / LB (L1) | Z / C |
| Nunchuk stick | W A S D | left stick | stick |
| D-pad, + / - / Home | arrows, + / - (P / M), Esc | D-pad, Menu / View / Guide | same |
| Drag | hold left click, move | hold A: both sticks move the pointer, Nunchuk stick ignored until A is let go | real motion |

## Motions

| Motion | Keyboard & mouse | Controller | Real Wii Remote |
|---|---|---|---|
| Twist while holding (fragments, tray, skull puzzle) | grab with left + right, then keep only left held: grab stays, mouse wheel turns | hold A + RT, LB / RB turn | twist |
| Twist a tool to an angle (scissors, stapler) | mouse wheel | LB / RB | twist |
| Saw | hold left click + mouse wheel | hold A + LB / RB | twist |
| Bandage | hold left click + circle the mouse (works, 13 Nov) | hold A + circle the right stick | turn round |
| Twist the Nunchuk (chisel, snare) | chisel: wheel; snare: Ctrl + wheel | LB / RB; in endoscopy only while LT (Z) is held, so LB stays the palette otherwise | twist the Nunchuk |
| Strike / swing down | middle click | Y (Triangle) | swing |
| CPR | middle click, in time | Y (Triangle), in time | swing |
| Paddles (push both forward) | hold W + wheel up (W counts this way only during the paddle step) | L3 + R3 together | push both |
| Endoscope forward / back | wheel up / down (or R / V), no A + B, gentler than now | right stick up / down | push / pull |
| Speaker sounds | PC speakers | PC speakers + rumble; DualSense speaker on PS5 | the Remote's speaker |
| Rumble | - | controller rumble | the Remote's rumble |

Changed from before: controller B is only the B button (RT no longer backs out); swing moved from L3 to Y
(Y no longer gives the Wii's 2 button, which the game doesn't use in play). Mouse: after a grab starts,
right click may be let go.

## Status
- Rumble: hooked at WPADControlMotor (0x80253960): real Wii Remote + controller. Untested.
- Real Wii Remote: runtime/wiimote_hid.c. Buttons, accelerometer, IR pointer (roll-corrected; TTPC_WII_GAIN,
  TTPC_WII_FLIPY), Nunchuk stick + accelerometer; looked for every 3 s (pair any time, or a DolphinBar in
  mode 4). Prompts show the original Wii pictures while it is in use. Untested.
- Speaker: the game's 6.67 ms speaker alarm (0x80314B80) is replaced by runtime/speaker.c, which takes the
  40 samples per tick (6000 Hz, scaled by the game's volume) and sends them to: a real Remote (4-bit ADPCM over
  report 0x18, paced 150 packets/s), else the PC speakers (mixed into the game audio) + rumble following the
  loudness, + a DualSense's own speaker when one is connected. TTPC_SPEAKER=0 off, =pc PC only. Untested.
- Inputs implemented (13 Nov): what is being played comes from the files the game loads (disc.c: su/or/em/en, au,
  di). Wheel = 15 degree twist (level again after ~2.5 s untouched with A/B up); in the endoscope wheel / R / V
  drive (no A+B needed, gentler: TTPC_ENDO_SPEED, default 0.5) and Ctrl+wheel twists the snare; W + wheel up =
  paddles (W stops being the stick until let go); a grab holds while left click does. Pad: RT = B only with A or
  LT, LB/RB twist (endoscope: only with LT), Y swing, X = 1, L3+R3 paddles, endoscope on the right stick, both
  sticks move the pointer while A is held.
- Ultrawide: pieces mirrored into the sides must be at least 96 px tall (the pointer was mirrored); repeating
  tiles stay tiles for 60 frames once seen (the title-screen sides flickered).

## Button prompts (runtime: texpack.c prompt sets, platform.c prompts_update)

Settings: **Button prompts** = Automatic (the device used last: a key / click / real mouse move, or a pad button /
stick push) | Wii Remote (original pictures) | Keyboard & mouse | Controller; **Controller style** = Automatic (SDL's
controller type: PS4, PS5, else Xbox) | Xbox | PlayStation 4 | PlayStation 5.

Sets live in `prompts/<keyboard|xbox|ps4|ps5>/` next to the exe as `tt_<id>_<name>.png` (same ids as texture-pack
files); they win over the pack and work with it off. Switching re-looks-up the cached textures only (no re-decode).
`tools/prompts/make_prompts.py` makes them from the player's own extracted 2D art (keeps the picture, redraws the
symbol on each grey plate); the result contains game art, so it is made locally, not shipped.

Done (pass 1, 15 pictures): help bars (CHECK/BACK, BACK, SCROLL/ROTATE, SELECT/BACK, VOICE PLAY), the OK/BACK keys,
the episode-select help (D-pad, A, +/-), the A and A+B speech bubbles (operations).

Next:
1. (Checked: font.brfnt has no button glyphs, so dialogue names buttons in words - a script-text pass, with ttre.)
2. The Wii Remote / Nunchuk motion pictures (sprite/ope/rimokon.tpx, rimo_swing.tpx): show the PC move instead.
3. Tutorial pages (327, tutorial/<mode>/tuto_*.tpx).
