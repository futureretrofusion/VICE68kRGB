#!/usr/bin/env bash
set -u
set -o pipefail

ROOT="${1:-$HOME/Documents/Amiga/VICE/vice-3.1}"
PARENT="${2:-$HOME/Documents/Amiga/VICE}"
RELEASE_NAME="${3:-VICE-3.1-FRF-AmigaOS-PiStorm-2026-06-28-FINAL}"

RELEASE_ROOT="$PARENT/$RELEASE_NAME"
FINAL_ZIP="$PARENT/$RELEASE_NAME.zip"
PROGRAMS="x64 x64sc x64dtv xscpu64 x128 xvic xpet xplus4 xcbm2 xcbm5x0 vsid"

fail()
{
    echo "ERROR: $*" >&2
    exit 1
}

export PATH="/opt/amiga/bin:$PATH"
hash -r 2>/dev/null || true

cd "$ROOT" || fail "Cannot enter source tree: $ROOT"

for f in \
    frf_build_complete_release_020_060_final_v2.sh \
    RELEASE_NOTES_FRF.txt \
    FRF_CONTRIBUTIONS.txt \
    README_FRF_RELEASE.txt \
    FRF_SOURCE_COMMENTING.txt \
    src/arch/amigaos/ui.c \
    src/arch/amigaos/ahi.c \
    src/arch/amigaos/ahi.h
do
    [ -f "$f" ] || fail "Required file missing: $ROOT/$f"
done

AUDIT="$ROOT/FRF_SOURCE_COMMENT_AUDIT.txt"

ROOT="$ROOT" AUDIT="$AUDIT" python3 - <<'PY'
from pathlib import Path
import os
import re
import sys

root = Path(os.environ["ROOT"])
audit_path = Path(os.environ["AUDIT"])

checks = []
failures = []
warnings = []

def add(name, ok, detail):
    checks.append((name, ok, detail))
    if not ok:
        failures.append(name + ": " + detail)

def read(rel):
    p = root / rel
    if not p.exists():
        failures.append(rel + ": file is missing")
        return ""
    return p.read_text(errors="replace")

ui = read("src/arch/amigaos/ui.c")
ahi = read("src/arch/amigaos/ahi.c")
ahih = read("src/arch/amigaos/ahi.h")
archdep = read("src/arch/amigaos/archdep.c")
loadlibs = read("src/arch/amigaos/loadlibs.c")
sidres = read("src/sid/sid-resources.c")
video = read("src/arch/amigaos/video.c")

# Hard Pause/AHI: code and explanatory comments.
add(
    "Hard-pause marker in ui.c",
    "FRF_HARD_PAUSE_AHI_RELEASE" in ui,
    "feature marker and pause-trap explanation",
)
add(
    "Hard-pause marker in ahi.c",
    "FRF_HARD_PAUSE_AHI_RELEASE" in ahi,
    "feature marker and AHI ownership explanation",
)
add(
    "AHI resume declaration",
    "extern s32 ahi_resume(void);" in ahih,
    "public AmigaOS backend resume declaration",
)
add(
    "CPU remains in pause trap",
    "while (is_paused)" in ui and "ui_event_handle();" in ui,
    "main CPU halted while UI events remain available",
)
add(
    "AHI released while paused",
    "ahi_pause();" in ui and "ahi_close();" in ahi,
    "pause trap calls backend release",
)
add(
    "AHI reopened before resume",
    "(void)ahi_resume();" in ui and "result = ahi_open(" in ahi,
    "saved backend parameters are used to reopen AHI",
)
add(
    "SID core not closed by pause",
    "sound_close();" not in re.search(
        r'static void pause_trap\(.*?^\}', ui, re.M | re.S
    ).group(0) if re.search(
        r'static void pause_trap\(.*?^\}', ui, re.M | re.S
    ) else False,
    "pause trap does not destroy the VICE sound/SID core",
)

# Menu bindings.
menus = [
    "c64uires.h", "c64dtvuires.h", "c128uires.h", "vic20uires.h",
    "petuires.h", "plus4uires.h", "cbm2uires.h", "cbm5x0uires.h",
    "scpu64uires.h",
]
for name in menus:
    text = read("src/arch/amigaos/" + name)
    add(
        name + " Right-Amiga+P",
        bool(re.search(
            r'ITEMTOGGLE\(IDMS_PAUSE\s*,\s*"P"\s*,\s*IDM_PAUSE\)',
            text,
        )),
        "P assigned to Pause",
    )
    add(
        name + " Right-Amiga+Q disabled",
        not bool(re.search(
            r'ITEMTOGGLE\(IDMS_GRAB_MOUSE\s*,\s*"Q"\s*,\s*IDM_MOUSE\)',
            text,
        )),
        "Q is not assigned to Grab Mouse",
    )

