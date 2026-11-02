using System.Text.Json;
using Tomlyn;
using Tomlyn.Model;

namespace SolarNX.ShadPs4;

/// <summary>
/// DEPRECATED / not currently used: only ShadPs4LibraryChannel (also deprecated/unwired) calls
/// this. Left intact rather than deleted in case the Switch-side library-picker approach is worth
/// reviving - see ShadPs4LibraryChannel's doc comment.
///
/// Discovers the user's installed shadPS4 games by reading shadPS4's own config (which folders it
/// scans) and then recursively looking for the standard PS4 dump layout (sce_sys/param.sfo +
/// sce_sys/icon0.png) under those folders - the configured folder list can't be trusted as pointing
/// directly at valid game folders (live-verified: one configured entry on the dev machine was an
/// empty leftover folder, and a real game was two levels deeper than another configured root), so
/// this walks down looking for the marker file instead of trusting folder names/depth.
///
/// Two config formats are supported, tried in this order: config.json's General.install_dirs
/// (an array of {enabled, path} objects) - confirmed live to be what a "BBFork"-channel
/// shadPS4QtLauncher build actually persists its Game Folders list to, with config.toml sitting
/// alongside it unused/stale - and falling back to config.toml's [GUI] Directories/
/// DirectoriesEnabled (mainline shadPS4's documented format) if config.json is missing or doesn't
/// parse. Both are re-read fresh every call since either is mutable between calls.
/// </summary>
public sealed class ShadPs4LibraryScanner
{
    private const int MaxScanDepth = 5;
    private const int MaxGames = LibraryProtocol.MaxGames;

    /// <summary>Re-reads config.toml fresh every call (it's mutable - the user can add/remove game
    /// folders in shadPS4's own GUI between calls) and re-walks the filesystem. Every failure mode
    /// (missing/unreadable config, bad folder, permission error, malformed SFO) is skip-and-continue;
    /// this never throws.</summary>
    public IReadOnlyList<ShadPs4GameEntry> ScanLibrary()
    {
        var result = new List<ShadPs4GameEntry>();
        var seenTitleIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        List<string> roots;
        try
        {
            roots = ReadEnabledDirectories();
        }
        catch (Exception ex)
        {
            Logger.Log("ShadPs4LibraryScanner", $"failed to read shadPS4 config: {ex.Message}");
            return result;
        }

        foreach (var root in roots)
        {
            if (result.Count >= MaxGames) break;

            try
            {
                if (!Directory.Exists(root)) continue;
                ScanDirectory(root, 0, result, seenTitleIds);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                Logger.Log("ShadPs4LibraryScanner", $"failed to scan root '{root}': {ex.Message}");
            }
        }

        if (result.Count > MaxGames)
        {
            Logger.Log("ShadPs4LibraryScanner", $"found {result.Count} games, truncating to {MaxGames}");
            result.RemoveRange(MaxGames, result.Count - MaxGames);
        }

        return result;
    }

