namespace SolarNX.ShadPs4;

/// <summary>DEPRECATED / not currently used: only ShadPs4LibraryScanner/LibraryProtocol (also
/// deprecated/unwired) reference this - see ShadPs4LibraryChannel's doc comment.
///
/// One discovered shadPS4 game, ready to be serialized onto the wire by LibraryProtocol.
/// IconPngBytes is the raw, unmodified contents of the game's sce_sys/icon0.png - null when that
/// file is missing (a missing icon doesn't disqualify the game itself).</summary>
public readonly record struct ShadPs4GameEntry(string TitleId, string Title, byte[]? IconPngBytes);
