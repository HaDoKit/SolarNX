#include "lang.h"

#include <switch.h>

#include "../logger/logger.h"
#include "../../settings/settings.h"

static bool s_consoleRussian = false;

static const char *const kEnglish[Str_Count] = {
    [Str_MyGames] = "My games",
    [Str_LayoutSwitch] = "Y  Layout: Switch (A = A)",
    [Str_LayoutXbox] = "Y  Layout: Xbox (by position)",
    [Str_MenuSettingsWifi] = "X  Settings: Wi-Fi",
    [Str_MenuInternetPrefix] = "X  Internet: ",
    [Str_IpAddressPrefix] = "IP Address: ",

    [Str_SettingsTitle] = "Settings",
    [Str_RowMode] = "Mode",
    [Str_RowAddress] = "PC address (no server)",
    [Str_RowServer] = "Server (optional)",
    [Str_RowServerKey] = "Server key",
    [Str_RowPcId] = "PC ID (on the server)",
    [Str_RowPair] = "Pair with this PC",
    [Str_RowLanguage] = "Language",
    [Str_ModeInternet] = "Internet (experimental)",
    [Str_ModeWifi] = "Wi-Fi (local network)",
    [Str_NotSet] = "(not set)",
    [Str_NoServer] = "(none - direct)",
    [Str_KeySet] = "(set)",
    [Str_KeyNone] = "(none)",
    [Str_PairedWithPrefix] = "paired with PC ",
    [Str_NotPaired] = "not paired",
    [Str_LanguageAuto] = "Auto (console language)",
    [Str_Note1] = "Without a server: forward UDP 19010 on the PC's router and enter its public address.",
    [Str_Note2] = "With your rendezvous server: no forwarding - enter the server and the PC ID from the PC.",
    [Str_Note3] = "Then pair once: \"Pair with this PC\" here and \"Pair Switch\" on the PC.",
    [Str_RestartNote] = "Restart SolarNX to switch to the new mode.",
    [Str_StatusPrefix] = "Status: ",
    [Str_HintPair] = "A  Pair      B  Back",
    [Str_HintChange] = "A  Change      B  Back",
    [Str_KbdAddress] = "PC address (IP or domain, optionally :port)",
    [Str_KbdServer] = "Rendezvous server (address or address:port, empty = none)",
    [Str_KbdServerKey] = "Server access key (empty = none)",
    [Str_KbdPcId] = "PC ID - shown in the PC's settings",

    [Str_LibWaiting] = "Waiting for the library from your PC...",
    [Str_LibEmpty] = "Your library is empty",
    [Str_LibAddHint] = "Add games in SolarNX on the PC (Game library button) with streaming enabled",
    [Str_HintPlay] = "A  Play      B  Back",
    [Str_HintBack] = "B  Back",

    [Str_StatusOff] = "off",
    [Str_StatusNoAddress] = "no PC address set",
    [Str_StatusBadAddress] = "PC address not found",
    [Str_StatusNotPaired] = "not paired - X > Pair",
    [Str_StatusWaiting] = "waiting for the PC",
    [Str_StatusConnected] = "connected (encrypted)",
    [Str_StatusBusy] = "PC is busy with another Switch",
    [Str_StatusWrongVersion] = "PC runs another SolarNX version",
    [Str_StatusPairing] = "pairing - waiting for the PC",
    [Str_StatusPairingClosed] = "pairing - open it on the PC first",
    [Str_StatusPairingCode] = "pairing - compare the code on the PC",
    [Str_StatusPaired] = "paired",
    [Str_StatusPairingRefused] = "pairing refused on the PC",
    [Str_StatusPairingFailed] = "pairing failed",
    [Str_StatusServerNotFound] = "server address not found",
    [Str_StatusServerNoAnswer] = "the server doesn't answer",
    [Str_StatusServerBadKey] = "the server refused the key",
    [Str_StatusServerNoPc] = "PC not on the server (is streaming on?)",
    [Str_StatusNoPcId] = "set the PC ID (shown on the PC)",
};

