[Русский](CHANGELOG.md) | **English**

# Changelog

Versions follow [Semantic Versioning](https://semver.org/). While the project is in alpha
(`0.x.y-alpha`), settings, the library and the protocol are not guaranteed to be compatible between
versions: install the same version on the PC and on the Switch.

## [0.2.5-alpha] - 2026-11-03

- Rumble: the game's rumble reaches the Joy-Cons or the Pro Controller. Turn it off on the PC in
  Settings → "Rumble on the Switch", or on the Switch itself in the X menu → "Rumble" (all motors,
  no small motor, or off).
- Update check: on start and once a day SolarNX looks for a new version and says whether the Switch
  app needs updating too. Nothing is downloaded automatically. Turn it off in Settings → Updates,
  which also has a "Check now" button.
- The PC and the Switch tell each other their versions. If the Switch app is incompatible with the PC
  app, streaming doesn't start and the home screen says what to update. Small differences don't get
  in the way: the 0.2.4 Switch app keeps working with the 0.2.5 PC app, just without the new
  features.
- "Traffic priority" (on by default): video, audio and input are marked as urgent, and a router with
  WMM support lets them ahead of other traffic. If Wi-Fi got worse, turn it off in Settings.
- The NVENC preset is adaptive by default: streaming starts at the best quality (p7, hq) and the
  preset steps down if the graphics card can't keep up.
- Debug mode (Settings → Debug): the Switch shows statistics over the video - bitrate, losses,
  latency. The Switch's debug menu (Minus + Plus + ZL + ZR together) changes the bitrate, codec,
  resolution, frame rate and NVENC preset on the fly.
- Playing over the internet is steadier: more redundancy (about 40%), frames are sent evenly instead
  of in bursts, the Switch copes with packets arriving out of order, and the sound no longer falls
  behind after the connection hiccups.
- Fixed: in internet mode, after a game exited or crashed SolarNX stopped launching games until it
  was restarted.

## [0.2.4-alpha] - 2026-11-03

- Video is now H.265 (HEVC): at the same bitrate the picture is cleaner than with H.264. If the
  graphics card can't encode H.265, SolarNX falls back to H.264 by itself. The codec can be chosen in
  Settings -> Codec. Encoding on the CPU is always H.264.
- By default the resolution follows the Switch screen: 1280×720 handheld, 1920×1080 docked. If you
  put the Switch in the dock or take it out mid-game, the picture freezes for about a second and goes
  on at the new resolution. For 1080p raise the bitrate, about twice as much. A fixed resolution can
  be chosen in Settings -> Resolution.
- The video format changed: install 0.2.4 on both the PC and the Switch.

## [0.2.3-alpha] - 2026-11-02

- Video is now protected against packet loss: each frame gets extra packets (a Reed–Solomon code,
  about 20%), and the Switch rebuilds lost ones without asking the PC again. On Wi-Fi with random
  losses of up to a few percent the picture stays clean.
- If a frame still can't be rebuilt, instead of smeared artifacts ("water on glass") the last whole
  frame stays on screen for a moment - until the next keyframe, at most half a second.
- Video connects faster: the Switch no longer analyzes the stream before it starts showing it.
- Lower input lag: the Switch sends the buttons and sticks about 120 times per second instead of 43.
- Every 10 seconds the PC log shows how many packets the Switch rebuilt and how many frames it lost;
  frequent frame losses now count towards adaptive bitrate.
- The video format changed: install 0.2.3 on both the PC and the Switch.

## [0.2.2-alpha] - 2026-11-02

No new features: on GitHub, versions 0.2.0-alpha and 0.2.1-alpha came out without files. This version
contains everything from 0.2.0 and 0.2.1 (details in [CHANGELOG.en.md](https://github.com/HaDoKit/SolarNX/blob/master/CHANGELOG.en.md)): the installer, the Switch app and the rendezvous
server.

## [0.2.1-alpha] - 2026-11-02

- The PC app now speaks two languages, Russian and English: Settings → Language. By default it follows
  Windows; it changes at once, no restart needed.
- The installer is bilingual too.
- The Switch app now also speaks Russian: the language comes from the console's settings and can be
  changed in the main menu, X → Language. The Switch now shows Cyrillic, including in game names
  from the library.
- The X screen on the Switch is renamed Settings: it now holds the language choice too.

## [0.2.0-alpha] - 2026-11-02

- The project is renamed: **OrbitNX → SolarNX**. The PC app is `SolarNX.exe`, the Switch app is
  `/switch/SolarNX/SolarNX-Switch.nro`.
- **Experimental:** Internet mode - the Switch connects to the PC over the internet on a single UDP port,
  19010: through your own rendezvous server or with the port forwarded by hand. Only a paired Switch can connect:
  pairing is done once, comparing a 6-digit code on both screens. All traffic is encrypted (X25519,
  AES-256, HMAC-SHA256). Wi-Fi mode is unchanged.
- **Experimental:** the SolarNX Rendezvous server for your own VPS (Linux, Windows, Docker): the PC and
  the Switch find each other by ID and connect directly without port forwarding; if needed, the server
  relays the encrypted traffic itself.
- Removed the old unused shadPS4 game-picker channel.
- Library games now start as an ordinary user rather than with SolarNX's own administrator rights. Games
  that need administrator rights have a «Запускать от имени администратора» (Run as administrator)
  checkbox in the game editor.
- The A/B and X/Y layout can be switched on the Switch itself, with Y in the main menu. The setting
  syncs with the PC; the last change wins.
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

[0.2.3-alpha]: https://github.com/HaDoKit/SolarNX/releases/tag/v0.2.3-alpha
[0.2.2-alpha]: https://github.com/HaDoKit/SolarNX/releases/tag/v0.2.2-alpha
[0.2.1-alpha]: https://github.com/HaDoKit/SolarNX/releases/tag/v0.2.1-alpha
[0.2.0-alpha]: https://github.com/HaDoKit/SolarNX/releases/tag/v0.2.0-alpha
[0.1.0-alpha]: https://github.com/HaDoKit/SolarNX/releases/tag/v0.1.0-alpha
