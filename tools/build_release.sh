#!/usr/bin/env bash
# build_release.sh RID [OUTDIR]
#
# Build the redistributable libtispeech and svsay for one runtime identifier:
# osx-arm64, osx-x64, linux-x64, linux-arm64 or win-x64. No DLL is involved, so
# the result contains no SoftVoice data; it reads the tables from the user's
# own DLLs at run time (tispeech_load_dlls, svsay -d).
#
# Linux and Windows targets cross-compile with `zig cc` (glibc 2.28 baseline),
# so every RID builds from any host with zig and cmake; the macOS ones need a
# Mac. Writes OUTDIR (default dist/RID) and dist/tispeech-RID.{tar.gz,zip}.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RID="${1:?usage: build_release.sh RID [OUTDIR]}"
OUT="${2:-$ROOT/dist/$RID}"
BUILD="$ROOT/build-release/$RID"
mkdir -p "$OUT" "$BUILD/tc"

# No debug info, and no build-machine paths in what does remain (__FILE__).
args=(-S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
      "-DCMAKE_C_FLAGS=-g0 -ffile-prefix-map=$ROOT=.")
zigcc() {   # wrapper scripts: CMake rejects a compiler with arguments
    printf '#!/bin/sh\nexec zig cc -target %s "$@"\n' "$1" > "$BUILD/tc/cc"
    printf '#!/bin/sh\nexec zig ar "$@"\n' > "$BUILD/tc/ar"
    printf '#!/bin/sh\nexec zig ranlib "$@"\n' > "$BUILD/tc/ranlib"
    chmod +x "$BUILD/tc/"*
    # Strip at link time: zig's cached libc start files keep debug info that
    # names the directory they were first built in.
    args+=(-DCMAKE_C_COMPILER="$BUILD/tc/cc" -DCMAKE_AR="$BUILD/tc/ar"
           -DCMAKE_RANLIB="$BUILD/tc/ranlib"
           -DCMAKE_EXE_LINKER_FLAGS=-s -DCMAKE_SHARED_LINKER_FLAGS=-s)
}
case "$RID" in
    osx-arm64)   args+=(-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0) ;;
    osx-x64)     args+=(-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15) ;;
    linux-x64)   zigcc x86_64-linux-gnu.2.28
                 args+=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64) ;;
    linux-arm64) zigcc aarch64-linux-gnu.2.28
                 args+=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64) ;;
    win-x64)     zigcc x86_64-windows-gnu
                 args+=(-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=AMD64
                        -DCMAKE_RC_COMPILER=false) ;;
    *) echo "build_release.sh: unknown RID $RID" >&2; exit 2 ;;
esac

cmake "${args[@]}" >/dev/null
cmake --build "$BUILD" --target tispeech_shared svsay -j 8 >/dev/null

rm -rf "${OUT:?}"/*
case "$RID" in
    osx-*)   cp "$BUILD/libtispeech.dylib" "$BUILD/svsay" "$OUT/" ;;
    linux-*) cp "$BUILD/libtispeech.so" "$BUILD/svsay" "$OUT/" ;;
    # libtispeech.dll, never tispeech.dll: on a case-insensitive file system
    # that name is taken by the managed TiSpeech.dll next to it.
    win-*)   dll=$(find "$BUILD" -maxdepth 1 -iname '*tispeech.dll' | head -1)
             cp "$dll" "$OUT/libtispeech.dll"
             cp "$BUILD/svsay.exe" "$OUT/" ;;
esac
mkdir -p "$OUT/include/tispeech"
cp "$ROOT/include/tispeech/capi.h" "$OUT/include/tispeech/"
cp "$ROOT/README.md" "$OUT/"

cd "$(dirname "$OUT")"
name="tispeech-$RID"
case "$RID" in
    win-*) rm -f "$name.zip"; (cd "$OUT" && zip -qr "../$name.zip" .) ;;
    *)     tar -czf "$name.tar.gz" -C "$OUT" . ;;
esac
ls -la "$OUT"