static const char *const kRussian[Str_Count] = {
    [Str_MyGames] = "Мои игры",
    [Str_LayoutSwitch] = "Y  Раскладка: Switch (A = A)",
    [Str_LayoutXbox] = "Y  Раскладка: Xbox (по положению)",
    [Str_MenuSettingsWifi] = "X  Настройки: Wi-Fi",
    [Str_MenuInternetPrefix] = "X  Интернет: ",
    [Str_IpAddressPrefix] = "IP-адрес: ",

    [Str_SettingsTitle] = "Настройки",
    [Str_RowMode] = "Режим",
    [Str_RowAddress] = "Адрес ПК (без сервера)",
    [Str_RowServer] = "Сервер (необязательно)",
    [Str_RowServerKey] = "Ключ сервера",
    [Str_RowPcId] = "ID ПК (на сервере)",
    [Str_RowPair] = "Привязать к этому ПК",
    [Str_RowLanguage] = "Язык",
    [Str_ModeInternet] = "Интернет (экспериментально)",
    [Str_ModeWifi] = "Wi-Fi (локальная сеть)",
    [Str_NotSet] = "(не задан)",
    [Str_NoServer] = "(нет — напрямую)",
    [Str_KeySet] = "(задан)",
    [Str_KeyNone] = "(нет)",
    [Str_PairedWithPrefix] = "привязан к ПК ",
    [Str_NotPaired] = "не привязан",
    [Str_LanguageAuto] = "Авто (язык консоли)",
    [Str_Note1] = "Без сервера: пробросьте UDP 19010 на роутере ПК и введите его внешний адрес.",
    [Str_Note2] = "С сервером-посредником проброс не нужен: введите сервер и ID ПК с компьютера.",
    [Str_Note3] = "Затем один раз привяжите: «Привязать к этому ПК» здесь и «Привязать Switch» на ПК.",
    [Str_RestartNote] = "Перезапустите SolarNX, чтобы сменить режим.",
    [Str_StatusPrefix] = "Статус: ",
    [Str_HintPair] = "A  Привязать      B  Назад",
    [Str_HintChange] = "A  Изменить      B  Назад",
    [Str_KbdAddress] = "Адрес ПК (IP или домен, можно с :портом)",
    [Str_KbdServer] = "Сервер-посредник (адрес или адрес:порт, пусто — без сервера)",
    [Str_KbdServerKey] = "Ключ доступа к серверу (пусто — без ключа)",
    [Str_KbdPcId] = "ID ПК — указан в настройках на ПК",

    [Str_LibWaiting] = "Ждём библиотеку с ПК...",
    [Str_LibEmpty] = "Библиотека пуста",
    [Str_LibAddHint] = "Добавьте игры в SolarNX на ПК (кнопка «Библиотека игр») при включённой трансляции",
    [Str_HintPlay] = "A  Играть      B  Назад",
    [Str_HintBack] = "B  Назад",

    [Str_StatusOff] = "выключен",
    [Str_StatusNoAddress] = "не задан адрес ПК",
    [Str_StatusBadAddress] = "адрес ПК не найден",
    [Str_StatusNotPaired] = "не привязан — X > Привязать",
    [Str_StatusWaiting] = "ожидание ПК",
    [Str_StatusConnected] = "подключено (зашифровано)",
    [Str_StatusBusy] = "ПК занят другим Switch",
    [Str_StatusWrongVersion] = "на ПК другая версия SolarNX",
    [Str_StatusPairing] = "привязка — ожидание ПК",
    [Str_StatusPairingClosed] = "привязка — сначала откройте её на ПК",
    [Str_StatusPairingCode] = "привязка — сверьте код на ПК",
    [Str_StatusPaired] = "привязан",
    [Str_StatusPairingRefused] = "привязка отклонена на ПК",
    [Str_StatusPairingFailed] = "привязка не удалась",
    [Str_StatusServerNotFound] = "адрес сервера не найден",
    [Str_StatusServerNoAnswer] = "сервер не отвечает",
    [Str_StatusServerBadKey] = "сервер не принял ключ",
    [Str_StatusServerNoPc] = "ПК нет на сервере (трансляция включена?)",
    [Str_StatusNoPcId] = "укажите ID ПК (он показан на ПК)",
};

void langInit(void)
{
    if (R_FAILED(setInitialize()))
        return;
    u64 code = 0;
    SetLanguage language;
    if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &language)))
        s_consoleRussian = language == SetLanguage_RU;
    setExit();
    logWrite("lang", "console language: %s", s_consoleRussian ? "Russian" : "not Russian - English");
}

const char *tr(StrId id)
{
    if (id < 0 || id >= Str_Count)
        return "";
    SettingsLanguage chosen = settingsGetLanguage();
    bool russian = chosen == SettingsLanguage_Russian || (chosen == SettingsLanguage_Auto && s_consoleRussian);
    const char *text = russian ? kRussian[id] : kEnglish[id];
    return text != NULL ? text : kEnglish[id];
}
