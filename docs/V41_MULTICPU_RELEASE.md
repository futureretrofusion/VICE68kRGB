# VICE68kRGB V41 MultiCPU release

This document defines the intended first binary release layout for the Future Retro Fusion **VICE 3.1 AmigaOS m68k** branch.

## Source generation

The release target is **VICE68kRGB V41 — Persistent RGB Screenmode**.

V41 must retain the proven native RGB renderer marker:

```text
FRF_PAL_RGB_V37_MAME_BULK_C2P
```

and must not contain the rejected scaling/EHB experiment:

```text
FRF_PAL_RGB_V38_FIT_EHB_BULK_C2P
```

The V41 source marker is:

```text
FRF_PAL_RGB_V41_PERSISTENT_SCREENMODE
```

**Do not publish V41 binaries against an older public source tree.** The matching V41 source must be synchronized to `main` before the V41 GitHub Release is created.

## MultiCPU x64 binaries

The release uses six fully independent builds. Target objects and static libraries must never be reused across CPU/FPU profiles.

| Binary | CPU/FPU profile |
|---|---|
| `x64-VICE68kRGB-V41_020` | `-m68020 -msoft-float` |
| `x64-VICE68kRGB-V41_020_FPU` | `-m68020 -m68881` |
| `x64-VICE68kRGB-V41_040` | `-m68040 -msoft-float` |
| `x64-VICE68kRGB-V41_040_FPU` | `-m68040 -mhard-float` |
| `x64-VICE68kRGB-V41_060` | `-m68060 -msoft-float` |
| `x64-VICE68kRGB-V41_060_FPU` | `-m68060 -mhard-float` |

Common FRF optimisation flags:

```text
-Ofast -fomit-frame-pointer -fno-strict-aliasing -noixemul -finline-functions
```

The CPU/FPU selection must be passed through compile and link stages.

## Proven V41 build route

The V41 work uses the configured VICE 3.1 source tree and the AmigaOS cross compiler in `/opt/amiga/bin`.

The proven x64 link route uses:

```text
make -C src x64 \
  CFLAGS="..." CXXFLAGS="..." OBJCFLAGS="..." CCASFLAGS="..." \
  LIBS="../src/socketdrv/libsocketdrv.a -lm <libmui.a> -lauto" \
  resid_libs="../src/resid/libresid.a" \
  resid_dtv_libs="../src/resid-dtv/libresiddtv.a" \
  V=1
```

For MultiCPU packaging, all target `.o`, `.a`, `.lo` and related dependency products are removed in an isolated source copy before each build.

## V41 persistent native RGB configuration

V41 adds a normal VICE string resource:

```text
FRFRGBMode
```

Format:

```text
ModeID,width,height,depth[,overscan,autoscroll]
```

Because this is a VICE resource, Save Settings / Save Settings on Exit can persist it.

Command-line selection:

```text
-frfrgb ModeID,width,height,depth
```

Clear the configured native RGB mode:

```text
+frfrgb
```

Fullscreen uses the normal VICE option:

```text
-fullscreen
```

Example:

```text
x64 -frfrgb 0xMODEID,320,256,4 -fullscreen
```

Snapshot example:

```text
x64 -frfrgb 0xMODEID,320,256,4 -fullscreen \
    -autostart "Work:Snapshots/game.vsf"
```

If no `FRFRGBMode` is saved, entering fullscreen may open the existing ASL screenmode requester. A successful native RGB choice is written back to `FRFRGBMode` and can be reused later.

## Release assets

The GitHub release should contain:

```text
VICE68kRGB-V41-AmigaOS68K-MultiCPU.zip
x64-VICE68kRGB-V41_020
x64-VICE68kRGB-V41_020_FPU
x64-VICE68kRGB-V41_040
x64-VICE68kRGB-V41_040_FPU
x64-VICE68kRGB-V41_060
x64-VICE68kRGB-V41_060_FPU
BUILD_VARIANTS.txt
SHA256SUMS.txt
RELEASE_NOTES.txt
build.log
```

Suggested tag:

```text
vice3.1-frf-v41-multicpu
```

Suggested title:

```text
VICE68kRGB V41 MultiCPU (VICE 3.1)
```

No Commodore ROM images, games, disk/tape/cartridge images or snapshots are part of the release.