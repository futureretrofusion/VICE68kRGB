#!/usr/bin/env bash
set -u

ROOT="${1:-$HOME/Documents/Amiga/VICE/vice-3.1}"

UI="$ROOT/src/arch/amigaos/ui.c"
AHI="$ROOT/src/arch/amigaos/ahi.c"
AHIH="$ROOT/src/arch/amigaos/ahi.h"
AMIGA_DIR="$ROOT/src/arch/amigaos"

fail()
{
    echo "ERROR: $*" >&2
    exit 1
}

for f in "$UI" "$AHI" "$AHIH"; do
    [ -f "$f" ] || fail "Missing $f"
done

cd "$ROOT" || fail "Cannot enter $ROOT"

STAMP="$(date +%Y%m%d_%H%M%S)"
BACKUP="$ROOT/FRF_RECOVERY_BACKUPS/HARD_PAUSE_AHI_RELEASE_$STAMP"
mkdir -p "$BACKUP/src/arch/amigaos"

cp -v "$UI" "$BACKUP/src/arch/amigaos/ui.c.before"
cp -v "$AHI" "$BACKUP/src/arch/amigaos/ahi.c.before"
cp -v "$AHIH" "$BACKUP/src/arch/amigaos/ahi.h.before"

for f in "$AMIGA_DIR"/*uires.h; do
    [ -f "$f" ] || continue
    cp -v "$f" "$BACKUP/src/arch/amigaos/$(basename "$f").before"
done

for f in RELEASE_NOTES_FRF.txt FRF_CONTRIBUTIONS.txt README_FRF_RELEASE.txt; do
    [ -f "$f" ] && cp -v "$f" "$BACKUP/$f.before"
done

ROOT="$ROOT" python3 - <<'PY'
from pathlib import Path
import os
import re

root = Path(os.environ["ROOT"])
ui = root / "src/arch/amigaos/ui.c"
ahi = root / "src/arch/amigaos/ahi.c"
ahih = root / "src/arch/amigaos/ahi.h"
amiga = root / "src/arch/amigaos"

EXPECTED_MENUS = {
    "c64uires.h",
    "c64dtvuires.h",
    "c128uires.h",
    "vic20uires.h",
    "petuires.h",
    "plus4uires.h",
    "cbm2uires.h",
    "cbm5x0uires.h",
    "scpu64uires.h",
}


def function_bounds(text, signature):
    start = text.find(signature)
    if start < 0:
        raise SystemExit("Function not found: " + signature)

    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit("Opening brace not found: " + signature)

    depth = 0
    state = "code"
    i = brace

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "code":
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "/" and nxt == "/":
                state = "line"
                i += 1
            elif ch == "/" and nxt == "*":
                state = "block"
                i += 1
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return start, i + 1
        elif state == "string":
            if ch == "\\":
                i += 1
            elif ch == '"':
                state = "code"
        elif state == "char":
            if ch == "\\":
                i += 1
            elif ch == "'":
                state = "code"
        elif state == "line":
            if ch == "\n":
                state = "code"
        elif state == "block":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 1

        i += 1

    raise SystemExit("Could not parse function: " + signature)


def replace_function(text, signature, replacement):
    start, end = function_bounds(text, signature)
    return text[:start] + replacement + text[end:]


# ---------------------------------------------------------------
# Right-Amiga+P -> Pause on all machine menus.
# ---------------------------------------------------------------
menu_pattern = re.compile(
    r'ITEMTOGGLE\('
    r'(?P<label>IDMS_PAUSE)\s*,'
    r'(?P<gap1>\s*)'
    r'(?P<key>NULL|"P")\s*,'
    r'(?P<gap2>\s*)'
    r'(?P<id>IDM_PAUSE)'
    r'\)'
)

covered = set()

for path in sorted(amiga.glob("*uires.h")):
    text = path.read_text(errors="replace")

    for line_no, line in enumerate(text.splitlines(), 1):
        if re.search(r'ITEM[A-Z]*\([^,\n]+,\s*"P"\s*,', line):
            if "IDM_PAUSE" not in line:
                raise SystemExit(
                    "{}:{}: Right-Amiga+P already assigned: {}".format(
                        path.name, line_no, line.strip()
                    )
                )

    if "IDMS_PAUSE" not in text:
        continue

    text, count = menu_pattern.subn(
        lambda m: (
            "ITEMTOGGLE("
            + m.group("label")
            + ","
            + m.group("gap1")
            + '"P",'
            + m.group("gap2")
            + m.group("id")
            + ")"
        ),
        text,
        count=1,
    )

    if count != 1:
        raise SystemExit(
            "Could not set Pause command key in " + path.name
        )

    path.write_text(text)
    covered.add(path.name)

missing = sorted(EXPECTED_MENUS - covered)
if missing:
    raise SystemExit(
        "Pause menu definitions not patched in: " + ", ".join(missing)
    )

print("OK: Right-Amiga+P assigned to Pause in all machine menus")


# ---------------------------------------------------------------
# Add the AHI resume declaration.
# ---------------------------------------------------------------
h = ahih.read_text(errors="replace")

if "extern s32 ahi_resume(void);" not in h:
    needle = "extern void ahi_pause(void);"
    if needle not in h:
        raise SystemExit("ahi_pause declaration not found in ahi.h")

    h = h.replace(
        needle,
        needle
        + "\nextern s32 ahi_resume(void);"
          " /* FRF_HARD_PAUSE_AHI_RELEASE */",
        1,
    )

