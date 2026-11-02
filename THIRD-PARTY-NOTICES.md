# Third-Party Notices

SolarNX uses and, in its distributed builds, includes the third-party software listed below. Each
component remains under its own license; these notices are reproduced as those licenses require.

## PC host (`SolarNX/`)

### Bundled executables

**FFmpeg** (`ffmpeg.exe`) - GPL-3.0. Unmodified "essentials" build from https://www.gyan.dev/ffmpeg/builds/,
configured with `--enable-gpl --enable-version3`. It runs as a separate program that SolarNX starts
for screen capture and video encoding. Copyright (c) the FFmpeg developers. Source code:
https://ffmpeg.org/download.html and https://github.com/GyanD/codexffmpeg (build scripts).
License text: https://www.gnu.org/licenses/gpl-3.0.html

**ViGEmBus** driver installer (`vigembus-setup.exe`) - BSD 3-Clause.
Copyright (c) Nefarius Software Solutions e.U. https://github.com/nefarius/ViGEmBus

### NuGet libraries

**Concentus** (Opus audio codec) - BSD 3-Clause. Copyright (c) Skype Limited, Xiph.Org Foundation,
CSIRO, Microsoft Corporation, Jean-Marc Valin, Gregory Maxwell, Mark Borgerding, Timothy B.
Terriberry, Logan Stromberg. Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the copyright notice, this list of conditions and the
following disclaimer are retained. Neither the name of Internet Society, IETF or IETF Trust may be
used to endorse or promote products derived from this software without specific prior written
permission. Provided "AS IS" without warranty of any kind. https://github.com/lostromb/concentus

**Nefarius.ViGEm.Client** - MIT. Copyright (c) Benjamin Höglinger-Stelzer (Nefarius).
https://github.com/nefarius/ViGEm.NET

**NAudio** - MIT. Copyright (c) Mark Heath, Ray Molenkamp and NAudio contributors.
https://github.com/naudio/NAudio

**WPF-UI** - MIT. Copyright (c) Leszek Pomianowski and WPF UI Contributors.
https://github.com/lepoco/wpfui

**Tomlyn** - BSD 2-Clause. Copyright (c) Alexandre Mutel. https://github.com/xoofx/Tomlyn

**System.Management** - MIT. Copyright (c) .NET Foundation and Contributors.
https://github.com/dotnet/runtime

**Microsoft.Windows.CsWin32** - MIT. Copyright (c) Microsoft Corporation. Build-time source
generator only; nothing from it ships at runtime. https://github.com/microsoft/CsWin32

## Nintendo Switch client (`Client/`)

Statically linked from the devkitPro toolchain's portlibs:

- **libnx** - ISC. Copyright (c) switchbrew. https://github.com/switchbrew/libnx
- **FFmpeg** (libavformat, libavcodec, libavutil, libswresample, libswscale) - built by devkitPro
  with `--enable-gpl` (GPL-2.0-or-later). Copyright (c) the FFmpeg developers.
  Source: https://ffmpeg.org/download.html and https://github.com/devkitPro/pacman-packages
- **SDL2**, **SDL2_ttf**, **SDL2_image** - zlib. Copyright (c) Sam Lantinga and contributors.
  https://github.com/libsdl-org
- **SDL2_gfx** - zlib. Copyright (c) Andreas Schiffler.
- **libopus**, **opusfile** - BSD 3-Clause. Copyright (c) Xiph.Org Foundation and contributors.
- **FreeType** - FreeType License (FTL). Portions of this software are copyright (c) The FreeType
  Project (https://freetype.org). All rights reserved.
- **HarfBuzz** - MIT ("Old MIT"). Copyright (c) HarfBuzz contributors.

Included as source (also ported to C# in `SolarNX/Session/InternetCrypto.cs`):

- **TweetNaCl** X25519 (`crypto_scalarmult`) - public domain. Daniel J. Bernstein, Bernard van Gastel,
  Wesley Janssen, Tanja Lange, Peter Schwabe, Sjaak Smetsers. https://tweetnacl.cr.yp.to
- **libpng** - libpng license; **zlib** - zlib; **libjpeg-turbo** - IJG / BSD 3-Clause;
  **libwebp** - BSD 3-Clause (Google).

Included as source:

- **SDL_FontCache** (`Client/common/fontcache/`) - MIT. Copyright (c) Jonathan Dearborn.
  https://github.com/grimfang4/SDL_FontCache
