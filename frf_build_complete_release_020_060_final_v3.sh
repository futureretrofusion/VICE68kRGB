#!/usr/bin/env bash
set -u
set -o pipefail

ROOT="${1:-$HOME/Documents/Amiga/VICE/vice-3.1}"
RELEASE_ROOT="${2:-$HOME/Documents/Amiga/VICE/VICE-3.1-FRF-AmigaOS-PiStorm-2026-06-29-FINAL}"

PROGRAMS="x64 x64sc x64dtv xscpu64 x128 xvic xpet xplus4 xcbm2 xcbm5x0 vsid"
FLAGS_020="-m68020 -m68881 -Ofast -fomit-frame-pointer -fno-strict-aliasing -noixemul -finline-functions"
FLAGS_060="-m68060 -mhard-float -Ofast -fomit-frame-pointer -fno-strict-aliasing -noixemul -finline-functions"
FRF_RELEASE_BUILDER_VERSION="3.0-ahi-timing-startup-final"

fail()
{
    echo "ERROR: $*" >&2
    exit 1
}

export PATH="/opt/amiga/bin:$PATH"
hash -r 2>/dev/null || true

command -v m68k-amigaos-gcc >/dev/null 2>&1 ||
    fail "m68k-amigaos-gcc was not found"

cd "$ROOT" || fail "Cannot enter $ROOT"

for f in \
    src/Makefile \
    src/arch/amigaos/c64uires.h \
    src/arch/amigaos/timer.c \
    src/sounddrv/soundahi.c \
    RELEASE_NOTES_FRF.txt \
    FRF_CONTRIBUTIONS.txt \
    README_FRF_RELEASE.txt \
    RUNTIME_TEST_CHECKLIST.txt
do
    [ -f "$f" ] || fail "Missing $ROOT/$f"
done

if grep -RIn --include='*uires.h' \
    'ITEMTOGGLE(IDMS_GRAB_MOUSE,[[:space:]]*"Q",[[:space:]]*IDM_MOUSE)' \
    src/arch/amigaos >/dev/null 2>&1
then
    fail "Right-Amiga+Q is still assigned to Grab Mouse"
fi

grep -q "FRF_HARD_PAUSE_AHI_RELEASE" src/arch/amigaos/ui.c ||
    fail "Final hard-pause patch is missing from ui.c"

grep -q "s32 ahi_resume(void)" src/arch/amigaos/ahi.c ||
    fail "Pause-time AHI reopen support is missing"

grep -q "FRF_AHI_TIMER_SUBTRACT_FIX" src/arch/amigaos/timer.c ||
    fail "The final AHI timer subtraction fix is missing"

grep -q "microseconds = dt->tv_usec - st->tv_usec;" \
    src/arch/amigaos/timer.c ||
    fail "timer_subtime() does not contain the corrected subtraction"

if grep -Fq 'dt->tv_usec * (extrasub ==1 ) ? 10 : 1' \
    src/arch/amigaos/timer.c
then
    fail "The original malformed timer expression is still present"
fi

grep -q "FRF_AHI_BACKEND_RESUME_FIX" src/sounddrv/soundahi.c ||
    fail "The final AHI backend resume fix is missing"

grep -q "return ahi_resume();" src/sounddrv/soundahi.c ||
    fail "_ahi_resume() is not connected to ahi_resume()"

python3 - <<'PY_NOOP'
from pathlib import Path
import re
text = Path("src/sounddrv/soundahi.c").read_text(errors="replace")
bad = re.search(
    r'static\s+int\s+_ahi_resume\s*\(\s*void\s*\)\s*'
    r'\{\s*return\s+0\s*;\s*\}',
    text,
    re.S,
)
raise SystemExit(1 if bad else 0)
PY_NOOP
[ "$?" -eq 0 ] ||
    fail "The old no-op _ahi_resume() is still present"

for f in \
    c64uires.h c64dtvuires.h c128uires.h vic20uires.h petuires.h \
    plus4uires.h cbm2uires.h cbm5x0uires.h scpu64uires.h
