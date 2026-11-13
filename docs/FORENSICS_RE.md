# Forensics (autopsy) body examination - reverse-engineering notes

Goal: let the VR layer own the morgue examination (hit test in 3D, then tell the game "this zone")
instead of translating the laser into Wii-pointer clicks on the game's own camera view.

## Files (case 110, the first one)
* `autopsy/<lang>/<case>/remain/rem00..03.bf`, `col01.bf`, `col03.bf` - the examination scripts.
  `col*.bf` have one procedure per zone (eyes, hands, shirt, buttons, ...), `rem*.bf` drive the stages.
* `operation2/hit/HIT07xx.hit` - the zone shapes (format "MHD2", shared with the surgery hit code):
  u32 version 1, "MHD2", ..., u32 count at 0x14; per zone: a 3x4 matrix (rotation + translation, cm),
  a few floats (centre/extent?), u32 n, then n points (xyz) of an outline. HIT0701 has 2 zones.
* `model/autopsy/kensiNN_body_collision_NN.brres`, `_things_collision_NN.brres` - invisible models,
  one per zone group (no names inside: the pairing is by index).
* Case descriptor tables in main.dol around 0x803A4700 (case 110: 0x803B1740..0x803B1860) list the
  hit files with the script each pairs with (e.g. HIT0701.hit + col01.bf, HIT0702.hit + rem02.bf).

## Code (main.dol)
* Body-examination module: 0x8012E490..0x80132350 (task name "autRemainsClothes" at 0x80418220,
  which is also the base of its data block: "./operation2/hit/%s" at 0x80418234, "remain/%s" etc.).
  * 0x8012E9F0 - set-up: loads the hit file ("./operation2/hit/%s") and the scripts.
  * 0x80130570 - state machine (jump table): states run rem00..03 (script handles at obj+56..+72)
    through 0x8002A500.
  * 0x8012FAF0 / 0x80130AF0 - load col/rem scripts (0x8020B384 sprintf -> 0x80076CF0 path ->
    0x80028A60 load) and register per-zone names with 0x8002E0A0(name, 2, handler 0x801302F0, &slot):
    names come from sprintf(obj+532 format, table obj+352[i]); slots at obj+10456 + 8*i.
    0x801302F0 turned out to be a completion callback (stores two handles into the slot), so these
    registrations look like resource-load requests, not the zone hooks.
  * 0x8012F2C0 - per-frame update (called with a counter in r4 that runs 0..15 during the zoom).
* Script VM entry points used: 0x80028A60 load, 0x80028AD0, 0x8002A260, 0x8002A500 run,
  0x8002E0A0 register named hook, 0x8002E5E0, 0x8002E600 (per step, very frequent).

## Tools
* `TTPC_AUTTRACE=1` (+ lift with RECOMP_LOG for 0x8012E490..0x80132350 and the VM entries) logs the
  module's calls when their arguments change (hle.c rt_logfn).
* `VR Desktop - Start in Morgue.bat`: replays recordings/morgue_start.txt at TTPC_SPEED=4 and hands
  over when the VR layer reports the morgue (TTPC_REPLAY_UNTIL=morgue).

## Next
1. Find the per-frame pointer hit test and the zone -> procedure dispatch: candidates are the calls at the
   end of the state machine's state 0 (0x80131040, 0x80131A00, 0x801313D0 - which steps the script VM -,
   0x80131EC0, 0x80131DF0) and 0x80131730 / 0x80131800.
2. Decode MHD2 fully; check zone outlines against the kensi01 body in 3D.
3. VR: own hit test on the zones -> call the handler path for that zone; drop the pointer/zoom path.
