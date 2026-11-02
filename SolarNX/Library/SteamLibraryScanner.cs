using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace SolarNX.Library;

/// <summary>One installed Steam app, for the "pick from Steam" list in the library editor.</summary>
public sealed record SteamGame(string AppId, string Name, string? CoverPath)
{
    public override string ToString() => Name;
}

/// <summary>
/// Lists installed Steam games straight from Steam's own files - no Steamworks, no network:
/// steamapps/libraryfolders.vdf for the library roots, each root's appmanifest_*.acf for appid and
/// name, and appcache/librarycache/&lt;appid&gt;/ for the 600×900 capsule Steam itself shows in its
/// library (falling back to the landscape header, which CoverImage then center-crops). Read-only;
/// tolerant of anything missing or malformed (it just yields fewer games).
/// </summary>
public static class SteamLibraryScanner
{
    private static readonly Regex PathLine = new("\"path\"\\s+\"([^\"]+)\"", RegexOptions.Compiled);
    private static readonly Regex AppIdLine = new("\"appid\"\\s+\"(\\d+)\"", RegexOptions.Compiled);
    private static readonly Regex NameLine = new("\"name\"\\s+\"([^\"]+)\"", RegexOptions.Compiled);

    // Steam runtime/tooling packages that show up as "installed apps" but aren't games.
    private static readonly HashSet<string> NonGameAppIds = new() { "228980" /* Steamworks Common Redistributables */ };
    private static readonly string[] NonGameNamePrefixes = { "Proton", "Steam Linux Runtime", "Steamworks" };

    public static IReadOnlyList<SteamGame> ListInstalled()
    {
        var steamRoot = FindSteamRoot();
        if (steamRoot is null) return Array.Empty<SteamGame>();

        var games = new Dictionary<string, SteamGame>();
        foreach (var library in LibraryRoots(steamRoot))
        {
            var steamApps = Path.Combine(library, "steamapps");
            if (!Directory.Exists(steamApps)) continue;
            foreach (var manifest in SafeEnumerate(steamApps, "appmanifest_*.acf"))
            {
                try
                {
                    var text = File.ReadAllText(manifest);
                    var appId = AppIdLine.Match(text).Groups[1].Value;
                    var name = Unescape(NameLine.Match(text).Groups[1].Value);
                    if (appId.Length == 0 || name.Length == 0) continue;
                    if (NonGameAppIds.Contains(appId) || NonGameNamePrefixes.Any(p => name.StartsWith(p, StringComparison.OrdinalIgnoreCase))) continue;
                    games[appId] = new SteamGame(appId, name, FindCover(steamRoot, appId));
                }
                catch
                {
                    // unreadable manifest - skip it
                }
            }
        }
        return games.Values.OrderBy(g => g.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    private static string? FindSteamRoot()
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
        var steamPath = key?.GetValue("SteamPath") as string;
        if (!string.IsNullOrEmpty(steamPath) && Directory.Exists(steamPath)) return Path.GetFullPath(steamPath);
        var steamExe = key?.GetValue("SteamExe") as string;
        return string.IsNullOrEmpty(steamExe) ? null : Path.GetDirectoryName(Path.GetFullPath(steamExe));
    }

    private static IEnumerable<string> LibraryRoots(string steamRoot)
    {
        var roots = new List<string> { steamRoot };
        var vdf = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
        try
        {
            foreach (Match m in PathLine.Matches(File.ReadAllText(vdf)))
                roots.Add(Unescape(m.Groups[1].Value));
        }
        catch
        {
            // no/unreadable libraryfolders.vdf - the main root alone
        }
        return roots.Distinct(StringComparer.OrdinalIgnoreCase);
    }

    private static string? FindCover(string steamRoot, string appId)
    {
        var cacheDir = Path.Combine(steamRoot, "appcache", "librarycache");
        var appDir = Path.Combine(cacheDir, appId);
        var candidates = new[]
        {
            Path.Combine(appDir, "library_600x900.jpg"),
            Path.Combine(cacheDir, $"{appId}_library_600x900.jpg"), // older flat cache layout
        };
        foreach (var candidate in candidates)
            if (File.Exists(candidate)) return candidate;

        if (Directory.Exists(appDir))
        {
            var nested = SafeEnumerate(appDir, "library_600x900.jpg", SearchOption.AllDirectories).FirstOrDefault();
            if (nested is not null) return nested;
            var header = Path.Combine(appDir, "header.jpg");
            if (File.Exists(header)) return header;
        }
        var flatHeader = Path.Combine(cacheDir, $"{appId}_header.jpg");
        return File.Exists(flatHeader) ? flatHeader : null;
    }

    private static IEnumerable<string> SafeEnumerate(string dir, string pattern, SearchOption option = SearchOption.TopDirectoryOnly)
    {
        try { return Directory.EnumerateFiles(dir, pattern, option).ToList(); }
        catch { return Array.Empty<string>(); }
    }

    // VDF escapes backslashes ("C:\\steam").
    private static string Unescape(string value) => value.Replace("\\\\", "\\");
}
