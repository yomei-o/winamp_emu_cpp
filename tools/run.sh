#!/bin/sh
# Runs Jw_win.exe headless with tmp/c as its C: drive (C:\jww is a copy of
# orig/, as in the browser) and writes the screen to tmp/shot.png.
#   sh tools/run.sh [seconds] [extra jwemu options...]
cd "$(dirname "$0")/.."
mkdir -p tmp/c
[ -d tmp/c/jww ] || cp -r orig tmp/c/jww
secs=${1:-15}; [ $# -gt 0 ] && shift
root=$(pwd)
cd tmp/c/jww
JWEMU_REGISTRY="$root/tmp/reg.txt" "$root/jwemu.exe" --headless --drive-c "$root/tmp/c" --time $((secs * 1000)) \
    --fonts "$root/fonts" --dlls "$root/dlls" --shot "$root/tmp/shot.png" "$@" 'C:\jww\Jw_win.exe'