# Shutdown protections claimed in release notes.
add(
    "Network shutdown guard",
    "FRF_NETWORK_DISABLED_SHUTDOWN_GUARD" in archdep
    or bool(re.search(
        r'#ifdef\s+HAVE_NETWORK\s+archdep_network_shutdown\(\);',
        archdep,
        re.S,
    )),
    "network shutdown only occurs when networking is available",
)
add(
    "Null-safe CloseLibrary",
    "amiga_libs[i].lib_base && amiga_libs[i].lib_base[0]" in loadlibs,
    "both the base slot and actual library base are checked",
)

# SID defaults and ReSID availability.
add(
    "FastSID default",
    bool(re.search(
        r'"SidEngine"\s*,\s*SID_ENGINE_FASTSID',
        sidres,
    )),
    "FastSID selected as default",
)
add(
    "SID filters default off",
    bool(re.search(
        r'"SidFilters"\s*,\s*0',
        sidres,
    )),
    "SID filters disabled by default",
)

# Optional historical markers. Missing markers are warnings, not failures,
# because older applied patches may use equivalent comments.
optional = {
    "Native P96 dirty path marker": "FRF_MAY17_NATIVE_DIRTY_FASTPATH_V2",
    "Direct signed 16-bit AHI marker": "FRF_MAY17_DIRECT_M16S",
}
combined = video + "\n" + ahi
for name, token in optional.items():
    if token not in combined:
        warnings.append(name + ": marker not found; verify equivalent code manually")

# Confirm substantive marker occurrences are inside or immediately adjacent
# to comments rather than being unexplained magic tokens.
for rel, text, token in [
    ("ui.c", ui, "FRF_HARD_PAUSE_AHI_RELEASE"),
    ("ahi.c", ahi, "FRF_HARD_PAUSE_AHI_RELEASE"),
]:
    pos = text.find(token)
    context = text[max(0, pos - 250):pos + 500] if pos >= 0 else ""
    commented = "/*" in context and "*/" in context
    add(
        rel + " explanatory comment",
        commented,
        "FRF marker is accompanied by a block comment",
    )

lines = []
lines.append("FRF SOURCE COMMENT AND FEATURE AUDIT")
lines.append("=" * 38)
lines.append("")
lines.append("Source: " + str(root))
lines.append("")
lines.append("Critical checks")
lines.append("---------------")
for name, ok, detail in checks:
    lines.append(("[PASS] " if ok else "[FAIL] ") + name)
    lines.append("       " + detail)

lines.append("")
lines.append("Warnings")
lines.append("--------")
if warnings:
    for warning in warnings:
        lines.append("[WARN] " + warning)
else:
    lines.append("None.")

lines.append("")
lines.append("Comment contexts")
lines.append("----------------")
for rel, text, token in [
    ("src/arch/amigaos/ui.c", ui, "FRF_HARD_PAUSE_AHI_RELEASE"),
    ("src/arch/amigaos/ahi.c", ahi, "FRF_HARD_PAUSE_AHI_RELEASE"),
    ("src/arch/amigaos/archdep.c", archdep,
     "FRF_NETWORK_DISABLED_SHUTDOWN_GUARD"),
]:
    pos = text.find(token)
    if pos < 0:
        continue
    start_line = text[:pos].count("\n") + 1
    context_lines = text.splitlines()
    lo = max(0, start_line - 4)
    hi = min(len(context_lines), start_line + 9)
    lines.append("")
    lines.append("{} around line {}".format(rel, start_line))
    for number in range(lo, hi):
        lines.append("{:6d}: {}".format(number + 1, context_lines[number]))

lines.append("")
lines.append("Result")
lines.append("------")
if failures:
    lines.append("FAILED: {} critical check(s) failed.".format(len(failures)))
    for failure in failures:
        lines.append("- " + failure)
else:
    lines.append("PASSED: critical FRF changes are present and documented.")

audit_path.write_text("\n".join(lines) + "\n")

if failures:
    print(audit_path.read_text())
    sys.exit(1)

print(audit_path.read_text())
PY

[ "$?" -eq 0 ] ||
    fail "Source/comment audit failed; see $AUDIT"

rm -rf "$RELEASE_ROOT"
rm -f "$FINAL_ZIP"
mkdir -p "$PARENT"

