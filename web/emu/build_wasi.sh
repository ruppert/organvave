#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The instrument in the browser: web/emu/felucca_web.c compiled with wasi-sdk
# (https://github.com/WebAssembly/wasi-sdk, a reactor module with no imports) plus the page.
#   WASI_SDK=/path/to/wasi-sdk web/emu/build_wasi.sh   ->  ${OUT:-docs/emu}/{index.html, worklet.js, felucca.wasm, fonts/}
# Run after one ./build.sh (build/gen: the tables, the fonts, the icons).
set -e
cd "$(dirname "$0")/../.."
: "${WASI_SDK:=}"
# WASI_SDK: a wasi-sdk directory (wasi-sdk-25.0-x86_64-linux); or WASI_SYSROOT: a wasi-libc install (its lib/wasm32-wasi and
# include) built with the system clang (cmake; THREAD_MODEL=single), linked with -nostdlib, no compiler-rt needed.
if [ -z "$WASI_SYSROOT" ]; then : "${WASI_SDK:?set WASI_SDK to a wasi-sdk directory, or WASI_SYSROOT to a wasi-libc install}"; fi
OUT=${OUT:-docs/emu}
[ -f build/gen/felucca_tables.h ] || { echo "emu: run ./build.sh once first (build/gen)"; exit 1; }
VERSION=$(sed -n 's/^#define ORGANVAVE_VERSION "\([^"]*\)".*/\1/p' firmware/src/core.h | head -n 1)
mkdir -p "$OUT/fonts"
if [ -n "$WASI_SYSROOT" ]; then
    CLANG=${CLANG:-clang}
    OBJ=$(mktemp)
    "$CLANG" --sysroot="$WASI_SYSROOT" --target=wasm32-wasi ${EMU_OPT:--O2} -c -ffp-contract=off -std=gnu99 -w \
        "-DORGANVAVE_VERSION=\"$VERSION\"" -Ibuild/gen -Ifirmware/src -Iweb/emu/wasi_shim web/emu/felucca_web.c -o "$OBJ.o"
    "$CLANG" --target=wasm32-wasi -nostdlib -Wl,--export-dynamic -Wl,-z,stack-size=1048576 -Wl,--initial-memory=33554432 \
        -Wl,--no-entry -o "$OUT/felucca.wasm" "$WASI_SYSROOT/lib/wasm32-wasi/crt1-reactor.o" "$OBJ.o" \
        -L"$WASI_SYSROOT/lib/wasm32-wasi" -lc -lm
    rm -f "$OBJ.o" "$OBJ"
else
"$WASI_SDK/bin/clang" --sysroot="$WASI_SDK/share/wasi-sysroot" --target=wasm32-wasi -mexec-model=reactor \
    ${EMU_OPT:--O2} -ffp-contract=off -std=gnu99 -w "-DORGANVAVE_VERSION=\"$VERSION\"" \
    -Ibuild/gen -Ifirmware/src -Iweb/emu/wasi_shim \
    -Wl,--export-dynamic -Wl,-z,stack-size=1048576 -Wl,--initial-memory=33554432 \
    web/emu/felucca_web.c -o "$OUT/felucca.wasm" -lm
fi
sed -e "s/@VERSION@/organvave $VERSION/g" -e "s/@PREVIEW@//g" web/emu/index.html > "$OUT/index.html"
cp web/emu/worklet.js "$OUT/"
cp web/emu/fonts/DotGothic16-subset.woff web/emu/fonts/OFL.txt "$OUT/fonts/"
echo "emu: $OUT ($(wc -c < "$OUT/felucca.wasm" | tr -d ' ') B wasm, $VERSION)"
