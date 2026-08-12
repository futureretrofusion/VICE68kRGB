#!/usr/bin/env bash
set -u
set -o pipefail

ROOT="${1:-$HOME/Documents/Amiga/VICE/vice-3.1}"

fail()
{
    echo "ERROR: $*" >&2
    exit 1
}

cd "$ROOT" || fail "Cannot enter source tree: $ROOT"

for f in \
    src/arch/amigaos/timer.c \
    src/arch/amigaos/ahi.c \
    src/arch/amigaos/ahi.h \
    src/sounddrv/soundahi.c
do
    [ -f "$f" ] || fail "Required file missing: $ROOT/$f"
done

grep -q 's32 ahi_resume(void)' src/arch/amigaos/ahi.c ||
    fail "ahi_resume() implementation is missing; apply the FRF hard-pause/AHI-release patch first"

grep -q 'extern s32 ahi_resume(void);' src/arch/amigaos/ahi.h ||
    fail "ahi_resume() declaration is missing from ahi.h"

STAMP="$(date +%Y%m%d_%H%M%S)"
BACKUP="$ROOT/FRF_RECOVERY_BACKUPS/FINAL_AHI_TIMING_STARTUP_FIXES_$STAMP"

mkdir -p "$BACKUP/src/arch/amigaos" "$BACKUP/src/sounddrv"
cp -v src/arch/amigaos/timer.c "$BACKUP/src/arch/amigaos/timer.c.before"
cp -v src/sounddrv/soundahi.c "$BACKUP/src/sounddrv/soundahi.c.before"

ROOT="$ROOT" python3 - <<'PY'
from pathlib import Path
import os
import re

root = Path(os.environ["ROOT"])
timer_path = root / "src/arch/amigaos/timer.c"
soundahi_path = root / "src/sounddrv/soundahi.c"

timer = timer_path.read_text(errors="replace")
soundahi = soundahi_path.read_text(errors="replace")

timer_fixed = '''void timer_subtime(void *t, struct timeval *dt, struct timeval *st)
{
    long seconds;
    long microseconds;

    /*
     * FRF_AHI_TIMER_SUBTRACT_FIX:
     * The original AmigaOS 3 expression used a malformed ternary and did
     * not subtract timeval values. AHI uses this elapsed time to estimate
     * its current read position and free buffer space. A bad result makes
     * VICE believe the AHI ring is still full and can throttle emulation to
     * approximately one frame per complete audio buffer.
     */
    seconds = dt->tv_sec - st->tv_sec;
    microseconds = dt->tv_usec - st->tv_usec;

    if (microseconds < 0) {
        microseconds += 1000000;
        seconds--;
    }

    dt->tv_sec = seconds;
    dt->tv_usec = microseconds;
}'''

if "FRF_AHI_TIMER_SUBTRACT_FIX" not in timer:
    timer_pattern = re.compile(
        r'void\s+timer_subtime\s*\(\s*void\s*\*t\s*,\s*'
        r'struct\s+timeval\s*\*dt\s*,\s*struct\s+timeval\s*\*st\s*\)\s*'
        r'\{\s*'
        r'int\s+extrasub\s*=\s*0\s*;\s*'
        r'if\s*\(\s*dt->tv_usec\s*<\s*st->tv_usec\s*\)\s*'
        r'\{\s*extrasub\s*=\s*1\s*;\s*\}\s*'
        r'dt->tv_usec\s*=\s*\(\s*dt->tv_usec\s*\*\s*'
        r'\(\s*extrasub\s*==\s*1\s*\)\s*\?\s*10\s*:\s*1\s*\)\s*'
        r'-\s*st->tv_usec\s*;\s*'
        r'dt->tv_sec\s*=\s*dt->tv_sec\s*-\s*'
        r'\(\s*st->tv_sec\s*\+\s*extrasub\s*\)\s*;\s*'
        r'\}',
        re.S,
    )
    timer, count = timer_pattern.subn(timer_fixed, timer, count=1)
    if count != 1:
        raise SystemExit(
            "Could not find the original broken timer_subtime() body. "
            "No files were written."
        )

resume_fixed = '''static int _ahi_resume(void)
{
    /*
     * FRF_AHI_BACKEND_RESUME_FIX:
     * ahi_pause() now releases ahi.device and clears the active backend.
     * Generic VICE sound_suspend()/sound_resume() cycles also occur during
     * startup and UI operations, not only during the FRF hard pause.
     *
     * The old no-op hook returned success while leaving audio.audio_task
     * closed. snddata.playdev then still looked valid to sound.c, so VICE
     * remained silent until a Sound Settings OK forced a complete reopen.
     */
    return ahi_resume();
}'''

if "FRF_AHI_BACKEND_RESUME_FIX" not in soundahi:
    resume_pattern = re.compile(
        r'static\s+int\s+_ahi_resume\s*\(\s*void\s*\)\s*'
        r'\{\s*return\s+0\s*;\s*\}',
        re.S,
    )
    soundahi, count = resume_pattern.subn(resume_fixed, soundahi, count=1)
    if count != 1:
        raise SystemExit(
            "Could not find the original no-op _ahi_resume() body. "
            "No files were written."
        )

checks = [
    ("timer marker", "FRF_AHI_TIMER_SUBTRACT_FIX" in timer),
    ("real microsecond subtraction",
     "microseconds = dt->tv_usec - st->tv_usec;" in timer),
    ("one-second timeval borrow", "microseconds += 1000000;" in timer),
    ("AHI resume marker", "FRF_AHI_BACKEND_RESUME_FIX" in soundahi),
    ("real backend reopen", "return ahi_resume();" in soundahi),
    ("old timer expression removed",
     "dt->tv_usec * (extrasub ==1 ) ? 10 : 1" not in timer),
    ("old no-op resume removed",
     not bool(re.search(
         r'static\s+int\s+_ahi_resume\s*\(\s*void\s*\)\s*'
         r'\{\s*return\s+0\s*;\s*\}',
         soundahi,
         re.S,
     ))),
]

failed = [name for name, ok in checks if not ok]
if failed:
    raise SystemExit("Validation failed: " + ", ".join(failed))

timer_path.write_text(timer)
soundahi_path.write_text(soundahi)

print("Applied/validated:")
for name, _ in checks:
    print("  " + name)
PY

STATUS=$?
if [ "$STATUS" -ne 0 ]; then
    cp -v "$BACKUP/src/arch/amigaos/timer.c.before" src/arch/amigaos/timer.c
    cp -v "$BACKUP/src/sounddrv/soundahi.c.before" src/sounddrv/soundahi.c
    fail "Final AHI fixes failed; both source files were restored"
fi

echo
echo "Final shared AmigaOS fixes are present:"
echo "  timer.c    - correct timeval subtraction"
echo "  soundahi.c - real AHI reopen from the VICE resume hook"
echo
echo "Backup:"
echo "  $BACKUP"
