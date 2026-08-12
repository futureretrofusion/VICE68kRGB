# Building ViceRGB for AmigaOS m68k

ViceRGB is a VICE 3.1-derived AmigaOS source tree.

## Typical FRF compiler profiles

### M68020 / M68881

```text
-m68020 -m68881 -Ofast -fomit-frame-pointer
-fno-strict-aliasing -noixemul -finline-functions
```

### M68060 hard-float

```text
-m68060 -mhard-float -Ofast -fomit-frame-pointer
-fno-strict-aliasing -noixemul -finline-functions
```

## Requirements

A configured AmigaOS m68k GCC cross-toolchain plus the SDK/NDK and libraries
required by the selected VICE configuration.

The public repository keeps upstream configure/build-system source files such
as `configure`, `configure.ac` and `Makefile.in` where present. Machine-local
generated `Makefile`, object, dependency, archive and configure-result files are
excluded.

## Emulator frontends

Depending on configuration:

```text
x64
x64sc
x64dtv
xscpu64
x128
xvic
xplus4
xpet
xcbm2
xcbm5x0
vsid
```

## ROMs

ROM images are not distributed in this repository. Provide required firmware
separately from a source you are legally entitled to use.
