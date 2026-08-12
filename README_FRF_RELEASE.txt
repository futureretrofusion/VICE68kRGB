VICE 3.1 FRF AMIGAOS / PISTORM RELEASE
======================================

Binaries/M68020-M68881/ contains the broad-compatibility build.
Binaries/M68060-HARDFLOAT/ contains the 68060 hard-float build.
Source/ contains the complete corresponding modified source ZIP.

Final shared AHI fixes
----------------------
- Correct AmigaOS timeval subtraction: removes the false 6-7 fps throttle.
- Real AHI resume hook: sound initialises without opening Sound Settings.

Keys
----
Right-Amiga+P  Hard Pause/Resume; AHI is released while paused
Right-Amiga+X  Quit
Right-Amiga+Q  Intentionally disabled
Grab Mouse     Still available through the menu

No Commodore ROM images are included.

See:
- RELEASE_NOTES.txt
- FRF_CONTRIBUTIONS.txt
- FRF_SOURCE_COMMENTING.txt
- SOURCE_COMMENT_AUDIT.txt
- RUNTIME_TEST_CHECKLIST.txt
- BUILD_INFO.txt
- SHA256SUMS.txt