ahih.write_text(h)


# ---------------------------------------------------------------
# AHI backend-only release/reopen.
# ---------------------------------------------------------------
a = ahi.read_text(errors="replace")

state_block = '''
/*
 * FRF_HARD_PAUSE_AHI_RELEASE:
 * Preserve active backend parameters outside audio_t because ahi_close()
 * clears audio_t. Pause can release ahi.device completely and resume
 * without closing or recreating the VICE SID engine.
 */
static s32 frf_ahi_pause_frequency = 0;
static u32 frf_ahi_pause_mode = 0;
static s32 frf_ahi_pause_fragsize = 0;
static s32 frf_ahi_pause_frags = 0;
static void (*frf_ahi_pause_callback)(s64 time) = NULL;
static int frf_ahi_pause_reopen = 0;

'''

if "static int frf_ahi_pause_reopen = 0;" not in a:
    open_pos = a.find("s32 ahi_open(")
    if open_pos < 0:
        raise SystemExit("ahi_open function not found")
    a = a[:open_pos] + state_block + a[open_pos:]

pause_fn = '''void ahi_pause(void)
{
    if (audio.audio_task == NULL || frf_ahi_pause_reopen) {
        return;
    }

    /*
     * FRF_HARD_PAUSE_AHI_RELEASE:
     * Save the sample-domain fragment size before ahi_close() clears
     * audio_t, then release the AHI task/device/requests/ports/buffers.
     */
    frf_ahi_pause_frequency = audio.frequency;
    frf_ahi_pause_mode = audio.mode;
    frf_ahi_pause_fragsize =
        audio.fragsize >> audio.samples_to_bytes;
    frf_ahi_pause_frags = audio.frags;
    frf_ahi_pause_callback = audio.audio_sync;
    frf_ahi_pause_reopen = 1;

    ahi_close();
}'''

a = replace_function(a, "void ahi_pause(void)", pause_fn)

resume_fn = '''s32 ahi_resume(void)
{
    s32 result;

    if (!frf_ahi_pause_reopen) {
        return 0;
    }

    result = ahi_open(frf_ahi_pause_frequency,
                      frf_ahi_pause_mode,
                      frf_ahi_pause_fragsize,
                      frf_ahi_pause_frags,
                      frf_ahi_pause_callback);

    if (result == 0) {
        frf_ahi_pause_reopen = 0;
    }

    return result;
}'''

if "s32 ahi_resume(void)" in a:
    a = replace_function(a, "s32 ahi_resume(void)", resume_fn)
else:
    close_start, close_end = function_bounds(a, "void ahi_close(void)")
    a = a[:close_end] + "\n\n" + resume_fn + a[close_end:]

ahi.write_text(a)

print("OK: pause releases AHI and resume reopens identical backend settings")


# ---------------------------------------------------------------
# Hard CPU pause trap with AHI release for the paused interval.
# ---------------------------------------------------------------
u = ui.read_text(errors="replace")

if '#include "ahi.h"' not in u:
    include_anchor = '#include "vice.h"'
    if include_anchor not in u:
        raise SystemExit('Could not find #include "vice.h" in ui.c')
    u = u.replace(
        include_anchor,
        include_anchor + '\n#include "ahi.h"',
        1,
    )

trap_fn = '''static void pause_trap(WORD addr, void *data)
{
    /*
     * FRF_HARD_PAUSE_AHI_RELEASE:
     * This main-CPU trap is a hard emulation stop. Only the Amiga UI
     * event pump stays alive so Pause can be toggled again.
     */
    ui_display_paused(1);
    vsync_suspend_speed_eval();

    /* Release ahi.device for the complete paused interval. */
    ahi_pause();

    while (is_paused) {
        timer_usleep(vice_timer, 1000000 / 100);
        ui_event_handle();
    }

    /* Restore AHI before allowing the emulated CPU to continue. */
    (void)ahi_resume();
    vsync_suspend_speed_eval();
    ui_display_paused(0);
}'''

u = replace_function(
    u,
    "static void pause_trap(WORD addr, void *data)",
    trap_fn,
)

pause_control_fn = '''void ui_pause_emulation(int flag)
{
    if (network_connected()) {
        return;
    }

    if (flag && !is_paused) {
        is_paused = 1;
        interrupt_maincpu_trigger_trap(pause_trap, 0);
    } else if (!flag && is_paused) {
        /*
         * pause_trap() reopens AHI and restores the title after seeing
         * this flag clear.
         */
        is_paused = 0;
    }
}'''

