# Future Retro Fusion Contributions

This document identifies Future Retro Fusion work in this VICE 3.1 AmigaOS
branch. It is an attribution and provenance record; it does not replace the
licence notices in individual source files.

## AmigaOS / PiStorm integration

FRF development on this branch includes work in the following areas:

- AmigaOS m68k and accelerated-Amiga/PiStorm/Emu68 integration.
- Picasso96/RTG display-path optimisation and fullscreen handling.
- Native low-depth Amiga RGB output.
- AHI audio-path optimisation and stability work.
- AmigaOS menu, input, resource and shutdown safeguards.
- M68020/M68881 and M68060 hard-float build/release profiles.

## Proven native RGB engine

The published branch contains the V37 native RGB engine marker:

```text
FRF_PAL_RGB_V37_MAME_BULK_C2P
```

The V37 work includes:

- direct native planar publication for low-depth Amiga screens;
- indexed Fast-RAM staging;
- 16-pixel chunky-to-planar word conversion;
- four-plane publication for 16-colour RGB output;
- changed-word shadow comparison to reduce Chip-RAM writes;
- contiguous changed-run publication;
- support for standard and interleaved planar bitmap layouts;
- native-refresh ownership separated from the RTG/P96 path.

The rejected V38 fit-to-screen / EHB experiment is not part of this published
source snapshot.

## Menu-safe native RGB

This branch also contains the V39 menu-safe marker:

```text
FRF_PAL_RGB_V39_MENU_SAFE
```

That work protects Intuition menus from the direct-planar renderer and aligns
menu layout with the actual native screen rather than an unrelated public
screen.

## Earlier FRF AmigaOS work represented by this branch

The wider FRF VICE 3.1 AmigaOS work has also included:

- native-format Picasso96 dirty-rectangle rendering;
- direct signed 16-bit AHI output;
- corrected AHI timing/buffer behaviour and safer shutdown;
- practical FastSID/default configuration tuning for accelerated Amiga use;
- corrected AmigaOS menu/fullscreen/resource behaviour;
- PRG RAM-injection and drive/default tuning;
- null-safe network/shared-library shutdown handling;
- release audits, source packaging and processor-specific build profiles.

The source itself remains the authority for the exact implementation present in
any particular commit.

## Licence of FRF contributions

FRF modifications to existing VICE files are licensed under the same terms as
those files. FRF-only source/documentation without a more specific notice is
made available under **GPL-2.0-or-later**.

Copyright © 2026 Future Retro Fusion contributors for original FRF-authored
material.
