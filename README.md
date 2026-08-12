# VICE68kRGB

**VICE68kRGB** is a Future Retro Fusion branch of **VICE 3.1** for AmigaOS m68k,
with a focus on accelerated Amigas, PiStorm/Emu68, RTG/Picasso96 and native
classic-Amiga RGB output.

## What this repository contains

- Complete modified VICE source selected for this FRF branch.
- AmigaOS RGB/RTG and AHI changes.
- Source-level provenance and FRF contribution documentation.
- Upstream VICE copyright/licence material.
- Build guidance for AmigaOS m68k cross-compilation.

It intentionally does **not** include:

- Commodore ROM images;
- games or application software;
- disk/tape/cartridge images;
- VICE snapshots;
- personal configuration;
- local compiler output, logs or recovery snapshots.

## Current stable RGB baseline

The public source is required to contain:

```text
FRF_PAL_RGB_V37_MAME_BULK_C2P
```

The rejected V38 scaling/EHB experiment is explicitly blocked by the
publication audit.

Optional later FRF generations such as V39 menu protection and V40 direct RGB
launch are documented only when their source markers are actually detected.

## Licensing

VICE is distributed under the **GNU General Public License version 2 or, at
your option, any later version**. The repository preserves upstream `COPYING`
and individual source-file notices.

FRF modifications to VICE files are supplied under the same licence terms as
the files modified.

See:

- [`LICENSE.md`](LICENSE.md)
- [`COPYING`](COPYING)
- [`NOTICE.md`](NOTICE.md)
- [`FRF_CONTRIBUTIONS.md`](FRF_CONTRIBUTIONS.md)

## Building

See [`docs/BUILD_AMIGA.md`](docs/BUILD_AMIGA.md).

## Credits

VICE: VICE development team and contributors.

AmigaOS/PiStorm integration, native RGB/RTG work, AHI work, interface fixes,
validation and FRF release engineering: **Future Retro Fusion**.
