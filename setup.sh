#!/usr/bin/env bash
# Trauma Team Resuscitated - Setup for Linux
# Builds the game for this PC from YOUR OWN copy of Trauma Team (USA), then (optionally) makes the button-prompt
# pictures from it. Nothing of the game is downloaded: only free tools (Python via uv, DolphinTool if you give a
# disc image). Everything downloaded stays in tools-cache/.
#
#   ./setup.sh                          looks for an extracted disc in ./disc, ../disc or the one used last time
#   ./setup.sh /path/to/TraumaTeam.rvz  a disc image (.iso .rvz .wbfs .wia .gcz) or an extracted disc folder
#   options: --disc-jp <image or folder>   Japanese voices from HOSPITAL. 6-nin no Ishi (Japan)
#
# Needs: gcc, make, pkg-config and the SDL2 development files, e.g.
#   Debian / Ubuntu / Mint:  sudo apt install build-essential pkg-config libsdl2-dev curl
#   Fedora:                  sudo dnf install gcc make pkgconf-pkg-config SDL2-devel curl
#   Arch / SteamOS dev:      sudo pacman -S base-devel sdl2 curl
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE="$ROOT/tools-cache"; BIN="$ROOT/bin"
DOL_SHA1=420143d75336a5f2bc14f9dfbe8a9ec56efbe8c8   # Trauma Team (USA) SUOE01, sys/main.dol
JP_SHA1=c33b2f40f504f02ff8c3c7cd4d95e4ab0c6fd851    # HOSPITAL. 6-nin no Ishi (Japan), sys/main.dol
mkdir -p "$CACHE" "$BIN"
exec > >(tee "$CACHE/setup.log") 2>&1

step() { printf '\n\033[36m== %s\033[0m\n' "$1"; }
fail() { printf '\n\033[31mERROR: %s\033[0m\n' "$1"; exit 1; }
sha1() { sha1sum "$1" | cut -d' ' -f1; }

DISC=""; DISC_JP=""; TUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --disc-jp) DISC_JP="$2"; shift 2 ;;
        --tutorials) shift 2 ;;   # no longer used
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) DISC="$1"; shift ;;
    esac
done

echo "Trauma Team Resuscitated - Setup"
echo "An unofficial fan project, free and not affiliated with Atlus, Sega or Nintendo."
echo "It builds the game from your own copy; nothing here contains the game."

