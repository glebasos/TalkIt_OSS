#!/usr/bin/env bash
# build_release.sh RID [OUTDIR]
#
# Build the redistributable libtispeech and svsay for one runtime identifier:
# osx-arm64, osx-x64, linux-x64, linux-arm64, win-x64, android-arm64 or
# android-x64. Speech data is compiled from data/; no original DLL or
# extraction step is needed.
#
# Linux and Windows targets cross-compile with `zig cc` (glibc 2.28 baseline),
# so every RID builds from any host with zig and cmake; the macOS ones need a
# Mac. Writes OUTDIR (default dist/RID) and dist/tispeech-RID.{tar.gz,zip}.
#
# Android needs no NDK: the library imports nothing but plain libc (malloc,
# string and stdio calls, opendir/readdir), whose ABI musl and bionic share,
# and zig's musl target links it against an unversioned "libc.so", which is
# bionic's name. Only the library is built there; svsay would need bionic's
# dynamic linker. Clang turns memcmp()==0 into bcmp(), which 64-bit bionic does
# not export, so that rewrite is disabled; the import check after the build
# catches anything else bionic lacks.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RID="${1:?usage: build_release.sh RID [OUTDIR]}"
OUT="${2:-$ROOT/dist/$RID}"
BUILD="$ROOT/build-release/$RID"
mkdir -p "$OUT" "$BUILD/tc"

# No debug info, and no build-machine paths in what does remain (__FILE__).
args=(-S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
      -DTISPEECH_USE_EXTRACTED_DATA=ON
      -DTISPEECH_BASE_DLL= -DTISPEECH_ENG_DLL= -DTISPEECH_SPAN_DLL=
      -DTISPEECH_VERIFY_ORIGINAL=OFF
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
    android-arm64) zigcc aarch64-linux-musl
                 args+=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
                        "-DCMAKE_C_FLAGS=-g0 -ffile-prefix-map=$ROOT=. -fno-builtin-bcmp"
                        "-DCMAKE_SHARED_LINKER_FLAGS=-s -Wl,-z,max-page-size=16384") ;;
    android-x64) zigcc x86_64-linux-musl
                 args+=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64
                        "-DCMAKE_C_FLAGS=-g0 -ffile-prefix-map=$ROOT=. -fno-builtin-bcmp"
                        "-DCMAKE_SHARED_LINKER_FLAGS=-s -Wl,-z,max-page-size=16384") ;;
    win-x64)     zigcc x86_64-windows-gnu
                 args+=(-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=AMD64
                        -DCMAKE_RC_COMPILER=false) ;;
    *) echo "build_release.sh: unknown RID $RID" >&2; exit 2 ;;
esac

cmake "${args[@]}" >/dev/null
targets=(tispeech_shared svsay)
case "$RID" in android-*) targets=(tispeech_shared) ;; esac
cmake --build "$BUILD" --target "${targets[@]}" -j 8 >/dev/null

# Android loads with BIND_NOW: one import bionic lacks and dlopen fails.
case "$RID" in android-*)
    bionic="calloc closedir fclose fopen fread free fseek ftell malloc memchr memcmp
            memcpy memmove memset opendir readdir realloc strcasecmp strcat strchr
            strcmp strcpy strcspn strlen strncat strncmp strncpy strrchr strspn strstr"
    for sym in $(nm -D --undefined-only "$BUILD/libtispeech.so" | awk '{print $NF}'); do
        case " $(echo $bionic) " in *" $sym "*) ;;
            *) echo "build_release.sh: $RID libtispeech.so imports $sym, which bionic may not export" >&2; exit 1 ;;
        esac
    done ;;
esac

rm -rf "${OUT:?}"/*
case "$RID" in
    osx-*)   cp "$BUILD/libtispeech.dylib" "$BUILD/svsay" "$OUT/" ;;
    linux-*) cp "$BUILD/libtispeech.so" "$BUILD/svsay" "$OUT/" ;;
    android-*) cp "$BUILD/libtispeech.so" "$OUT/" ;;
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
