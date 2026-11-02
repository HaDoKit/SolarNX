[Русский](usage.md) | **English**

# Usage

## First connection

1. Start SolarNX on the Switch. The console's IP address is shown at the bottom of the menu
   (`IP Address: …`).

   ![Menu on the Switch](images/client/menu.jpg)

2. On the PC, open SolarNX, enter that address in the **«IP клиента»** (Client IP) field and click
   **«▶ Включить трансляцию»** (Start streaming). The address is remembered.

   ![SolarNX on the PC](images/app/home.png)

3. On the Switch, choose what to play (see below). The PC starts the right app, and the picture
   appears on the Switch within a few seconds.

The status under the button shows what's happening: «выключено» (off), «ожидание подключения
клиента…» (waiting for the client: the PC is waiting for the Switch) or «клиент подключён, идёт
трансляция» (client connected, streaming), with the mode name.

## Modes

The Switch menu has three tiles:

- **Steam** opens Steam Big Picture on the PC. From there it's regular Big Picture: library, store,
  launching games. After you quit a game, Steam returns to Big Picture by itself.
- **shadPS4** starts the PlayStation 4 emulator in its gamepad mode. The path to `shadPS4.exe` is set
  in «Настройки» (Settings) → «shadPS4» (SolarNX tries to find it by itself). In this mode the gamepad
  is a virtual DualShock 4 with gyro.
- **My games** is your own game library with covers. How to fill it is described in
  [Game library](library.en.md). When the game closes on the PC, the Switch returns to the menu.

## Controls

- The Switch buttons, sticks and triggers work as an Xbox 360 gamepad (or a DualShock 4, in shadPS4
  and in library games where it's selected). The ZL/ZR triggers are digital: pressed or released.
- You can connect up to 4 gamepads; each one becomes a separate virtual controller.
- The Switch **touch screen** works as touch on the PC screen (in library games, on the game window).
- **Gyro** is only sent in DualShock 4 mode.

### Gestures

The Switch system keeps the HOME and Capture buttons for itself, so SolarNX uses gestures instead:

| Gesture | Action |
|---|---|
| **−** and **+** together | Steam (Guide) button: the Steam menu in Big Picture; on DualShock 4, the PS button |
| hold **− for 3 seconds** | back to the SolarNX menu on the Switch. On the PC, the running game (and Big Picture) is closed |

A single − or + press is delayed by up to 0.1 seconds, while SolarNX waits to see whether the second
button follows.

## PC settings

The gear button on the SolarNX main screen.

![Settings](images/app/settings.png)

| Setting | What it does |
|---|---|
| Resolution | size of the streamed picture. The Switch screen is 1280×720; more is usually unnecessary |
| Frames per second | 60 is smoother; 30 halves the network load and gives a sharper picture at the same bitrate |
| Bitrate (kbps) | video quality. Higher is sharper but harder on Wi-Fi. Lower it if you see artifacts |
| Adaptive bitrate | SolarNX lowers the bitrate when the connection has problems and raises it back once things are stable. The Bitrate value becomes the upper limit |
| Encoder | Auto (recommended): SolarNX picks the encoder for your graphics card itself. The line under the list shows the detected graphics card and the encoder that will be used. Choose one by hand only if the automatic one causes problems, such as artifacts. The AMD and Intel encoders are experimental. Takes effect the next time the Switch connects |
| Switch layout (A↔B, X↔Y) | swaps A/B and X/Y so the buttons match the labels on the Switch |
| shadPS4 | path to the emulator |
| Drivers | reinstalls ViGEmBus |

The **«Показать экран»** (Show screen) toggle on the main window shows a preview of what the Switch
sees.

## Stopping

- On the Switch: hold − for 3 seconds to return to the menu, or close the app with HOME.
- On the PC: «Выключить трансляцию» (Stop streaming). If a game was running, SolarNX closes it and
  exits Big Picture.