do
    grep -q \
        'ITEMTOGGLE(IDMS_PAUSE,[[:space:]]*"P",[[:space:]]*IDM_PAUSE)' \
        "src/arch/amigaos/$f" ||
        fail "Right-Amiga+P is missing from $f"
done

STAMP="$(date +%Y%m%d_%H%M%S)"
LOGDIR="$ROOT/FRF_RELEASE_LOGS_$STAMP"
MAKEFILE_BACKUP="$ROOT/src/Makefile.FRF_RELEASE_BACKUP_$STAMP"

rm -rf "$RELEASE_ROOT"
mkdir -p "$LOGDIR" "$RELEASE_ROOT"
cp src/Makefile "$MAKEFILE_BACKUP" || fail "Could not back up src/Makefile"

restore_makefile()
{
    cp "$MAKEFILE_BACKUP" "$ROOT/src/Makefile" 2>/dev/null || true
}
trap restore_makefile EXIT HUP INT TERM

python3 - <<'PY'
from pathlib import Path

p = Path("src/Makefile")
lines = p.read_text(errors="replace").splitlines(True)
programs = (
    "x64$(EXEEXT) x64sc$(EXEEXT) x64dtv$(EXEEXT) "
    "xscpu64$(EXEEXT) x128$(EXEEXT) xvic$(EXEEXT) "
    "xpet$(EXEEXT) xplus4$(EXEEXT) xcbm2$(EXEEXT) "
    "xcbm5x0$(EXEEXT) vsid$(EXEEXT)"
)

out = []
i = 0
changed = False

while i < len(lines):
    line = lines[i]
    if not changed and line.startswith("bin_PROGRAMS ="):
        out.append("bin_PROGRAMS = " + programs + "\n")
        changed = True
        while line.rstrip("\n").endswith("\\"):
            i += 1
            line = lines[i]
        i += 1
        continue
    out.append(line)
    i += 1

if not changed:
    raise SystemExit("Could not locate bin_PROGRAMS in src/Makefile")

p.write_text("".join(out))
PY

[ "$?" -eq 0 ] || fail "Could not enable all emulator targets"

MUI_ARCHIVE="$(m68k-amigaos-gcc -print-file-name=libmui.a)"
[ "$MUI_ARCHIVE" != "libmui.a" ] && [ -f "$MUI_ARCHIVE" ] ||
    fail "System libmui.a was not found"

cp RELEASE_NOTES_FRF.txt "$RELEASE_ROOT/RELEASE_NOTES.txt"
cp FRF_CONTRIBUTIONS.txt "$RELEASE_ROOT/FRF_CONTRIBUTIONS.txt"
cp README_FRF_RELEASE.txt "$RELEASE_ROOT/README_FIRST.txt"
cp RUNTIME_TEST_CHECKLIST.txt "$RELEASE_ROOT/RUNTIME_TEST_CHECKLIST.txt"

clean_profile()
{
    find src -xdev -maxdepth 9 -type f \
        \( -name '*.o' -o -name '*.a' -o -name '*.lo' -o -name '*.Tpo' \) \
        -delete

    for p in $PROGRAMS; do
        rm -f "src/$p"
    done

    find src -xdev -maxdepth 9 -type f \
        \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.S' \) \
        -print0 |
    while IFS= read -r -d '' f; do
        d="$(dirname "$f")"
        b="$(basename "$f")"
        b="${b%.*}"
        mkdir -p "$d/.deps"
        : > "$d/.deps/$b.Po"
    done

    mkdir -p src/src/arch/amigaos
}

