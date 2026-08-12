#!/usr/bin/env bash
set -u
set -o pipefail

ROOT="${1:-$HOME/Documents/Amiga/VICE/vice-3.1}"
FLAGS="-m68060 -mhard-float -Ofast -fomit-frame-pointer -fno-strict-aliasing -noixemul -finline-functions"

fail()
{
    echo "ERROR: $*" >&2
    exit 1
}

export PATH="/opt/amiga/bin:$PATH"
hash -r 2>/dev/null || true

cd "$ROOT" || fail "Cannot enter $ROOT"

grep -q "FRF_HARD_PAUSE_AHI_RELEASE" src/arch/amigaos/ui.c ||
    fail "Hard-pause patch is not present in ui.c"

grep -q "s32 ahi_resume(void)" src/arch/amigaos/ahi.c ||
    fail "AHI resume implementation is not present"

grep -q \
    'ITEMTOGGLE(IDMS_PAUSE,[[:space:]]*"P",[[:space:]]*IDM_PAUSE)' \
    src/arch/amigaos/c64uires.h ||
    fail "Right-Amiga+P is not assigned in c64uires.h"

MUI_ARCHIVE="$(m68k-amigaos-gcc -print-file-name=libmui.a)"
SOCKET_ARCHIVE="$ROOT/src/socketdrv/libsocketdrv.a"
LOG="$ROOT/FRF_060_HARD_PAUSE_AHI_RELEASE_$(date +%Y%m%d_%H%M%S).log"
OUT="$HOME/Documents/Amiga/VICE/x64-FRF-M68060-OFAST-HARD-PAUSE-AHI-RELEASE"

[ "$MUI_ARCHIVE" != "libmui.a" ] && [ -f "$MUI_ARCHIVE" ] ||
    fail "System libmui.a not found"

[ -f "$SOCKET_ARCHIVE" ] ||
    fail "libsocketdrv.a missing"

[ -f src/resid/libresid.a ] ||
    fail "libresid.a missing; ReSID remains retained"

echo "Rebuilding x64 with hard Pause and pause-time AHI release..."
echo "Flags: $FLAGS"
echo

rm -f \
    src/arch/amigaos/ahi.o \
    src/arch/amigaos/ui.o \
    src/arch/amigaos/c64ui.o \
    src/arch/amigaos/libarch.a \
    src/x64

mkdir -p src/arch/amigaos/.deps
: > src/arch/amigaos/.deps/ahi.Po
: > src/arch/amigaos/.deps/ui.Po
: > src/arch/amigaos/.deps/c64ui.Po

make -C src/arch/amigaos ahi.o ui.o c64ui.o \
    CFLAGS="$FLAGS" \
    CXXFLAGS="$FLAGS" \
    OBJCFLAGS="$FLAGS" \
    CCASFLAGS="$FLAGS" \
    V=1 ||
    fail "ahi.o/ui.o/c64ui.o compilation failed"

make -C src/arch/amigaos libarch.a \
    CFLAGS="$FLAGS" \
    CXXFLAGS="$FLAGS" \
    OBJCFLAGS="$FLAGS" \
    CCASFLAGS="$FLAGS" \
    V=1 ||
    fail "libarch.a rebuild failed"

make -C src x64 \
    CFLAGS="$FLAGS" \
    CXXFLAGS="$FLAGS" \
    OBJCFLAGS="$FLAGS" \
    CCASFLAGS="$FLAGS" \
    LIBS="$SOCKET_ARCHIVE -lm $MUI_ARCHIVE -lauto" \
    resid_libs="../src/resid/libresid.a" \
    V=1 2>&1 | tee "$LOG"

STATUS=${PIPESTATUS[0]}
[ "$STATUS" -eq 0 ] ||
    fail "x64 link failed; see $LOG"

[ -s src/x64 ] ||
    fail "src/x64 was not produced"

cp -v src/x64 "$OUT" ||
    fail "Could not save the test executable"

[ -s "$OUT" ] ||
    fail "Saved executable is empty"

echo
echo "Built:"
echo "  $OUT"
echo
echo "Test sequence:"
echo "  1. Start x64 using AHI."
echo "  2. Press Right-Amiga+P."
echo "  3. Confirm emulation freezes exactly and AHI is released."
echo "  4. Press Right-Amiga+P again."
echo "  5. Confirm AHI reopens and emulation continues."
echo
echo "ReSID remains linked and selectable."
echo
echo "Log:"
echo "  $LOG"
