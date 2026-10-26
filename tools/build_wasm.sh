#!/bin/sh
# Builds the browser version: waemu.js / waemu.wasm / waemu.data in the
# repository root (GitHub Pages serves main:/).  Low priority, like the native
# build.  WASM_OUT=<dir> writes somewhere else.  Winamp itself comes out of
# orig/winamp295.exe (tools/prepare_winamp.py) into build/drive_c.
set -e
cd "$(dirname "$0")/.."
EMSDK="${EMSDK:-/c/prog/emsdk/emsdk}"
EMCC="$EMSDK/upstream/emscripten/em++.exe"
[ -f "$EMCC" ] || EMCC="$EMSDK/upstream/emscripten/em++"
[ -f "$EMCC" ] || { echo "em++ not found under $EMSDK" >&2; exit 1; }
. tools/lowpri.sh
OUT="${WASM_OUT:-.}"
OBJ=build/wasm
mkdir -p "$OBJ" "$OUT" dlls
cp dlls_dist/wine/comctl32.dll dlls/comctl32.dll

FLAGS="-std=c++17 -O2 -fwasm-exceptions -Isrc -Isrc/emu -Isrc/win32 -Isrc/third_party -Isrc/third_party/libtera"
SRC="$(ls src/emu/*.cpp | grep -v '/main\.cpp$') $(ls src/win32/*.cpp) src/host/main_wasm.cpp"

# Compile each file once; only changed ones next time.
JOBS=${JOBS:-$(( $(nproc) > 2 ? $(nproc) - 1 : 1 ))}
objs=""
pids=""
n=0
for f in $SRC; do
    o="$OBJ/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$/.o/')"
    objs="$objs $o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$(find src -name '*.h' -newer "$o" | head -1)" ]; then
        "$EMCC" $FLAGS -c "$f" -o "$o" &
        pids="$pids $!"
        n=$((n + 1))
        if [ $n -ge "$JOBS" ]; then
            for p in $pids; do wait "$p"; done
            pids=""
            n=0
        fi
    fi
done
for p in $pids; do wait "$p"; done
ko="$OBJ/kanji.o"
[ -f "$ko" ] && [ ! src/third_party/libtera/kanji.c -nt "$ko" ] || \
    "$EMSDK/upstream/emscripten/emcc.exe" -O2 -w -c src/third_party/libtera/kanji.c -o "$ko"

# Git Bash rewrites anything that looks like a POSIX path ("/C:/jww" became
# "C;C:"); the emscripten virtual paths must reach emcc as written.
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
# What the guest finds on its C: drive, and the fonts and DLLs beside it.
[ -f build/drive_c/Winamp/winamp.exe ] || python -I tools/prepare_winamp.py orig/winamp295.exe build/drive_c
PRELOAD="--preload-file build/drive_c/Winamp@/C:/Winamp --preload-file samples@/C:/Music"
PRELOAD="$PRELOAD --preload-file fonts@/fonts --preload-file dlls@/dlls"

"$EMCC" -O2 -fwasm-exceptions $objs "$ko" -o "$OUT/waemu.js" \
    -sMODULARIZE=1 -sEXPORT_NAME=createWaemu \
    -sENVIRONMENT=worker,node \
    -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sINITIAL_MEMORY=128MB \
    -sSTACK_SIZE=16MB \
    -sFORCE_FILESYSTEM=1 \
    -sEXPORTED_FUNCTIONS='["_waemu_main"]' \
    -sEXPORTED_RUNTIME_METHODS='["FS","ENV","HEAPU8","HEAP16","HEAP32","ccall"]' \
    $PRELOAD
ls -l "$OUT"/waemu.js "$OUT"/waemu.wasm "$OUT"/waemu.data