build_profile()
{
    NAME="$1"
    FLAGS="$2"
    OUTDIR="$3"
    LOG="$LOGDIR/$NAME.log"

    echo
    echo "============================================================"
    echo "Building $NAME"
    echo "Release builder: $FRF_RELEASE_BUILDER_VERSION"
    echo "Flags: $FLAGS"
    echo "============================================================"

    clean_profile

    SOCKET_ARCHIVE="$ROOT/src/socketdrv/libsocketdrv.a"
    SOUND_ARCHIVE="$ROOT/src/sounddrv/libsounddrv.a"
    ARCH_MUI_ARCHIVE="$ROOT/src/arch/amigaos/mui/libarchmui.a"
    RESID_ARCHIVE="$ROOT/src/resid/libresid.a"
    RESID_DTV_ARCHIVE="$ROOT/src/resid-dtv/libresiddtv.a"

    echo
    echo "Building mandatory archives removed by the clean step..."

    make -C src/arch/amigaos/mui \
        libarchmui.a \
        CFLAGS="$FLAGS" \
        CXXFLAGS="$FLAGS" \
        OBJCFLAGS="$FLAGS" \
        CCASFLAGS="$FLAGS" \
        V=1 2>&1 | tee -a "$LOG"
    STATUS=${PIPESTATUS[0]}
    [ "$STATUS" -eq 0 ] ||
        fail "$NAME failed building libarchmui.a; see $LOG"

    make -C src/sounddrv \
        libsounddrv.a \
        CFLAGS="$FLAGS" \
        CXXFLAGS="$FLAGS" \
        OBJCFLAGS="$FLAGS" \
        CCASFLAGS="$FLAGS" \
        V=1 2>&1 | tee -a "$LOG"
    STATUS=${PIPESTATUS[0]}
    [ "$STATUS" -eq 0 ] ||
        fail "$NAME failed building libsounddrv.a; see $LOG"

    make -C src/socketdrv \
        libsocketdrv.a \
        CFLAGS="$FLAGS" \
        CXXFLAGS="$FLAGS" \
        OBJCFLAGS="$FLAGS" \
        CCASFLAGS="$FLAGS" \
        V=1 2>&1 | tee -a "$LOG"
    STATUS=${PIPESTATUS[0]}
    [ "$STATUS" -eq 0 ] ||
        fail "$NAME failed building libsocketdrv.a; see $LOG"

    make -C src/resid \
        libresid.a \
        CFLAGS="$FLAGS" \
        CXXFLAGS="$FLAGS" \
        OBJCFLAGS="$FLAGS" \
        CCASFLAGS="$FLAGS" \
        V=1 2>&1 | tee -a "$LOG"
    STATUS=${PIPESTATUS[0]}
    [ "$STATUS" -eq 0 ] ||
        fail "$NAME failed building libresid.a; see $LOG"

    make -C src/resid-dtv \
        libresiddtv.a \
        CFLAGS="$FLAGS" \
        CXXFLAGS="$FLAGS" \
        OBJCFLAGS="$FLAGS" \
        CCASFLAGS="$FLAGS" \
        V=1 2>&1 | tee -a "$LOG"
    STATUS=${PIPESTATUS[0]}
    [ "$STATUS" -eq 0 ] ||
        fail "$NAME failed building libresiddtv.a; see $LOG"

    [ -s "$ARCH_MUI_ARCHIVE" ] ||
        fail "$NAME did not produce libarchmui.a"
    [ -s "$SOCKET_ARCHIVE" ] ||
        fail "$NAME did not produce libsocketdrv.a"
    [ -s "$SOUND_ARCHIVE" ] ||
        fail "$NAME did not produce libsounddrv.a"
    [ -s "$RESID_ARCHIVE" ] ||
        fail "$NAME did not produce libresid.a"
    [ -s "$RESID_DTV_ARCHIVE" ] ||
        fail "$NAME did not produce libresiddtv.a"

    echo
    echo "Building the complete emulator suite..."

    make -C src all-recursive \
        CFLAGS="$FLAGS" \
        CXXFLAGS="$FLAGS" \
        OBJCFLAGS="$FLAGS" \
        CCASFLAGS="$FLAGS" \
        LIBS="$SOCKET_ARCHIVE -lm $MUI_ARCHIVE -lauto" \
        resid_libs="../src/resid/libresid.a" \
        resid_dtv_libs="../src/resid-dtv/libresiddtv.a" \
        V=1 2>&1 | tee -a "$LOG"

    STATUS=${PIPESTATUS[0]}
    [ "$STATUS" -eq 0 ] ||
        fail "$NAME build failed; see $LOG"

    mkdir -p "$OUTDIR"
    MISSING=""

    for p in $PROGRAMS; do
        if [ ! -s "src/$p" ]; then
            make -C src "$p" \
                CFLAGS="$FLAGS" \
                CXXFLAGS="$FLAGS" \
                OBJCFLAGS="$FLAGS" \
                CCASFLAGS="$FLAGS" \
                LIBS="$SOCKET_ARCHIVE -lm $MUI_ARCHIVE -lauto" \
                resid_libs="../src/resid/libresid.a" \
                resid_dtv_libs="../src/resid-dtv/libresiddtv.a" \
                V=1 2>&1 | tee -a "$LOG"
        fi

        if [ -s "src/$p" ]; then
            cp -v "src/$p" "$OUTDIR/$p"
        else
            MISSING="$MISSING $p"
        fi
    done

    [ -z "$MISSING" ] || fail "$NAME missing:$MISSING"

    {
        echo "VICE 3.1 FRF $NAME"
        echo "Built: $(date -Is)"
        echo "Compiler: $(m68k-amigaos-gcc --version | head -1)"
        echo "Flags: $FLAGS"
        echo "FRF timer fix: FRF_AHI_TIMER_SUBTRACT_FIX"
        echo "FRF AHI resume fix: FRF_AHI_BACKEND_RESUME_FIX"
        echo "FRF hard pause: FRF_HARD_PAUSE_AHI_RELEASE"
        echo
        echo "Programs:"
        for p in $PROGRAMS; do
            printf "  %-8s " "$p"
            stat -c '%s bytes' "$OUTDIR/$p"
        done
    } > "$OUTDIR/BUILD_INFO.txt"

    cp "$RELEASE_ROOT/RELEASE_NOTES.txt" "$OUTDIR/"
    cp "$RELEASE_ROOT/FRF_CONTRIBUTIONS.txt" "$OUTDIR/"
    cp "$RELEASE_ROOT/README_FIRST.txt" "$OUTDIR/"
    cp "$RELEASE_ROOT/RUNTIME_TEST_CHECKLIST.txt" "$OUTDIR/"

    (
        cd "$OUTDIR" || exit 1
        sha256sum $PROGRAMS BUILD_INFO.txt RELEASE_NOTES.txt \
            FRF_CONTRIBUTIONS.txt README_FIRST.txt \
            RUNTIME_TEST_CHECKLIST.txt > SHA256SUMS.txt
    ) || fail "Could not create checksums for $NAME"
}

