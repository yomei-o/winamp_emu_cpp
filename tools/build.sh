#!/bin/sh
# Native build of jwemu.exe, at low priority, in parallel.
set -e
cd "$(dirname "$0")/.."
export PATH=/c/prog/w64devkit/bin:$PATH
JOBS=${JOBS:-$(( $(nproc) > 2 ? $(nproc) - 1 : 1 ))}
. tools/lowpri.sh
make -j"$JOBS" "$@"
# comctl32 runs inside the emulator: Wine's (LGPL, dlls_dist/wine/), the same
# file for the native and the browser builds.
mkdir -p dlls
cp dlls_dist/wine/comctl32.dll dlls/comctl32.dll
