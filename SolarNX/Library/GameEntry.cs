using System.Text.Json.Serialization;
using SolarNX.Input;

namespace SolarNX.Library;

/// <summary>How a library entry is started - see GameLauncher.</summary>
[JsonConverter(typeof(JsonStringEnumConverter))]
public enum GameEntryType
{
    /// <summary>A plain exe started directly, no Steam involved.</summary>
    Native,

    /// <summary>An installed Steam game, started through steam://rungameid so Steam Input applies.</summary>
    Steam,

    /// <summary>An emulator exe plus a ROM, joined by an argument template ("{rom}" placeholder).</summary>
    Emulator,

    /// <summary>A user-written cmd script, run via cmd.exe - for anything the other types can't express.</summary>
    Script,
}

/// <summary>
/// One game in the host-side library (library.json). Only Id, Name and the cover ever leave this
/// machine - everything else stays on the PC and is used by GameLauncher when the Switch player
/// picks the entry (HidPacket.AppTarget = Library, HidPacket.LibraryEntryId = Id).
/// </summary>
public sealed class GameEntry
{
    /// <summary>Stable numeric id, assigned once by GameLibrary and never reused - it's what the
    /// Switch sends back in HidPacket.LibraryEntryId.</summary>
    public uint Id { get; set; }

    public string Name { get; set; } = "";

    public GameEntryType Type { get; set; } = GameEntryType.Native;

    // Native
    public string ExePath { get; set; } = "";
    public string Arguments { get; set; } = "";
    public string WorkingDirectory { get; set; } = "";

    // Steam
    public string SteamAppId { get; set; } = "";

    // Emulator
    public string EmulatorPath { get; set; } = "";
    public string RomPath { get; set; } = "";
    public string ArgumentTemplate { get; set; } = "\"{rom}\"";

    // Script
    public string Script { get; set; } = "";

    /// <summary>Virtual pad the game gets. Steam games get Steam Input on top of whichever this is;
    /// DualShock4 is the only option that carries gyro/accel (useful for some emulators).</summary>
    [JsonConverter(typeof(JsonStringEnumConverter))]
    public ControllerMode Controller { get; set; } = ControllerMode.Xbox360;

    /// <summary>File name inside GameLibrary.CoversDirectory (empty = no cover). The library keeps its
    /// own copy so library.json never points at a file the user later moves or deletes.</summary>
    public string CoverFile { get; set; } = "";

    /// <summary>True (default): stream only the game's own window, scaled to the Switch screen - a
    /// windowed game fills the screen and other windows can't cover it. False: the whole primary
    /// monitor, as for Big Picture - for games whose window capture comes out black (some exclusive
    /// fullscreen titles) or that spread over several windows.</summary>
    public bool CaptureGameWindow { get; set; } = true;

    /// <summary>Which part of the cover image becomes the 2:3 tile on the Switch, picked in the
    /// editor's crop dialog. Null = centre crop (the default for any image whose shape isn't 2:3).
    /// Reset whenever the cover file changes.</summary>
    public CoverCrop? CoverCrop { get; set; }

    /// <summary>False (default): started as the ordinary desktop user, as a double-click in Explorer
    /// would - although SolarNX itself runs as administrator. True: with SolarNX's administrator
    /// rights, for games and tools that need them. A game whose own manifest demands administrator
    /// gets it either way. Not used for Steam games - Steam starts those itself.</summary>
    public bool RunAsAdmin { get; set; }

    public GameEntry Clone()
    {
        var copy = (GameEntry)MemberwiseClone();
        copy.CoverCrop = CoverCrop?.Clone();
        return copy;
    }
}

/// <summary>A rectangle in normalized source-image coordinates (0..1 on both axes), always 2:3 in
/// source pixels. Normalized so it survives the image being re-decoded at any size.</summary>
public sealed class CoverCrop
{
    public double X { get; set; }
    public double Y { get; set; }
    public double Width { get; set; }
    public double Height { get; set; }

    public bool IsValid =>
        Width > 0 && Height > 0 && X >= 0 && Y >= 0 && X + Width <= 1.0001 && Y + Height <= 1.0001;

    public CoverCrop Clone() => (CoverCrop)MemberwiseClone();
}
