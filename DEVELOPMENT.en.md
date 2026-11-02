[Русский](DEVELOPMENT.md) | **English**

# Developing SolarNX

How to build SolarNX from source and test changes on the console. For how the system
works internally, see [docs/architecture.en.md](docs/architecture.en.md).

## Repository layout

| Folder | Contents |
|---|---|
| `SolarNX/` | PC app: C#, .NET 10, WPF |
| `SolarNX/lib/` | bundled prebuilt tools: `ffmpeg.exe`, `vigembus-setup.exe` |
| `SolarNX/Native/TouchHelper/` | small C helper for touch input (`touchhelper.c` and the built `touchhelper.exe`) |
| `SolarNX/scripts/`, `SolarNX/installer.iss` | installer build |
| `Client/` | Switch app: C, devkitPro / libnx |
| `Client/tools/` | forwarder build and Switch log viewer over nxlink |
| `docs/` | user documentation |
| `.claude/skills/` | detailed architecture notes for both parts (see [below](#before-changing-code)) |

## Prerequisites

**PC app**
- Windows 10 1903 or later (Windows 11 recommended).
- [.NET 10 SDK](https://dotnet.microsoft.com/download).
- For the installer: [Inno Setup 6](https://jrsoftware.org/isdl.php).

**Switch client**
- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `switch-dev` and these libraries:
  ```
  pacman -S switch-dev switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-sdl2_gfx switch-ffmpeg switch-libopus switch-opusfile
  ```
- A Switch running the Homebrew Menu, for testing on the console.

## PC app

### Build and run

```
dotnet build SolarNX/SolarNX.csproj -c Release
```

The output is `SolarNX/bin/Release/net10.0-windows10.0.18362.0/SolarNX.exe`. The build copies
`ffmpeg.exe`, `vigembus-setup.exe` and `touchhelper.exe` next to it.

- Settings (`config.json`), the library (`library.json`, `covers/`) and the log (`SolarNX.log`) are
  stored next to the exe. Log timestamps use `HH:mm:ss.fff`, the same format as the Switch log.
- Virtual gamepads need the ViGEmBus driver. If it is missing, you can install it from the app's
  settings («Драйверы», Drivers).
- Only one instance of SolarNX can run at a time.

### Bundled tools in `SolarNX/lib/`

- **`ffmpeg.exe`**: the "essentials" build from [gyan.dev](https://www.gyan.dev/ffmpeg/builds/),
  currently 8.1. It must be 8.0 or later, because that release added the `gfxcapture` filter
  (Windows.Graphics.Capture) that capture relies on. After updating it, check both monitor capture
  and game-window capture.
- **`vigembus-setup.exe`**: the [ViGEmBus](https://github.com/nefarius/ViGEmBus) driver installer.

When you replace or add third-party components, update
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

### TouchHelper

Touches from the Switch screen are performed by a separate program, `touchhelper.exe`. Calling
`InjectTouchInput` directly from .NET does not work; the top of `touchhelper.c` explains why. The
built exe is checked in. If you change the source, rebuild it with MinGW:

```
gcc -O2 SolarNX/Native/TouchHelper/touchhelper.c -o SolarNX/Native/TouchHelper/touchhelper.exe
```

### Installer

```
powershell -ExecutionPolicy Bypass -File SolarNX/scripts/build-installer.ps1
```

The script runs `dotnet publish` (win-x64, self-contained), then compiles `installer.iss` with
`ISCC.exe`, which it looks for on PATH and in Inno Setup's default install folder. The output is
`SolarNX/Setup/SolarNX-Setup.exe`. The installer installs the ViGEmBus driver if it is missing and
places THIRD-PARTY-NOTICES.md next to the app.

## Switch client

### Build

```
make -C Client            # SolarNX-Switch.nro
make -C Client DEBUG=1    # SolarNX-Switch-debug.nro: -Og and logging over nxlink
```

The release and debug builds keep their object files apart (`build/` and `build_debug/`), so they
don't interfere with each other.

> **Paths with spaces.** devkitPro's make fails when the path to `Client/` contains spaces. There are
> three workarounds: build from a copy whose path has no spaces, map the folder to a drive
> (`subst Y: <path>\Client`, then `make -C /y`), or pass the path in `/c/...` form.

On Windows, run make from devkitPro's shell, for example
`C:\devkitPro\msys2\usr\bin\bash.exe -lc "make -C /c/path/to/Client"`. That way `DEVKITPRO` is
guaranteed to be set.

### Testing on the console

1. Open the Homebrew Menu on the Switch.
2. Send the app: `nxlink -a <Switch IP> Client/SolarNX-Switch.nro`.

The release build writes no log at all. To see the log, use the debug build:

```
powershell -ExecutionPolicy Bypass -File Client/tools/nxlink-log.ps1 -SwitchIp <Switch IP> -Nro Client/SolarNX-Switch-debug.nro -LogDir Client/logs
```

The script sends the `.nro`, accepts the app's log connection itself, prints each line and appends it
to `Client/logs/nxlink-<time>.log`. Don't replace it with `nxlink -s` redirected to a file: the log
then stops right after startup.

### HOME menu tile (forwarder)

`Client/tools/build-forwarder.ps1` builds `Client/SolarNX-Forwarder.nsp`. It is an installable
title: it adds an SolarNX tile to the Switch HOME menu and launches
`sdmc:/switch/SolarNX/SolarNX-Switch.nro`.

1. Build the client and copy `SolarNX-Switch.nro` to `/switch/SolarNX/` on the SD card.
2. Install [nton](https://pypi.org/project/nton/): `python -m pip install nton`.
3. Put **your own** console's keys in `%USERPROFILE%\.switch\prod.keys`. Never commit the keys or
   share them with anyone; `prod.keys` and `title.keys` are already in `.gitignore`.
4. `powershell -ExecutionPolicy Bypass -File Client/tools/build-forwarder.ps1`
5. Install the `.nsp` on the console with DBI or a similar tool.

The Title ID is pinned (`01E7E36C3EA40000`), so a rebuilt forwarder updates the tile that is already
installed. The forwarder only launches the `.nro`: to update the app, just copy a new `.nro`; there's
no need to reinstall the tile. Launched from the tile, the app runs as a full application rather than
the album applet, so it gets all the available memory.

## Before changing code

- `.claude/skills/` documents both parts: `solarnx-pc-architect` (PC) and `solarnx-switch-client`
  (Switch). It also lists approaches that were already tried and rejected. Read the section for the
  part you're changing so you don't repeat a dead end.
  - `solarnx-pc-architect/references/`: `switch-protocol.md`, `ffmpeg-pipeline.md`,
    `steam-bigpicture.md`, `native-interop.md`.
  - `solarnx-switch-client/references/`: `network-protocol.md`, `decode-pipeline.md`,
    `audio-pipeline.md`, `rendering-input.md`, `platform-constraints.md`.
- **The protocol is defined in code twice**: in C# in `SolarNX/` and in C in `Client/`. Change both
  sides in the same commit.
- SolarNX must not inject into game processes or read their memory; this matters for games with
  anti-cheat. Processes are opened with minimal access rights only.
- **Documentation** is kept in two languages: the Russian version is `X.md` and the English one is
  `X.en.md` in the same folder. When you change one, update the other too.

## License

[GPL-3.0](LICENSE). Third-party licenses are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
