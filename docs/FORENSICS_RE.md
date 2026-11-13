# Forensics (autopsy) body examination - reverse-engineering notes

Goal: let the VR layer own the morgue examination (hit test on its own model, then tell the game "this spot")
instead of translating the laser into Wii-pointer clicks on the game's camera view. Done in
`recomp/runtime/forensics.c` (see "VR" below).

## Files (case 110, the first one)
* `autopsy/<lang>/<case>/remain/rem00..03.bf`, `col01.bf`, `col03.bf` - the examination scripts.
* `operation2/hit/HIT07xx.hit` - "MHD2" zone files (shared with the surgery hit code, parser 0x800388B0,
  zone count 0x80038C10(type, hit), zone 0x80038BD0, point count 0x80038C50, point 0x80038CA0).
  Zones of type 4 hold 1-5 points **in the body model's space**: 1 point = a spot (pointer within 10 px),
  2 = a segment, 3+ = a polygon (all tested on the screen after projecting with the game camera).
  HIT0701 has 2 zones; HIT0702-0704 are empty (24 bytes).
* `model/autopsy/kensiNN_body_collision_NN.brres`, `_things_collision_NN.brres` - invisible models, one per
  script procedure of `col*.bf` (the model's index in the descriptor's list = the procedure number).
* Global model table: categories at 0x80376878 (8 bytes each: name table, count); category 7 = autopsy,
  its names at 0x8035EC64 (0 kensi_bg, 1 kensi01_body, 2 kensi01_things, 18 kensi01_body_collision_01, ...).
  0x8002E5E0(category, index) loads one.
* Case table (case 110 at 0x803B1740): examination descriptors at 0x803B17D0 (2 of them, 0x28 bytes):
  `u16 ?, u16 category (7), u16 model, u16 collision models[] (-1 ends), ... +0x18 hit file, +0x1C intro
  script, +0x20 zone script, +0x24 collision script`.
  * body:     model 1, collision 18/19/60, HIT0701.hit, rem00.bf, rem01.bf, col01.bf (lefthand, righthand, eyes)
  * effects:  model 2, collision 62/20/59/61, HIT0702.hit, rem02.bf, rem03.bf, col03.bf (button_sode, syatu_01, waki_01, ...)

## Code (main.dol)
* The examination task (data block 0x80418088, name "autRemainsClothes" at 0x80418220): calc 0x8012E9F0.
  Work (R32(task+0x38)): +0 state, +1 paused state, +2 next state, +5 zone count, +6 zone picked, +9 model
  count, +0xA model picked, +0x18 zoom timer (0..30), +0x2C/+0x30 Nunchuk stick, +0x38/+0x3C pointer,
  +0x44 zoom target, +0x50/+0x5C model position / rotation, +0x80 body model, +0x84[] collision models,
  +0xAC hit file, +0xB0/+0xB4/+0xB8 scripts (intro, zones, collision), +0xBC + 0xA0*i zone i (points at
  +0xC8, projected at +0x128, done flag +0xC4), +0x28BC + i collision model i done.
  States (jump table 0x804181F8):
  * 0 load (0x8012FAF0) -> 2 intro (runs +0xB0) -> 1
  * 1 overview: stick X turns the model, stick Y zooms; the EXIT plate (itfClickPlate 0) -> 9;
    A -> unproject the pointer into +0x44, state 3 -> 4
  * 3 camera move (0x8012F930), then the state in +2
  * 4 close-up: B or a stick tilt > 0.5 -> zoom out (3 -> 1). Zones: 0x8012F570(work, z) (pointer vs the
    projected zone). Models: 0x801301F0(work, i) (ray from the camera through the pointer, nw4r ray test
    0x80017BB0 on the model). A -> zone z: run +0xB4 procedure z+1 (state 5); model i: run +0xB8
    procedure i (state 6); nothing: run +0xB4 procedure 0 (state 7). 5/6/7 wait for the script, then 4.
  * 8 paused, 9 leaving (0x8012FA70)
  * camera (0x8012F2C0): fixed in 1/2/9, between the zoom target +0x44 and +0x74 otherwise.
* Camera objects: 0x80010200(id) (id 1 and 0xF -> camera 1 unless a hook is set); the view matrix (lookAt,
  3x4) is at camera+0, up +0x30, target +0x3C, position +0x48, fovy +0x54. Project a world point:
  0x80011940(out, point, camera); unproject the pointer: 0x80011A40(out, camera, x, y, depth).
  A model's world matrix: 0x800142C0(instance, out).
* 0x80130570 (same file) is a different task: up to 4 groups of 16 2D layout items you grab with A+B,
  move and twist (Wii Remote roll in 30 degree steps) onto a target.
* Other autopsy tasks: autInspection 0x80132800..0x80135000 (uses ./autopsy/hit/%s), autCharnelCalc
  (the morgue room) around 0x8014A380, autReasonQuiz, autHeadRestor, autHintCardSelect.

## VR (recomp/runtime/forensics.c)
* vr.c hands in the tapped spot in the VR model's frame plus that model's model->view matrix
  (`aut_vr_pick`). World = (camera view)^-1 x (model->view).
* While the task's calc runs: the pointer it reads (0x8000B370 / 0x8000B540) is the spot projected by the
  game camera, so the zoom target and the zone test land on it; the collision models are ray-tested along
  the laser itself (falls back to the game's camera ray when the laser goes through none of them).
* vr.c drives the flow from the task's state (`aut_state`): tap in the overview -> A (zoom) -> in the
  close-up A again -> the remark -> B (zoom out). B in the overview presses the EXIT plate. The Nunchuk
  stick only reaches the game from the VR grab, and only in the overview. Walking away no longer exits.
* Log lines `[aut]`: the world spot, the projected pointer, which collision models the laser went through,
  and the body's world origin from the game next to the one VR computed (they should agree).
* TTPC_AUTVR=0 turns it off; TTPC_EXAM_SIZE (m, default 1.5) and TTPC_EXAM_DIST (m, default 1.6) size and
  place the floating model.

## Next
1. Check the `[aut]` log on the PC: the two body origins should match; if not, the VR model frame is not
   the body's (then use 0x800142C0 instead of the camera).
2. The same for the other examination tasks (autInspection: the scene/room search with ./autopsy/hit).
3. Diagnosis via an original RONI-style console; a direct "start at the morgue" from the chapter's flow.
