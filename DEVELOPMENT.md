**Русский** | [English](DEVELOPMENT.en.md)

# Разработка SolarNX

Как собрать SolarNX из исходников и проверить изменения на консоли. Как система
устроена внутри — в [docs/architecture.md](docs/architecture.md).

## Структура репозитория

| Папка | Что внутри |
|---|---|
| `SolarNX/` | приложение для ПК: C#, .NET 10, WPF |
| `SolarNX/lib/` | готовые программы, которые идут в комплекте: `ffmpeg.exe`, `vigembus-setup.exe` |
| `SolarNX/Native/TouchHelper/` | маленькая программа на C для касаний (`touchhelper.c` и собранный `touchhelper.exe`) |
| `SolarNX/scripts/`, `SolarNX/installer.iss` | сборка установщика |
| `Client/` | приложение для Switch: C, devkitPro / libnx |
| `Client/tools/` | сборка форвардера и просмотр логов Switch через nxlink |
| `docs/` | документация для пользователей |
| `.claude/skills/` | подробные заметки по архитектуре обеих частей (см. [ниже](#прежде-чем-менять-код)) |

## Что нужно установить

**ПК-часть**
- Windows 10 1903 или новее (лучше Windows 11).
- [.NET 10 SDK](https://dotnet.microsoft.com/download).
- Для установщика — [Inno Setup 6](https://jrsoftware.org/isdl.php).

**Клиент для Switch**
- [devkitPro](https://devkitpro.org/wiki/Getting_Started) с `switch-dev` и библиотеками:
  ```
  pacman -S switch-dev switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-sdl2_gfx switch-ffmpeg switch-libopus switch-opusfile
  ```
- Switch с запущенным Homebrew Menu — для проверки на консоли.

## ПК-часть

### Сборка и запуск

```
dotnet build SolarNX/SolarNX.csproj -c Release
```

Результат — `SolarNX/bin/Release/net10.0-windows10.0.18362.0/SolarNX.exe`. При сборке рядом с ним
копируются `ffmpeg.exe`, `vigembus-setup.exe` и `touchhelper.exe`.

- Настройки (`config.json`), библиотека (`library.json`, `covers/`) и лог (`SolarNX.log`) хранятся
  рядом с exe. Время в логе — `HH:mm:ss.fff`, так же как в логе Switch.
- Для виртуальных геймпадов нужен драйвер ViGEmBus. Если он не установлен, его можно поставить из
  настроек приложения («Драйверы»).
- Одновременно может работать только одна копия SolarNX.

### Готовые программы в `SolarNX/lib/`

- **`ffmpeg.exe`** — сборка «essentials» с [gyan.dev](https://www.gyan.dev/ffmpeg/builds/), сейчас
  8.1. Нужна версия 8.0 или новее: в ней появился фильтр `gfxcapture` (Windows.Graphics.Capture),
  на котором держится захват. При обновлении проверьте захват монитора и окна игры.
- **`vigembus-setup.exe`** — установщик драйвера [ViGEmBus](https://github.com/nefarius/ViGEmBus).

При замене или добавлении сторонних компонентов обновите
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

### TouchHelper

Касания экрана Switch выполняет отдельная программа `touchhelper.exe`. Вызов `InjectTouchInput`
напрямую из .NET не работает, подробности — в начале `touchhelper.c`. Собранный exe лежит в
репозитории. Если вы изменили исходник, пересоберите его через MinGW:

```
gcc -O2 SolarNX/Native/TouchHelper/touchhelper.c -o SolarNX/Native/TouchHelper/touchhelper.exe
```

### Установщик

```
powershell -ExecutionPolicy Bypass -File SolarNX/scripts/build-installer.ps1
```

Скрипт выполняет `dotnet publish` (win-x64, self-contained), затем собирает `installer.iss` через
`ISCC.exe` — его он ищет в PATH и в стандартной папке установки Inno Setup. Результат —
`SolarNX/Setup/SolarNX-Setup.exe`. Установщик ставит драйвер ViGEmBus, если его нет, и кладёт рядом
с приложением THIRD-PARTY-NOTICES.md.

## Клиент для Switch

### Сборка

```
make -C Client            # SolarNX-Switch.nro
make -C Client DEBUG=1    # SolarNX-Switch-debug.nro: -Og и лог через nxlink
```

Обычная и отладочная сборки хранят объектные файлы раздельно (`build/` и `build_debug/`), поэтому
не мешают друг другу.

> **Путь с пробелами.** make из devkitPro не работает, если в пути к `Client/` есть пробелы.
> Есть три выхода: собирать из копии без пробелов в пути, подключить папку как диск
> (`subst Y: <путь>\Client`, затем `make -C /y`) или передавать путь в виде `/c/...`.

На Windows запускайте make из оболочки devkitPro, например
`C:\devkitPro\msys2\usr\bin\bash.exe -lc "make -C /c/path/to/Client"`. Так переменная `DEVKITPRO`
гарантированно задана.

### Проверка на консоли

1. Откройте Homebrew Menu на Switch.
2. Отправьте приложение: `nxlink -a <IP Switch> Client/SolarNX-Switch.nro`.

Обычная сборка ничего не пишет в лог. Чтобы видеть лог, используйте отладочную сборку:

```
powershell -ExecutionPolicy Bypass -File Client/tools/nxlink-log.ps1 -SwitchIp <IP Switch> -Nro Client/SolarNX-Switch-debug.nro -LogDir Client/logs
```

Скрипт отправляет `.nro`, сам принимает подключение лога от приложения, выводит строки на экран и
дописывает их в `Client/logs/nxlink-<время>.log`. Не заменяйте его на `nxlink -s` с выводом в
файл: в этом случае лог обрывается сразу после запуска.

### Плитка на главном экране (форвардер)

`Client/tools/build-forwarder.ps1` собирает `Client/SolarNX-Forwarder.nsp`. Это устанавливаемая
программа: она добавляет плитку SolarNX на главный экран Switch и запускает
`sdmc:/switch/SolarNX/SolarNX-Switch.nro`.

1. Соберите клиент и скопируйте `SolarNX-Switch.nro` на SD-карту в `/switch/SolarNX/`.
2. Установите [nton](https://pypi.org/project/nton/): `python -m pip install nton`.
3. Положите ключи **своей** консоли в `%USERPROFILE%\.switch\prod.keys`. Ключи нельзя коммитить и
   передавать кому-либо — `prod.keys` и `title.keys` уже есть в `.gitignore`.
4. `powershell -ExecutionPolicy Bypass -File Client/tools/build-forwarder.ps1`
5. Установите `.nsp` на консоль через DBI или похожую программу.

Title ID закреплён (`01E7E36C3EA40000`), поэтому новая сборка форвардера обновляет уже установленную
плитку. Форвардер только запускает `.nro`: чтобы обновить приложение, достаточно скопировать новый
`.nro`, переустанавливать плитку не нужно. Запущенное с плитки, приложение работает как полноценная
программа, а не как апплет альбома, и получает всю доступную память.

## Прежде чем менять код

- В `.claude/skills/` описаны обе части: `solarnx-pc-architect` (ПК) и `solarnx-switch-client`
  (Switch). Там же перечислены подходы, которые уже пробовали и отвергли, — прочитайте раздел про
  ту часть, которую меняете, чтобы не повторять тупиковые варианты.
  - `solarnx-pc-architect/references/`: `switch-protocol.md`, `ffmpeg-pipeline.md`,
    `steam-bigpicture.md`, `native-interop.md`.
  - `solarnx-switch-client/references/`: `network-protocol.md`, `decode-pipeline.md`,
    `audio-pipeline.md`, `rendering-input.md`, `platform-constraints.md`.
- **Протокол описан в коде дважды** — на C# в `SolarNX/` и на C в `Client/`. Меняйте обе стороны в
  одном коммите.
- SolarNX не должен внедряться в процессы игр и читать их память: это важно для игр с античитом.
  Процессы открываются только с минимальными правами.
- **Документация** ведётся на двух языках: русская версия — `X.md`, английская — `X.en.md` в той же
  папке. Меняя одну, обновите и другую.

## Лицензия

[GPL-3.0](LICENSE). Лицензии сторонних компонентов — в [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
