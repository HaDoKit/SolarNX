[Русский](installation.md) | **English**

# Installation

## Requirements

### PC

- Windows 10 version 1903 (build 18362) or later, 64-bit.
- An NVIDIA, AMD or Intel graphics card with an up-to-date driver, for hardware video encoding. SolarNX
  finds the graphics card and picks the encoder itself: NVENC (NVIDIA, driver 610 or later), AMF (AMD),
  Quick Sync (Intel), or Media Foundation for the rest. If none of them works, the CPU encodes the
  video (x264). That works, but puts a noticeable load on the CPU.

  > Encoding on **AMD and Intel graphics cards is an experimental feature**: it hasn't been tested on
  > real hardware yet. If there's no video or it has artifacts, choose the Media Foundation or CPU
  > encoder in the settings and let us know in [Issues](https://github.com/HaDoKit/SolarNX/issues).
- The **ViGEmBus** virtual gamepad driver. The SolarNX installer offers to install it for you.
- For Steam mode, Steam installed. For shadPS4 mode, [shadPS4](https://shadps4.net).

### Nintendo Switch

- A console with custom firmware (Atmosphère) and the Homebrew Menu.
- A connection to the same local network as the PC. 5 GHz Wi-Fi, or a PC wired to the router, works
  best: the picture quality depends directly on the network.

### Network

The PC and the Switch must be able to reach each other on the same network. The PC connects to the
Switch by its IP address, so you don't need to open any incoming ports on the PC. These ports are
used (details in [How it works](architecture.en.md)):

| Port | Protocol | Purpose |
|---|---|---|
| 19000 | UDP | video (PC → Switch) |
| 19001 | TCP | input (Switch → PC) |
| 19002 | UDP | audio (PC → Switch) |
| 19003 | TCP | game library (PC → Switch) |

## Installing on the PC

1. Download `SolarNX-Setup.exe` from the [releases](https://github.com/HaDoKit/SolarNX/releases) and run it.
2. At the drivers step, select **ViGEmBus** if it isn't installed yet.
3. After installation, start SolarNX. Closing the window minimizes it to the tray; to quit completely,
   use the tray icon menu → «Выход» (Exit).

You can reinstall the driver later: «Настройки» (Settings) → «Драйверы» (Drivers) →
«Переустановить драйвер геймпада» (Reinstall gamepad driver).

## Installing on the Switch

1. Download `SolarNX-Switch.nro` from the [releases](https://github.com/HaDoKit/SolarNX/releases).
2. Copy it to the Switch SD card into `/switch/SolarNX/`, so the path is
   `/switch/SolarNX/SolarNX-Switch.nro`.
3. Start SolarNX from the Homebrew Menu.

### HOME menu tile (optional)

To start SolarNX from the HOME menu instead of the Homebrew Menu, you can install a forwarder: a small
title that launches the `.nro` from the SD card. You build it on the PC for your own console, using
keys dumped from that console. Instructions are in
[DEVELOPMENT.en.md → HOME menu tile](../DEVELOPMENT.en.md#home-menu-tile-forwarder). Install the
resulting `SolarNX-Forwarder.nsp` with DBI.

> Install `SolarNX-Forwarder.nsp` specifically. The installer rejects other files with
> "Content meta not found".

The forwarder only launches the `.nro`, so to update SolarNX you just replace the `.nro` on the SD
card. There's no need to reinstall the tile.
