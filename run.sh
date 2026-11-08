#!/bin/bash
# run.sh <seconds> <frames-dir> <frame-every> <pointer x,y> <input script> [extra env...]
# Starts from a copy of usersave/ (the user's NAND save) and records frames + log.
cd ~/TraumaTeamRE
rm -rf nand "$2" && cp -r usersave nand && mkdir -p "$2"
shift 0
env TTPC_FRAMES="$2" TTPC_FRAME_EVERY="$3" TTPC_POINTER="$4" TTPC_INPUT="$5" TTPC_DISC=/root/disc/DATA "${@:6}" \
    timeout $(( $1 + 120 )) ${TTPC_BIN:-./build-hl/ttpc} ~/tt/orig/main.dol "$1" > "$2.log" 2>&1
python3 - "$2" <<'PY'
import sys, glob
from PIL import Image
fs = sorted(glob.glob(sys.argv[1] + '/*.ppm'))
step = max(1, len(fs) // 24)
sel = fs[::step][:24]
s = Image.new('RGB', (240 * 6, 180 * ((len(sel) + 5) // 6) or 180))
for i, f in enumerate(sel): s.paste(Image.open(f).resize((240, 180)), ((i % 6) * 240, (i // 6) * 180))
s.save(sys.argv[1] + '.png'); print(len(fs), 'frames;', [f[-10:-4] for f in sel])
PY
