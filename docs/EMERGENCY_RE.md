# Emergency (Maria, First Response) - reverse-engineering notes

* Sound labels for the mode: table at 0x8039A7F8 (EM_BLOOD_DIVIDE ...); the mode's sound id = 0x35 + index
  (played with 0x80026200(id, 0)): EM_VAINSECURING_START 0x84 / GEL_OK 0x85 (fn_80176F50), BANDAGE_GUIDE_ON 0x89
  (fn_80171C30), BANDAGE_ROTATION_START 0x8B (fn_801751A0), BANDAGE_WRAP_END 0x8C (fn_80173280, fn_80173870).
* Bandage wrap calc fn_80173870 (work = R32(arg+0)): the **Nunchuk** accelerometer (fn_8000B7C0, KPAD +0x68) ->
  roll in -1..1 (x * 1.081, clamped, halved; y >= 0 means past 90 degrees: +-1 - v) -> work+0x3BC; bandage angle
  work+0x3C4 = -k * roll (clamped +-359); guide marks (count obj+0xAC, angle +0x9C per 0x58-byte mark, tolerance
  +0x2C) are hit when the angle sweeps over them; progress work+0x3B8 += speed * f (f from 0x804860A0[chan] / 5).
* fn_80174430 is the same roll from the *Remote's* accelerometer (used by the tilt-steered cursor fn_801751A0).
* Remote accessors: 0x8000B670 acc history, 0x8000B710 / 0x8000B720 acc, 0x8000B730 acc speed, 0x8000B7C0 Nunchuk
  acc, 0x8000B880 roll from the horizon (degrees). KPAD samples: 0x80482E70 + chan * 0xB00 (16 x 0xB0).
* PC: runtime/motion.c answers fn_8000B7C0 with gravity turned by the player's twist (hold A + circle the mouse,
  or J / L); `[bandage]` log lines show the twist, the game's roll, the bandage angle and progress.
