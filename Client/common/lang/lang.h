#pragma once

// Interface text in English and Russian. The language is the one chosen in the settings
// (settingsGetLanguage), or the console's own language when that is "auto": Russian if the console
// is set to Russian, English otherwise. Every entry has both texts - add new ones to both tables in
// lang.c. Texts are UTF-8 (the system font has Cyrillic). Never use a text as a printf format:
// several end in a space and are meant to be followed by a value ("%s%s").

typedef enum {
    Str_MyGames,
    Str_LayoutSwitch,
    Str_LayoutXbox,
    Str_MenuSettingsWifi,
    Str_MenuInternetPrefix,
    Str_IpAddressPrefix,

    Str_SettingsTitle,
    Str_RowMode,
    Str_RowAddress,
    Str_RowServer,
    Str_RowServerKey,
    Str_RowPcId,
    Str_RowPair,
    Str_RowLanguage,
    Str_ModeInternet,
    Str_ModeWifi,
    Str_NotSet,
    Str_NoServer,
    Str_KeySet,
    Str_KeyNone,
    Str_PairedWithPrefix,
    Str_NotPaired,
    Str_LanguageAuto,
    Str_Note1,
    Str_Note2,
    Str_Note3,
    Str_RestartNote,
    Str_StatusPrefix,
    Str_HintPair,
    Str_HintChange,
    Str_KbdAddress,
    Str_KbdServer,
    Str_KbdServerKey,
    Str_KbdPcId,

    Str_LibWaiting,
    Str_LibEmpty,
    Str_LibAddHint,
    Str_HintPlay,
    Str_HintBack,

    Str_StatusOff,
    Str_StatusNoAddress,
    Str_StatusBadAddress,
    Str_StatusNotPaired,
    Str_StatusWaiting,
    Str_StatusConnected,
    Str_StatusBusy,
    Str_StatusWrongVersion,
    Str_StatusPairing,
    Str_StatusPairingClosed,
    Str_StatusPairingCode,
    Str_StatusPaired,
    Str_StatusPairingRefused,
    Str_StatusPairingFailed,
    Str_StatusServerNotFound,
    Str_StatusServerNoAnswer,
    Str_StatusServerBadKey,
    Str_StatusServerNoPc,
    Str_StatusNoPcId,

    Str_PcVersionPrefix,
    Str_PcLegacy,
    Str_PcOlderSuffix,
    Str_PcNewerSuffix,
    Str_UpdatePc,
    Str_UpdateSwitch,

    Str_Count
} StrId;

// Reads the console's language once. Call after settingsInit().
void langInit(void);

// The text in the current language.
const char *tr(StrId id);
