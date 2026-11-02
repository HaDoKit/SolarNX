[Русский](CHANGELOG.md) | **English**

# Changelog

Versions follow [Semantic Versioning](https://semver.org/). While the project is in alpha
(`0.x.y-alpha`), settings, the library and the protocol are not guaranteed to be compatible between
versions: install the same version on the PC and on the Switch.

## [Unreleased]

- The project is renamed: **OrbitNX → SolarNX**. The PC app is `SolarNX.exe`, the Switch app is
  `/switch/SolarNX/SolarNX-Switch.nro`.
- **Experimental:** hardware video encoding on AMD (AMF) and Intel (Quick Sync) graphics cards, and through Media
  Foundation on any graphics card - previously NVIDIA only. The encoder is picked automatically from the
  detected graphics card; the settings show the card and the chosen encoder, which can be changed by hand.
  Not tested on real AMD and Intel graphics cards yet.
- New "eclipse" style: icon, background and the glowing SOLARNX wordmark on the PC and the Switch,
  gold accent in the PC interface. The version number on the Switch moved to the bottom-left corner.
- The installer puts SolarNX in its own folder, carries over the settings, library and covers from
  OrbitNX, and removes the old version.

## [0.1.0-alpha] - 2026-10-24

First public alpha.

**PC (OrbitNX)**
- Streams the primary monitor or a game window: Windows.Graphics.Capture, H.264 via NVENC
  (on the CPU without NVIDIA).
- Audio from the default output device, in Opus.
- Modes: Steam Big Picture, shadPS4, your own game library (exe, Steam, emulator, cmd script) with
  covers and cover framing.
- Up to 4 virtual Xbox 360 / DualShock 4 gamepads (ViGEmBus), touch, gyro.
- Adaptive bitrate, automatic reconnection.
- Installer with the ViGEmBus driver.

**Switch (OrbitNX for Switch)**
- Hardware decoding, always showing the newest frame.
- Mode menu and the My games cover grid.
- Steam button with − and +, back to the menu by holding − for 3 seconds.
- HOME menu tile (forwarder, built yourself).

[0.1.0-alpha]: https://github.com/HaDoKit/SolarNX/releases/tag/v0.1.0-alpha
