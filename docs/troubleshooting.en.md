[Русский](troubleshooting.md) | **English**

# Troubleshooting

## No connection

- Check that the **«IP клиента»** (Client IP) field contains the address shown at the bottom of the
  SolarNX menu on the Switch.
- The PC and the Switch must be on the same network. Guest networks and the router's "client
  isolation" mode stop devices from seeing each other.
- SolarNX must be open on the Switch (menu or game), and «Включить трансляцию» (Start streaming)
  must be clicked on the PC.
- After you pick a mode on the Switch, connecting can take a few seconds.

## Black screen on the Switch

- **The game is minimized on the PC.** A minimized window produces no picture; restore it.
- **A library game is in exclusive fullscreen mode.** In its settings, clear the
  «Показывать на Switch только окно игры» (Show only the game window on the Switch) checkbox, or
  switch the game to windowed or borderless mode.
- **The picture is already cropped or shifted on the PC itself.** This is often caused by Windows
  compatibility flags: in the game exe's properties → Compatibility → Change high DPI settings, clear
  "Override high DPI scaling behavior". If the game is fully visible on the PC, it will look the same
  on the Switch.
- The game is protected from screen capture. In Steam mode the log shows `EXCLUDED FROM CAPTURE`.
  There is no way around this.

## Artifacts, "smeared" picture

If the picture briefly "smears", like water running down glass, and then recovers, video packets are
being lost.

- Lower the **bitrate** or turn on **adaptive bitrate**.
- Set **30 frames per second**: at the same bitrate, each frame gets twice as much data.
- Use 5 GHz Wi-Fi and keep the Switch closer to the router. It's better to connect the PC to the
  router with a cable.

## Controls don't work in the game

- The game window must be active on the PC. SolarNX activates it at launch, but if you click another
  window on the PC, some games stop accepting the gamepad; click back into the game.
- Make sure the ViGEmBus driver is installed («Настройки» (Settings) → «Драйверы» (Drivers)).
- Buttons are swapped (A/B, X/Y): toggle the Switch layout option in the settings.

## Steam

- The Steam button is **−** and **+** pressed together.
- Big Picture comes back by itself after you quit a game. If it stops responding to input, hold − for
  3 seconds to go back to the menu, then choose Steam again.

## Audio

- Everything playing on the PC's default audio output device is sent.
- If there is no sound, check that the game plays audio on the default device.

## Logs

- **PC:** `SolarNX.log` next to `SolarNX.exe`. It is overwritten each time the program starts.
- **Switch:** the console app does not save logs.

When you report a problem, attach `SolarNX.log` and describe what you were doing at the time.