echo
echo "Building both processor suites..."
bash "$ROOT/frf_build_complete_release_020_060_final_v2.sh" \
    "$ROOT" "$RELEASE_ROOT" ||
    fail "Complete binary build failed"

mkdir -p \
    "$RELEASE_ROOT/Binaries" \
    "$RELEASE_ROOT/Source" \
    "$RELEASE_ROOT/Build-Logs"

# Move the two profile folders under Binaries for a clear release layout.
for profile in M68020-M68881 M68060-HARDFLOAT; do
    [ -d "$RELEASE_ROOT/$profile" ] ||
        fail "Missing built profile directory: $profile"
    mv "$RELEASE_ROOT/$profile" "$RELEASE_ROOT/Binaries/$profile"
done

# Validate every binary before source packaging.
for profile in M68020-M68881 M68060-HARDFLOAT; do
    for program in $PROGRAMS; do
        [ -s "$RELEASE_ROOT/Binaries/$profile/$program" ] ||
            fail "Missing binary: $profile/$program"
    done
done

cp -v "$ROOT/FRF_SOURCE_COMMENT_AUDIT.txt" \
    "$RELEASE_ROOT/SOURCE_COMMENT_AUDIT.txt"
cp -v "$ROOT/FRF_SOURCE_COMMENTING.txt" \
    "$RELEASE_ROOT/FRF_SOURCE_COMMENTING.txt"

# Copy the newest build-log directory into the release.
LATEST_LOGDIR="$(
    find "$ROOT" -maxdepth 1 -type d -name 'FRF_RELEASE_LOGS_*' \
        -printf '%T@ %p\n' |
    sort -nr |
    head -1 |
    cut -d' ' -f2-
)"

if [ -n "$LATEST_LOGDIR" ] && [ -d "$LATEST_LOGDIR" ]; then
    cp -a "$LATEST_LOGDIR"/. "$RELEASE_ROOT/Build-Logs/"
fi

SOURCE_STAGE="$PARENT/.${RELEASE_NAME}_source_stage"
SOURCE_ZIP="$RELEASE_ROOT/Source/VICE-3.1-FRF-Modified-Source.zip"

rm -rf "$SOURCE_STAGE"
mkdir -p "$SOURCE_STAGE"

ROOT="$ROOT" STAGE="$SOURCE_STAGE" python3 - <<'PY'
from pathlib import Path
import os
import shutil

root = Path(os.environ["ROOT"]).resolve()
stage = Path(os.environ["STAGE"]).resolve()
dest = stage / "vice-3.1-frf-modified-source"

programs = {
    "x64", "x64sc", "x64dtv", "xscpu64", "x128", "xvic",
    "xpet", "xplus4", "xcbm2", "xcbm5x0", "vsid",
    "c1541", "petcat", "cartconv",
}

excluded_dirs = {
    ".git", ".svn", ".hg", ".deps", "autom4te.cache",
    "FRF_RECOVERY_BACKUPS", "src/src",
}

excluded_suffixes = {
    ".o", ".a", ".lo", ".Tpo", ".Po", ".log", ".zip",
    ".gz", ".bz2", ".xz", ".7z",
}

def ignore(directory, names):
    directory_path = Path(directory)
    ignored = set()

    for name in names:
        path = directory_path / name
        rel = path.relative_to(root)

        if name in excluded_dirs:
            ignored.add(name)
            continue

        if rel.parts and (
            rel.parts[0].startswith("FRF_RELEASE_LOGS_")
            or rel.parts[0].startswith("VICE-3.1-FRF-RELEASE")
        ):
            ignored.add(name)
            continue

        if name.startswith("Makefile.FRF_RELEASE_BACKUP_"):
            ignored.add(name)
            continue

        if name.endswith((".before", ".bak", "~")):
            ignored.add(name)
            continue

        if path.is_file():
            if path.suffix in excluded_suffixes:
                ignored.add(name)
                continue
            if path.parent.name == "src" and name in programs:
                ignored.add(name)
                continue
            if name in {"config.log", "config.status"}:
                ignored.add(name)
                continue

    return ignored

shutil.copytree(
    root,
    dest,
    symlinks=True,
    ignore=ignore,
)

# Remove any nested generated dependency directories missed through unusual
# paths, and ensure no built emulator executable slipped into src/.
for path in sorted(dest.rglob(".deps"), reverse=True):
    if path.is_dir():
        shutil.rmtree(path)

for program in programs:
    candidate = dest / "src" / program
    if candidate.exists():
        candidate.unlink()

print(dest)
PY