build_profile "M68020-M68881" "$FLAGS_020" \
    "$RELEASE_ROOT/M68020-M68881"

build_profile "M68060-HARDFLOAT" "$FLAGS_060" \
    "$RELEASE_ROOT/M68060-HARDFLOAT"

{
    echo "VICE 3.1 FRF AmigaOS Release"
    echo "Packaged: $(date -Is)"
    echo "Source: $ROOT"
    echo
    echo "M68020/M68881:"
    echo "  $FLAGS_020"
    echo
    echo "M68060 hard-float:"
    echo "  $FLAGS_060"
    echo
    echo "Programs:"
    for p in $PROGRAMS; do
        echo "  $p"
    done
    echo
    echo "Logs:"
    echo "  $LOGDIR"
} > "$RELEASE_ROOT/BUILD_INFO.txt"

(
    cd "$RELEASE_ROOT" || exit 1
    find . -type f ! -name SHA256SUMS.txt -print0 |
        sort -z |
        xargs -0 sha256sum > SHA256SUMS.txt
) || fail "Could not create release checksums"

PARENT="$(dirname "$RELEASE_ROOT")"
ARCHIVE="$(basename "$RELEASE_ROOT").tar.gz"

tar -C "$PARENT" -czf "$PARENT/$ARCHIVE" "$(basename "$RELEASE_ROOT")" ||
    fail "Could not create release archive"

restore_makefile
trap - EXIT HUP INT TERM

echo
echo "============================================================"
echo "RELEASE BUILD COMPLETE"
echo "============================================================"
echo "Distribution:"
echo "  $RELEASE_ROOT"
echo "Archive:"
echo "  $PARENT/$ARCHIVE"
echo "Logs:"
echo "  $LOGDIR"
echo "src/Makefile restored."