    private static string ConfigDir =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "shadPS4");

    private static List<string> ReadEnabledDirectories()
    {
        var jsonPath = Path.Combine(ConfigDir, "config.json");
        if (File.Exists(jsonPath))
        {
            var fromJson = TryReadDirectoriesFromJson(jsonPath);
            if (fromJson is not null) return fromJson;
        }

        return ReadDirectoriesFromToml(Path.Combine(ConfigDir, "config.toml"));
    }

    /// <summary>Returns null (not an empty list) on any parse failure/unexpected shape, so the
    /// caller falls back to config.toml instead of concluding "zero enabled directories" just
    /// because this build's config.json didn't parse as expected.</summary>
    private static List<string>? TryReadDirectoriesFromJson(string jsonPath)
    {
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(jsonPath));
            if (!doc.RootElement.TryGetProperty("General", out var general)) return null;
            if (!general.TryGetProperty("install_dirs", out var installDirs) || installDirs.ValueKind != JsonValueKind.Array) return null;

            var enabled = new List<string>();
            foreach (var entry in installDirs.EnumerateArray())
            {
                if (entry.ValueKind != JsonValueKind.Object) continue;
                if (!entry.TryGetProperty("enabled", out var enabledProp)) continue;
                if (enabledProp.ValueKind != JsonValueKind.True && enabledProp.ValueKind != JsonValueKind.False) continue;
                if (!enabledProp.GetBoolean()) continue;
                if (!entry.TryGetProperty("path", out var pathProp) || pathProp.ValueKind != JsonValueKind.String) continue;

                var path = pathProp.GetString();
                if (!string.IsNullOrEmpty(path)) enabled.Add(path);
            }
            return enabled;
        }
        catch (Exception ex) when (ex is JsonException or IOException or UnauthorizedAccessException)
        {
            Logger.Log("ShadPs4LibraryScanner", $"failed to parse config.json: {ex.Message}");
            return null;
        }
    }

    private static List<string> ReadDirectoriesFromToml(string tomlPath)
    {
        var enabled = new List<string>();
        if (!File.Exists(tomlPath)) return enabled;

        try
        {
            var text = File.ReadAllText(tomlPath);
            var model = TomlSerializer.Deserialize<TomlTable>(text);
            if (model is null) return enabled;
            if (model["GUI"] is not TomlTable gui) return enabled;

            if (gui["Directories"] is not TomlArray directories) return enabled;
            var directoriesEnabled = gui["DirectoriesEnabled"] as TomlArray;

            // DirectoriesEnabled can legitimately be a different length than Directories -
            // confirmed live on the dev machine, where the sibling installDirs/installDirsEnabled
            // pair in this exact config.toml already had a mismatched length. Never assume they
            // line up beyond whatever the shorter of the two covers.
            int count = directoriesEnabled is null ? 0 : Math.Min(directories.Count, directoriesEnabled.Count);

            for (int i = 0; i < count; i++)
            {
                if (directories[i] is not string dir) continue;
                if (directoriesEnabled![i] is not bool isEnabled || !isEnabled) continue;
                enabled.Add(dir);
            }
        }
        catch (Exception ex)
        {
            Logger.Log("ShadPs4LibraryScanner", $"failed to parse config.toml: {ex.Message}");
        }

        return enabled;
    }

    private static void ScanDirectory(string directory, int depth, List<ShadPs4GameEntry> result, HashSet<string> seenTitleIds)
    {
        if (result.Count >= MaxGames) return;

        var sfoPath = Path.Combine(directory, "sce_sys", "param.sfo");
        if (File.Exists(sfoPath))
        {
            TryAddGame(sfoPath, directory, result, seenTitleIds);
            return; // don't recurse further into a folder that's already a confirmed valid game
        }

        if (depth >= MaxScanDepth) return;

        IEnumerable<string> subdirs;
        try
        {
            subdirs = Directory.EnumerateDirectories(directory);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return;
        }

        foreach (var subdir in subdirs)
        {
            if (result.Count >= MaxGames) return;
            ScanDirectory(subdir, depth + 1, result, seenTitleIds);
        }
    }

    private static void TryAddGame(string sfoPath, string gameDir, List<ShadPs4GameEntry> result, HashSet<string> seenTitleIds)
    {
        try
        {
            var sfoBytes = File.ReadAllBytes(sfoPath);
            if (!ParamSfoReader.TryRead(sfoBytes, out var title, out var titleId)) return;
            if (string.IsNullOrEmpty(titleId)) return;
            if (!seenTitleIds.Add(titleId)) return; // duplicate TitleId across roots - keep the first match

            byte[]? iconBytes = null;
            var iconPath = Path.Combine(gameDir, "sce_sys", "icon0.png");
            if (File.Exists(iconPath))
            {
                try
                {
                    iconBytes = File.ReadAllBytes(iconPath);
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                {
                    Logger.Log("ShadPs4LibraryScanner", $"failed to read icon for '{titleId}': {ex.Message}");
                }
            }

            result.Add(new ShadPs4GameEntry(titleId, title ?? titleId, iconBytes));
        }
        catch (Exception ex)
        {
            Logger.Log("ShadPs4LibraryScanner", $"failed to parse '{sfoPath}': {ex.Message}");
        }
    }
}