u = replace_function(
    u,
    "void ui_pause_emulation(int flag)",
    pause_control_fn,
)

ui.write_text(u)

print("OK: emulated CPU remains trapped until Pause is toggled off")


# ---------------------------------------------------------------
# Update release documents if present in the source tree.
# ---------------------------------------------------------------
notes = root / "RELEASE_NOTES_FRF.txt"
if notes.exists():
    text = notes.read_text(errors="replace")
    additions = (
        "- Right-Amiga+P toggles hard Pause/Resume.\n"
        "- Pause traps and completely halts emulated CPU progression.\n"
        "- AHI is fully released while paused and reopened before resume.\n"
        "- SID engine state remains resident while AHI is released.\n"
    )
    if "Right-Amiga+P toggles hard Pause/Resume." not in text:
        anchor = "- Right-Amiga+X remains Quit.\n"
        if anchor in text:
            text = text.replace(anchor, anchor + additions, 1)
        else:
            text += "\nPause behaviour\n---------------\n" + additions
        notes.write_text(text)

contrib = root / "FRF_CONTRIBUTIONS.txt"
if contrib.exists():
    text = contrib.read_text(errors="replace")
    additions = (
        "- Added Right-Amiga+P as the Pause/Resume command key.\n"
        "- Added a hard main-CPU pause trap with UI-only event processing.\n"
        "- Added pause-time AHI release and parameter-preserving reopen.\n"
        "- Preserved FastSID/ReSID state while AHI is released.\n"
    )
    if "pause-time AHI release" not in text:
        anchor = "Shutdown stability\n------------------\n"
        block = (
            "Pause and resource management\n"
            "-----------------------------\n"
            + additions
            + "\n"
        )
        if anchor in text:
            text = text.replace(anchor, block + anchor, 1)
        else:
            text += "\n" + block
        contrib.write_text(text)

readme = root / "README_FRF_RELEASE.txt"
if readme.exists():
    text = readme.read_text(errors="replace")
    if "Right-Amiga+P is hard Pause/Resume." not in text:
        anchor = "Right-Amiga+X is Quit.\n"
        line = (
            "Right-Amiga+P is hard Pause/Resume; "
            "AHI is released while paused.\n"
        )
        if anchor in text:
            text = text.replace(anchor, anchor + line, 1)
        else:
            text += "\n" + line
        readme.write_text(text)

print("OK: release notes and FRF contribution credits updated")


# ---------------------------------------------------------------
# Final validation.
# ---------------------------------------------------------------
u = ui.read_text(errors="replace")
a = ahi.read_text(errors="replace")
h = ahih.read_text(errors="replace")

for token in (
    "FRF_HARD_PAUSE_AHI_RELEASE",
    "ahi_pause();",
    "(void)ahi_resume();",
    "while (is_paused)",
    "interrupt_maincpu_trigger_trap(pause_trap, 0);",
):
    if token not in u:
        raise SystemExit("Missing ui.c token: " + token)

for token in (
    "static int frf_ahi_pause_reopen = 0;",
    "frf_ahi_pause_fragsize =",
    "ahi_close();",
    "s32 ahi_resume(void)",
    "result = ahi_open(",
):
    if token not in a:
        raise SystemExit("Missing ahi.c token: " + token)

if "extern s32 ahi_resume(void);" not in h:
    raise SystemExit("ahi_resume declaration missing")

for name in sorted(EXPECTED_MENUS):
    text = (amiga / name).read_text(errors="replace")
    if not re.search(
        r'ITEMTOGGLE\(IDMS_PAUSE\s*,\s*"P"\s*,\s*IDM_PAUSE\)',
        text,
    ):
        raise SystemExit("Right-Amiga+P validation failed in " + name)

print()
print("FINAL VALIDATION")
print("  Right-Amiga+P: Pause/Resume")
print("  Main CPU: hard-stopped inside pause trap")
print("  AHI: closed for the complete paused interval")
print("  Resume: AHI reopened before CPU continues")
print("  SID state: not closed or recreated")
PY

STATUS=$?
if [ "$STATUS" -ne 0 ]; then
    echo
    echo "Patch failed. Restore files from:"
    echo "  $BACKUP"
    exit 1
fi

echo
echo "Pause menu bindings:"
grep -RIn --include='*uires.h' \
    'ITEMTOGGLE(IDMS_PAUSE' \
    src/arch/amigaos | sort

echo
echo "AHI pause/reopen markers:"
grep -n \
    'FRF_HARD_PAUSE_AHI_RELEASE\|ahi_pause\|ahi_resume' \
    src/arch/amigaos/ahi.c \
    src/arch/amigaos/ahi.h \
    src/arch/amigaos/ui.c | head -120

echo
echo "Backup:"
echo "  $BACKUP"