[ "$?" -eq 0 ] ||
    fail "Could not prepare clean modified source tree"

# Confirm an upstream licence file is included.
if ! find "$SOURCE_STAGE" -maxdepth 3 -type f \
    \( -iname 'COPYING*' -o -iname 'LICENSE*' \) |
    grep -q .
then
    fail "No upstream COPYING/LICENSE file found in source package"
fi

# Add audit and commenting documents inside the source package too.
SOURCE_TREE="$SOURCE_STAGE/vice-3.1-frf-modified-source"
cp "$ROOT/FRF_SOURCE_COMMENT_AUDIT.txt" \
    "$SOURCE_TREE/FRF_SOURCE_COMMENT_AUDIT.txt"
cp "$ROOT/FRF_SOURCE_COMMENTING.txt" \
    "$SOURCE_TREE/FRF_SOURCE_COMMENTING.txt"

SOURCE_TREE="$SOURCE_TREE" python3 - <<'PY'
from pathlib import Path
import hashlib
import os

root = Path(os.environ["SOURCE_TREE"])
lines = []

for path in sorted(p for p in root.rglob("*") if p.is_file()):
    rel = path.relative_to(root)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    lines.append("{}  {}".format(digest, rel.as_posix()))

(root / "FRF_SOURCE_MANIFEST_SHA256.txt").write_text(
    "\n".join(lines) + "\n"
)
PY

STAGE="$SOURCE_STAGE" SOURCE_ZIP="$SOURCE_ZIP" python3 - <<'PY'
from pathlib import Path
import os
import zipfile

stage = Path(os.environ["STAGE"])
target = Path(os.environ["SOURCE_ZIP"])

with zipfile.ZipFile(
    target, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
) as archive:
    for path in sorted(stage.rglob("*")):
        if path.is_file():
            archive.write(path, path.relative_to(stage))

print(target)
PY

[ -s "$SOURCE_ZIP" ] ||
    fail "Modified source ZIP was not produced"

rm -rf "$SOURCE_STAGE"

cat > "$RELEASE_ROOT/SOURCE_INCLUDED.txt" <<'EOF'
Corresponding Modified Source Included
======================================

Source/VICE-3.1-FRF-Modified-Source.zip contains the complete corresponding
modified VICE 3.1 source used for this release, excluding generated object
files, static archives, logs and previously built executables.

It retains the upstream source files, build files, attribution and licence
materials, plus the FRF release documentation and source audit.
EOF

# Rewrite the top-level README to match the final folder layout.
cp "$ROOT/README_FRF_RELEASE.txt" "$RELEASE_ROOT/README_FIRST.txt"
cp "$ROOT/RELEASE_NOTES_FRF.txt" "$RELEASE_ROOT/RELEASE_NOTES.txt"
cp "$ROOT/FRF_CONTRIBUTIONS.txt" "$RELEASE_ROOT/FRF_CONTRIBUTIONS.txt"

# Recreate the release-wide checksums after adding source and logs.
(
    cd "$RELEASE_ROOT" || exit 1
    find . -type f ! -name SHA256SUMS.txt -print0 |
        sort -z |
        xargs -0 sha256sum > SHA256SUMS.txt
) || fail "Could not generate final release checksums"

# Build one final ZIP with files rooted under RELEASE_NAME/.
RELEASE_ROOT="$RELEASE_ROOT" FINAL_ZIP="$FINAL_ZIP" python3 - <<'PY'
from pathlib import Path
import os
import zipfile

release = Path(os.environ["RELEASE_ROOT"]).resolve()
target = Path(os.environ["FINAL_ZIP"]).resolve()
root_name = release.name

with zipfile.ZipFile(
    target, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
) as archive:
    for path in sorted(release.rglob("*")):
        if path.is_file():
            archive.write(
                path,
                Path(root_name) / path.relative_to(release),
            )

print(target)
PY

[ -s "$FINAL_ZIP" ] ||
    fail "Final release ZIP was not produced"

echo
echo "============================================================"
echo "FINAL RELEASE READY"
echo "============================================================"
echo
echo "Release folder:"
echo "  $RELEASE_ROOT"
echo
echo "Final ZIP:"
echo "  $FINAL_ZIP"
echo
echo "Modified source ZIP:"
echo "  $SOURCE_ZIP"
echo
echo "All 22 emulator binaries verified:"
echo "  11 M68020/M68881"
echo "  11 M68060 hard-float"
echo
echo "Comment and feature audit:"
echo "  $RELEASE_ROOT/SOURCE_COMMENT_AUDIT.txt"