# ---------------------------------------------------------------- 1. your disc
step "1/6 Finding your disc"
disc_data() {   # the folder holding sys/main.dol (an extracted disc, with or without DATA/)
    [ -n "${1:-}" ] || return 1
    if [ -f "$1/sys/main.dol" ]; then (cd "$1" && pwd); return 0; fi
    if [ -f "$1/DATA/sys/main.dol" ]; then (cd "$1/DATA" && pwd); return 0; fi
    return 1
}
dolphin_tool() {   # DolphinTool (Dolphin's command-line tool): yours if installed, otherwise a copy is downloaded
    for c in dolphin-tool DolphinTool; do command -v "$c" >/dev/null && { command -v "$c"; return; }; done
    local d="$CACHE/dolphintool"
    if [ ! -x "$d/package/dist/dolphin-tool" ]; then
        echo "   downloading DolphinTool" >&2
        local url; url=$(curl -fsSL https://registry.npmjs.org/@emmercm/dolphin-tool-linux-x64/latest | sed -n 's/.*"tarball":"\([^"]*\)".*/\1/p')
        [ -n "$url" ] || fail "Could not get DolphinTool. Extract the disc with Dolphin instead (Properties > Filesystem > Extract Entire Disc)."
        mkdir -p "$d"; curl -fsSL "$url" | tar xz -C "$d"
    fi
    echo "$d/package/dist/dolphin-tool"
}
extract_image() {   # image, output folder -> the folder holding sys/main.dol
    local dt; dt=$(dolphin_tool)
    echo "   unpacking $(basename "$1") (a few minutes)..." >&2
    "$dt" extract -i "$1" -o "$2" --gameonly --quiet >&2 || true
    local dol; dol=$(find "$2" -path '*/sys/main.dol' 2>/dev/null | head -1)
    [ -n "$dol" ] && dirname "$(dirname "$dol")"
}
DATA=""
if [ -n "$DISC" ] && [ -f "$DISC" ]; then
    OUT="$ROOT/disc"
    DATA=$(disc_data "$OUT" || true)
    [ -n "$DATA" ] || DATA=$(extract_image "$DISC" "$OUT" || true)
    [ -n "$DATA" ] || fail "DolphinTool could not unpack that disc image. Is it a Wii disc image (.iso, .rvz, .wbfs, .wia, .gcz)?"
    if [ "$(sha1 "$DATA/sys/main.dol")" != "$DOL_SHA1" ]; then rm -rf "$OUT"; fail "That disc image is not Trauma Team (USA) (SUOE01). Only the USA version is supported."; fi
elif [ -n "$DISC" ]; then
    DATA=$(disc_data "$DISC" || true)
fi
[ -n "$DATA" ] || { [ -f "$BIN/disc.txt" ] && DATA=$(disc_data "$(head -1 "$BIN/disc.txt")" || true); }
for c in "$ROOT/disc" "$ROOT/../disc" "$ROOT/../../disc"; do [ -n "$DATA" ] || DATA=$(disc_data "$c" || true); done
[ -n "$DATA" ] || fail "No disc found. Run ./setup.sh /path/to/your/disc-image (or the folder you extracted it to with Dolphin)."
[ "$(sha1 "$DATA/sys/main.dol")" = "$DOL_SHA1" ] || fail "That disc's main.dol is not Trauma Team (USA) (SUOE01). Only the USA version is supported."
echo "   $DATA  (Trauma Team USA - OK)"

# ---------------------------------------------------------------- 2. tools
step "2/6 Checking the tools"
miss=""
for t in gcc make pkg-config curl; do command -v $t >/dev/null || miss="$miss $t"; done
pkg-config --exists sdl2 2>/dev/null || miss="$miss SDL2-development-files"
[ -z "$miss" ] || fail "Missing:$miss. Install them (see the top of this script for your distribution) and run it again."
echo "   gcc $(gcc -dumpversion), SDL $(pkg-config --modversion sdl2)"
UV="$CACHE/uv/uv"
if [ ! -x "$UV" ]; then
    echo "   downloading uv (Python)"
    mkdir -p "$CACHE/uv"
    curl -fsSL https://github.com/astral-sh/uv/releases/latest/download/uv-x86_64-unknown-linux-gnu.tar.gz | tar xz -C "$CACHE/uv" --strip-components=1
fi
export UV_CACHE_DIR="$CACHE/uv-cache" UV_PYTHON_INSTALL_DIR="$CACHE/python"

# ---------------------------------------------------------------- 3. recompile
step "3/6 Recompiling the game from your disc (a few minutes)"
(cd "$ROOT" && "$UV" run --no-project --python 3.12 python -m recomp.lift "$DATA/sys/main.dol" data/symbols.txt generated) || fail "Translating main.dol failed."
(cd "$ROOT/recomp" && make -f build.mk TARGET=linux VR=0 BUILD=../build-linux EXE=../build-linux/Resuscitated -j"$(nproc)") || fail "Building failed - see the messages above."

step "4/6 Putting the game together in the bin folder"
cp -f "$ROOT/build-linux/Resuscitated" "$BIN/"
strip "$BIN/Resuscitated" 2>/dev/null || true
[ -d "$BIN/nand" ] || cp -r "$ROOT/nand" "$BIN/"
echo "$DATA" > "$BIN/disc.txt"

if [ -n "$DISC_JP" ]; then   # optional: Japanese voices (only the disc's files/sound/jpn folder is kept, in disc_jp)
    echo "   Japanese voices: $DISC_JP"
    JP=$(disc_data "$DISC_JP" || true)
    if [ -n "$JP" ] && [ "$(sha1 "$JP/sys/main.dol")" != "$JP_SHA1" ]; then JP=""; fi
    if [ -z "$JP" ] && [ -f "$DISC_JP/files/sound/jpn/sys/00000000/static.brsar" ]; then JP="$(cd "$DISC_JP" && pwd)"; fi
    if [ -z "$JP" ] && [ -f "$DISC_JP" ]; then
        TMP="$CACHE/disc_jp_unpack"; rm -rf "$TMP"
        J=$(extract_image "$DISC_JP" "$TMP" || true)
        if [ -n "$J" ] && [ "$(sha1 "$J/sys/main.dol")" = "$JP_SHA1" ]; then
            DST="$ROOT/disc_jp/DATA/files/sound"; mkdir -p "$DST"; rm -rf "$DST/jpn"
            mv "$J/files/sound/jpn" "$DST/"; JP="$ROOT/disc_jp/DATA"
        fi
        rm -rf "$TMP"
    fi
    if [ -n "$JP" ] && [ -f "$JP/files/sound/jpn/sys/00000000/static.brsar" ]; then
        echo "$JP" > "$BIN/disc_jp.txt"; echo "   Japanese voices ready - pick them in the game: F1 > Language > Voices."
    else echo "   That is not HOSPITAL. 6-nin no Ishi (Japan) - skipping the Japanese voices."; fi
fi

# ---------------------------------------------------------------- 5. button prompts
step "5/6 Making the button prompts from your disc"
echo "   The help bubbles and the buttons in the tutorial pictures will show your own keys or controller buttons."
WITH=(--with pillow --with numpy --with scipy --with opencv-python-headless); EXTRA=()
(cd "$ROOT" && "$UV" run --no-project --python 3.12 "${WITH[@]}" python tools/prompts/setup_prompts.py "$DATA" "$BIN/prompts" "$CACHE/prompts-work" "${EXTRA[@]}") \
    || echo "   The button prompts could not be made; the game still works with the original Wii pictures."

step "6/6 Done"
cat > "$ROOT/Trauma Team Resuscitated.sh" <<'SH'
#!/usr/bin/env bash
# Starts the game from its own folder; the log goes to bin/ttpc.log (the previous one: ttpc.prev.log).
cd "$(dirname "$(readlink -f "$0")")/bin" || exit 1
[ -f ttpc.log ] && mv -f ttpc.log ttpc.prev.log
exec ./Resuscitated "$@" 2> ttpc.log
SH
chmod +x "$ROOT/Trauma Team Resuscitated.sh"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
if mkdir -p "$APPS" 2>/dev/null; then
    cat > "$APPS/trauma-team-resuscitated.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Trauma Team Resuscitated
Comment=Trauma Team, recompiled for PC
Exec="$ROOT/Trauma Team Resuscitated.sh"
Icon=$ROOT/recomp/res/resuscitated_icon.png
Categories=Game;
Terminal=false
DESK
fi
echo "   Start the game with \"Trauma Team Resuscitated.sh\" here, or from your applications menu."
echo "   F1 (or Options + Share on a controller): settings and controls."
