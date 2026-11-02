[Русский](README.md) | **English**

# SolarNX

> **Alpha version 0.2.2-alpha.** The project is at an early stage: expect bugs, and settings and the
> protocol may change between versions. Stable operation has so far been confirmed on a single setup
> only; see [Tested configuration](#tested-configuration). For what changed, see
> [CHANGELOG.en.md](CHANGELOG.en.md).

Stream games from a Windows PC to a Nintendo Switch over your local network: video and audio go
from the PC to the console, and input from the Joy-Con, Pro Controller and touch screen goes back to
the PC.

The project has two parts:

- **SolarNX**: the Windows app (`SolarNX/`). It launches the game, captures video and audio, sends
  them to the Switch, and turns button presses into a virtual gamepad.
- **SolarNX for Switch**: a homebrew app (`Client/`). It shows the stream and sends input.

![SolarNX on the Switch](docs/images/client/menu.jpg)

## Features

- **Steam Big Picture**: all of your Steam library, driven by a gamepad.
- **shadPS4**: the PlayStation 4 emulator, with gyro through a virtual DualShock 4.
- **Your own game library**: regular games (`.exe`), Steam games, emulators and custom launch
  scripts, with covers in the Switch menu.
- Up to 4 gamepads; the Switch touch screen works as touch input on the PC.
- Hardware encoding on NVIDIA, AMD and Intel graphics cards (the encoder is picked automatically; AMD and Intel are experimental) and hardware decoding on the Switch.
- Steam button on the Switch: **− + +**. Back to the menu: hold **− for 3 seconds**.

> The PC app's interface is in Russian. The English docs quote its labels in Russian, with a
> translation in parentheses.

## Documentation

- [Features](docs/features.en.md): what the system looks like and everything SolarNX can do, with screenshots
- [Installation](docs/installation.en.md): what you need on the PC and on the Switch
- [Usage](docs/usage.en.md): connecting, modes, controls, settings
- [Game library](docs/library.en.md): your own games, covers, window capture
- [Troubleshooting](docs/troubleshooting.en.md): black screen, artifacts, no connection, logs
- [How it works](docs/architecture.en.md): components, network ports and protocol

For developers, building from source: [DEVELOPMENT.en.md](DEVELOPMENT.en.md).

## Tested configuration

Stable operation of version 0.2.0-alpha is confirmed on this setup. SolarNX should work on other
hardware too, but it hasn't been tested yet.

| | |
|---|---|
| **PC** | Windows 11 Pro 24H2 (build 26100) |
| CPU | AMD Ryzen 7 5800X |
| GPU | NVIDIA GeForce RTX 3080, driver 610.74 |
| Memory | 80 GB |
| PC network | wired (Ethernet) to the router |
| FFmpeg | 8.1.2 (bundled) |
| **Switch** | Nintendo Switch OLED |
| Switch network | 5 GHz Wi-Fi |

If SolarNX works (or doesn't) for you on other hardware, please report it in
[Issues](https://github.com/HaDoKit/SolarNX/issues); it helps grow this list.

## Development

This project was developed together with [Claude Code](https://claude.com/claude-code), Anthropic's
AI coding assistant.

## License

[GPL-3.0](LICENSE). Third-party components and their licenses are listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
