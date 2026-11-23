#pragma once

// RU/EN strings for the shell, mirroring Client/common/lang (the Switch client uses the same
// texts). Language follows settings; "auto" resolves to Russian for RU locales, English
// otherwise. Texts are UTF-8.

typedef enum {
    Str_MyGames,
    Str_SettingsTitle,
    Str_HintPlay,
    Str_HintBack,
    Str_HintChange,
    Str_HintPair,
    Str_HintPlayTouch,
    Str_HintChangeTouch,
    Str_Back,

    Str_RowHostType,
    Str_RowAddress,
    Str_RowServer,
    Str_RowServerKey,
    Str_RowPcId,
    Str_RowPair,
    Str_RowLanguage,
    Str_RowRumble,
    Str_HostSolar,
    Str_HostSunshine,
    Str_NotSet,
    Str_NoServer,
    Str_KeySet,
    Str_KeyNone,
    Str_PairedPrefix,
    Str_NotPaired,
    Str_LanguageAuto,
    Str_Note1,
    Str_Note2,
    Str_Note3,

    Str_LibWaiting,
    Str_LibEmpty,
    Str_LibAddHint,

    Str_IpAddressPrefix,
    Str_LayoutSwitch,
    Str_LayoutXbox,
    Str_MenuSettingsWifi,
    Str_MenuInternetPrefix,
    Str_PcVersionPrefix,
    Str_Off,
    Str_RumbleLargeOnly,
    Str_RumbleAll,

    Str_EditAddress,
    Str_EditServer,
    Str_EditServerKey,
    Str_EditPcId,

    Str_Count
} StrId;

void stringsInit(int language); // 0 auto, 1 english, 2 russian
void stringsSetLanguage(int language);
int stringsLanguage(void);
const char *tr(StrId id);
