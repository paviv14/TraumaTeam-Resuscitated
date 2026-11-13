# Controls and button prompts

## Mapping (Wii Remote -> PC)

| Wii              | Keyboard & mouse                  | Xbox            | PlayStation 4 / 5          |
|------------------|-----------------------------------|-----------------|----------------------------|
| Pointer          | mouse                             | right stick (RB re-centres) | right stick (R1 re-centres) |
| A                | left click / Space / Enter        | A               | Cross                      |
| B                | right click / Shift / X           | B or RT         | Circle or R2               |
| A+B (grab)       | left + right click                | A + B (or RT)   | Cross + Circle (or R2)     |
| Z (Nunchuk)      | E / F                             | LT              | L2                         |
| C (Nunchuk)      | Q / C                             | LB              | L1                         |
| Nunchuk stick    | W A S D                           | left stick      | left stick                 |
| D-pad            | arrow keys                        | D-pad           | D-pad                      |
| + / -            | + / - (P / M)                     | Menu / View     | Options / Share (PS5: Create) |
| 1 / 2            | 1 / 2                             | X / Y           | Square / Triangle          |
| Home             | Esc / H                           | Guide           | PS                         |
| Twist (bandage)  | hold A + circle the mouse (J / L) | -               | -                          |
| Swing            | middle click / G                  | left-stick click | L3                        |
| Push / pull      | wheel / R, V                      | right-stick click | R3                       |

Undecided for now (kept as they were): pad B stays on both B and RT; A+B on the mouse stays left + right click.

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
