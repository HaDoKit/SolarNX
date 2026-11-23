#include "strings.h"

#include <string.h>
#include <SDL2/SDL.h>

static const char *const kEnglish[Str_Count] = {
    [Str_MyGames] = "My games",
    [Str_SettingsTitle] = "Settings",
    [Str_HintPlay] = "A  Play      B  Back",
    [Str_HintBack] = "B  Back",
    [Str_HintChange] = "A  Change      B  Back",
    [Str_HintPair] = "A  Pair      B  Back",
    [Str_HintPlayTouch] = "Tap a game to play",
    [Str_HintChangeTouch] = "Tap a row to change",
    [Str_Back] = "Back",

    [Str_RowHostType] = "Host",
    [Str_RowAddress] = "PC address (no server)",
    [Str_RowServer] = "Server (optional)",
    [Str_RowServerKey] = "Server key",
    [Str_RowPcId] = "PC ID (on the server)",
    [Str_RowPair] = "Pair with this PC",
    [Str_RowLanguage] = "Language",
    [Str_RowRumble] = "Rumble",
    [Str_HostSolar] = "SolarNX service",
    [Str_HostSunshine] = "Sunshine",
    [Str_NotSet] = "(not set)",
    [Str_NoServer] = "(none - direct)",
    [Str_KeySet] = "(set)",
    [Str_KeyNone] = "(none)",
    [Str_PairedPrefix] = "paired with PC ",
    [Str_NotPaired] = "not paired",
    [Str_LanguageAuto] = "Auto (device language)",
    [Str_Note1] = "Without a server: forward UDP 19010 on the PC's router and enter its public address.",
    [Str_Note2] = "With your rendezvous server: no forwarding - enter the server and the PC ID from the PC.",
    [Str_Note3] = "Then pair once: \"Pair with this PC\" here and \"Pair\" on the PC.",

    [Str_LibWaiting] = "Waiting for the library from your PC...",
    [Str_LibEmpty] = "Your library is empty",
    [Str_LibAddHint] = "Add games in SolarNX on the PC (Game library button) with streaming enabled",

    [Str_IpAddressPrefix] = "IP Address: ",
    [Str_LayoutSwitch] = "Layout: Switch (A = A)",
    [Str_LayoutXbox] = "Layout: Xbox (by position)",
    [Str_MenuSettingsWifi] = "Settings: Wi-Fi",
    [Str_MenuInternetPrefix] = "Internet: ",
    [Str_PcVersionPrefix] = "PC: SolarNX ",
    [Str_Off] = "off",
    [Str_RumbleLargeOnly] = "no small motor",
    [Str_RumbleAll] = "all",

    [Str_EditAddress] = "e.g. 192.168.1.10",
    [Str_EditServer] = "server address or name",
    [Str_EditServerKey] = "key shown on the PC",
    [Str_EditPcId] = "PC ID shown on the PC",
};

static const char *const kRussian[Str_Count] = {
    [Str_MyGames] = "Мои игры",
    [Str_SettingsTitle] = "Настройки",
    [Str_HintPlay] = "A  Играть      B  Назад",
    [Str_HintBack] = "B  Назад",
    [Str_HintChange] = "A  Изменить      B  Назад",
    [Str_HintPair] = "A  Привязать      B  Назад",
    [Str_HintPlayTouch] = "Коснитесь игры — запуск",
    [Str_HintChangeTouch] = "Коснитесь строки — изменить",
    [Str_Back] = "Назад",

    [Str_RowHostType] = "Сервер",
    [Str_RowAddress] = "Адрес ПК (без сервера)",
    [Str_RowServer] = "Сервер (необязательно)",
    [Str_RowServerKey] = "Ключ сервера",
    [Str_RowPcId] = "ID ПК (на сервере)",
    [Str_RowPair] = "Привязать к этому ПК",
    [Str_RowLanguage] = "Язык",
    [Str_RowRumble] = "Вибрация",
    [Str_HostSolar] = "Сервис SolarNX",
    [Str_HostSunshine] = "Sunshine",
    [Str_NotSet] = "(не задан)",
    [Str_NoServer] = "(нет — напрямую)",
    [Str_KeySet] = "(задан)",
    [Str_KeyNone] = "(нет)",
    [Str_PairedPrefix] = "привязан к ПК ",
    [Str_NotPaired] = "не привязан",
    [Str_LanguageAuto] = "Авто (язык устройства)",
    [Str_Note1] = "Без сервера: пробросьте UDP 19010 на роутере ПК и введите его внешний адрес.",
    [Str_Note2] = "С сервером-посредником проброс не нужен: введите сервер и ID ПК с компьютера.",
    [Str_Note3] = "Затем один раз привяжите: «Привязать к этому ПК» здесь и «Привязать» на ПК.",

    [Str_LibWaiting] = "Ждём библиотеку с ПК...",
    [Str_LibEmpty] = "Библиотека пуста",
    [Str_LibAddHint] = "Добавьте игры в SolarNX на ПК (кнопка «Библиотека игр») при включённой трансляции",

    [Str_IpAddressPrefix] = "IP-адрес: ",
    [Str_LayoutSwitch] = "Раскладка: Switch (A = A)",
    [Str_LayoutXbox] = "Раскладка: Xbox (по положению)",
    [Str_MenuSettingsWifi] = "Настройки: Wi-Fi",
    [Str_MenuInternetPrefix] = "Интернет: ",
    [Str_PcVersionPrefix] = "ПК: SolarNX ",
    [Str_Off] = "выкл",
    [Str_RumbleLargeOnly] = "без малого мотора",
    [Str_RumbleAll] = "вся",

    [Str_EditAddress] = "напр. 192.168.1.10",
    [Str_EditServer] = "адрес или имя сервера",
    [Str_EditServerKey] = "ключ с экрана ПК",
    [Str_EditPcId] = "ID ПК с экрана компьютера",
};

// 0 auto, 1 english, 2 russian
static int s_language = 0;

void stringsInit(int language)
{
    s_language = language;
}

void stringsSetLanguage(int language)
{
    s_language = language;
}

int stringsLanguage(void)
{
    return s_language;
}

static bool systemIsRussian(void)
{
    // SDL_GetPreferredLocales goes through the OS API on Android/iOS, env elsewhere.
    SDL_Locale *locales = SDL_GetPreferredLocales();
    bool russian = false;
    if (locales != NULL && locales[0].language != NULL)
        russian = strncmp(locales[0].language, "ru", 2) == 0;
    SDL_free(locales);
    return russian;
}

const char *tr(StrId id)
{
    if (id < 0 || id >= Str_Count)
        return "";
    bool russian = s_language == 2 || (s_language == 0 && systemIsRussian());
    const char *text = russian ? kRussian[id] : kEnglish[id];
    return text != NULL ? text : kEnglish[id];
}
