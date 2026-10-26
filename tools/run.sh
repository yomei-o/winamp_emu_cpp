#!/bin/sh
# Runs Winamp headless with tmp/c as its C: drive (C:\Winamp from the
# installer, C:\Music from samples/, as in the browser) and writes the screen
# to tmp/shot.png.  Script lines (see src/host/main_gui.cpp) go in tmp/run.script.
#   sh tools/run.sh [seconds] [file to play, e.g. 'C:\Music\doremi.mp3']
#   WAEMU_TRACE_WAVE=1 sh tools/run.sh ...   (waveOut calls on stderr)
cd "$(dirname "$0")/.."
root=$(cygpath -m "$PWD" 2>/dev/null || pwd)
mkdir -p tmp/c/Music
[ -f tmp/c/Winamp/winamp.exe ] || python -I tools/prepare_winamp.py orig/winamp295.exe tmp/c
cp samples/* tmp/c/Music/
secs=${1:-10}; [ $# -gt 0 ] && shift
script=""; [ -f tmp/run.script ] && script="--script $root/tmp/run.script"
./waemu.exe --headless --drive-c "$root/tmp/c" --time $((secs * 1000)) $script \
    --fonts "$root/fonts" --dlls "$root/dlls" --shot "$root/tmp/shot.png" 'C:\Winamp\winamp.exe' "$@"
