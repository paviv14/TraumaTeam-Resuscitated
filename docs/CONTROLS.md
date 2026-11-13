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
- Speaker: WPADCanSendStreamData 0x80255A70, WPADSendStreamData 0x80255B00 (report 0x18, 4-bit ADPCM),
  driven by the game's speaker module around 0x80313F70-0x80315200 (SpeakerSe.arc). Next: answer them,
  forward to a real Remote, decode to PC audio and the DualSense.
- To do: the per-tool hooks for twist / strike / push / endoscope (EMERGENCY_RE.md has the bandage pattern).

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
