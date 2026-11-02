[Русский](library.md) | **English**

# Game library

Your own library lets you launch any PC game from the Switch, not only through Steam. The games appear
on the Switch, with covers, in the **My games** tile.

## Adding a game

On the SolarNX main window, click the gamepad button (next to settings) → **«Добавить игру»**
(Add game).

![Game library](images/app/library.png)

![Game dialog](images/app/game-edit-steam.png)

### Launch methods

| Method | What to specify | How it starts |
|---|---|---|
| **Regular game (exe)** | the game file and, if needed, arguments and working folder | directly, without Steam |
| **Steam game** | pick from the list of installed games or enter the AppID | through Steam (`steam://rungameid/…`), with your Steam Input settings |
| **Emulator** | the emulator file, the game file (ROM) and arguments | the emulator with your game. In the arguments, `{rom}` is replaced with the game path, e.g. `-f -g "{rom}"` |
| **Custom script (cmd)** | commands, one per line | through `cmd.exe`, for anything the other methods don't cover |

If the game starts through a launcher (the launcher opens the game and then closes itself), that's
fine: SolarNX also tracks processes started by the launcher.

### Gamepad

- **Xbox 360** suits most games.
- **DualShock 4 (with gyro)**, for when the game or emulator needs gyro.

### Picture

The **«Показывать на Switch только окно игры»** (Show only the game window on the Switch) checkbox is
on by default: only the game window is sent to the Switch, stretched to the whole console screen. The
game fills the Switch screen even if it runs in a small window on the PC, and other windows don't
cover it.

Clear the checkbox if the picture is black; this happens with some games in exclusive fullscreen
mode. The whole primary monitor is then sent instead.

### Rights

The **«Запускать от имени администратора»** (Run as administrator) checkbox is off by default. Without it
the game starts as an ordinary user, the same as a double-click in Explorer, even though SolarNX itself
runs as administrator. Turn it on for games and programs that need administrator rights. A game that
itself requires administrator rights gets them either way. Steam games don't have the checkbox: Steam
starts them.

If User Account Control (UAC) is turned off in Windows, every program runs as administrator anyway, and
the checkbox changes nothing.

## Covers

Click **«Выбрать…»** (Choose…) under the cover and pick any image. On the Switch, covers are shown
in a 2:3 ratio (like Steam library covers). For Steam games the cover is filled in automatically.

If the image has a different shape, the **«Кадр обложки»** (Cover framing) window opens: drag the
frame with the mouse and change its size with the mouse wheel or the slider. The right side shows how
the cover will look on the Switch. To return to the framing later, use the **«Кадр…»** (Frame…)
button.

![Cover framing](images/app/cover-crop.png)

This is how the library looks on the Switch:

![Library on the Switch](images/client/my-games.jpg)

## How it works during play

- Library changes reach the Switch automatically while streaming is on, even if the Switch is in the
  menu at that moment.
- When a game starts, SolarNX gives its window focus so that input goes to the game. If the game
  switches windows (for example, launcher → the game itself), focus and capture move to the new
  window.
- When the game closes on the PC, the Switch returns to the menu.
- If you go back to the menu on the Switch (by holding − for 3 seconds), SolarNX closes the game on
  the PC.

The library is stored in `library.json` next to SolarNX, and covers in the `covers` folder.
