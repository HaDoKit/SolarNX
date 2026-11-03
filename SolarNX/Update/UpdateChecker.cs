using System.Net.Http;
using System.Net.Http.Headers;
using System.Text.Json;
using SolarNX.Session;

namespace SolarNX.Update;

/// <summary>Whether the Switch app has to follow a PC update - from the release's protocol.</summary>
public enum SwitchUpdate
{
    /// <summary>The release doesn't say (it predates release.json).</summary>
    Unknown,
    /// <summary>Same protocol as the Switch: only the PC needs the update.</summary>
    NotNeeded,
    /// <summary>Same major, newer minor: the Switch keeps working, its update brings the new features.</summary>
    Optional,
    /// <summary>Another major: both have to be updated.</summary>
    Required,
}

/// <summary>A newer release than this build.</summary>
public sealed record UpdateInfo(string Version, string PageUrl, ProtocolVersion? Protocol, SwitchUpdate Switch);

/// <summary>
/// Looks for a newer SolarNX on GitHub (pre-releases included - every release so far is an alpha):
/// at start-up after a short delay and then once a day while <see cref="Config.CheckForUpdates"/> is
/// on, or right away from the settings button. Only reads the public releases list and the newest
/// release's release.json (version + wire protocol, attached by the release workflow); nothing is
/// downloaded or installed - the user gets the release page.
/// </summary>
public sealed class UpdateChecker : IDisposable
{
    private const string ReleasesApi = "https://api.github.com/repos/HaDoKit/SolarNX/releases?per_page=20";
    private static readonly TimeSpan StartupDelay = TimeSpan.FromSeconds(15);
    private static readonly TimeSpan Interval = TimeSpan.FromHours(24);

    private readonly Config _config;
    private readonly SwitchCompatibility _compatibility;
    private readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(15) };
    private readonly CancellationTokenSource _cts = new();
    private readonly SemaphoreSlim _gate = new(1, 1);

    /// <summary>The newer release, or null (none, or not checked yet).</summary>
    public UpdateInfo? Available { get; private set; }

    /// <summary>When the last check finished, and whether it reached GitHub.</summary>
    public DateTime? LastCheckedUtc { get; private set; }
    public bool LastCheckFailed { get; private set; }

    /// <summary>After every check (thread pool).</summary>
    public event Action? Changed;

    public UpdateChecker(Config config, SwitchCompatibility compatibility)
    {
        _config = config;
        _compatibility = compatibility;
        _http.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("SolarNX", AppVersion.Current));
        _http.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        // A known Switch changes what the result says about it.
        _compatibility.Changed += () => { if (Available is { } info) { Available = info with { Switch = Classify(info.Protocol) }; Changed?.Invoke(); } };
    }

    public void Start() => _ = Task.Run(() => LoopAsync(_cts.Token));

    private async Task LoopAsync(CancellationToken ct)
    {
        try
        {
            await Task.Delay(StartupDelay, ct);
            while (!ct.IsCancellationRequested)
            {
                if (_config.CheckForUpdates) await CheckNowAsync();
                await Task.Delay(Interval, ct);
            }
        }
        catch (OperationCanceledException) { }
    }

    /// <summary>Checks now (the settings button, and the daily loop).</summary>
    public async Task CheckNowAsync()
    {
        if (!await _gate.WaitAsync(0)) return; // one at a time
        try
        {
            Available = await FindNewerAsync(_cts.Token);
            LastCheckFailed = false;
            Logger.Log("UpdateChecker", Available is { } info
                ? $"newer release: {info.Version} (protocol {info.Protocol?.ToString() ?? "?"}, Switch update {info.Switch})"
                : $"up to date ({AppVersion.Current})");
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or JsonException or InvalidOperationException)
        {
            LastCheckFailed = true;
            Logger.Log("UpdateChecker", $"check failed: {ex.Message}");
        }
        finally
        {
            LastCheckedUtc = DateTime.UtcNow;
            _gate.Release();
        }
        Changed?.Invoke();
    }

    private async Task<UpdateInfo?> FindNewerAsync(CancellationToken ct)
    {
        using var doc = JsonDocument.Parse(await _http.GetStringAsync(ReleasesApi, ct));
        var current = SemVer.Parse(AppVersion.Current);
        JsonElement? best = null;
        SemVer? bestVersion = null;
        foreach (var release in doc.RootElement.EnumerateArray())
        {
            if (release.GetProperty("draft").GetBoolean()) continue;
            var version = SemVer.Parse(release.GetProperty("tag_name").GetString()?.TrimStart('v'));
            if (version is null || current is null || version.CompareTo(current) <= 0) continue;
            if (bestVersion is null || version.CompareTo(bestVersion) > 0)
            {
                best = release.Clone();
                bestVersion = version;
            }
        }
        if (best is not { } newest || bestVersion is null) return null;

        ProtocolVersion? protocol = null;
        foreach (var asset in newest.GetProperty("assets").EnumerateArray())
        {
            if (asset.GetProperty("name").GetString() != "release.json") continue;
            using var manifest = JsonDocument.Parse(await _http.GetStringAsync(asset.GetProperty("browser_download_url").GetString(), ct));
            protocol = ProtocolVersion.Parse(manifest.RootElement.TryGetProperty("protocol", out var p) ? p.GetString() : null);
        }
        return new UpdateInfo(bestVersion.Text, newest.GetProperty("html_url").GetString() ?? "", protocol, Classify(protocol));
    }

    // Against the Switch as last seen; without one, against this PC's own protocol (they match).
    private SwitchUpdate Classify(ProtocolVersion? release)
    {
        if (release is not { } next) return SwitchUpdate.Unknown;
        var onSwitch = _compatibility.Switch?.Protocol ?? ProtocolVersion.Current;
        if (next.Major != onSwitch.Major) return SwitchUpdate.Required;
        return next.Minor > onSwitch.Minor ? SwitchUpdate.Optional : SwitchUpdate.NotNeeded;
    }

    public void Dispose()
    {
        _cts.Cancel();
        _http.Dispose();
    }

    /// <summary>"0.2.5-alpha" and the like: numeric core, then pre-release identifiers per SemVer
    /// (a release is newer than its pre-releases; numeric identifiers compare as numbers).</summary>
    internal sealed record SemVer(int Major, int Minor, int Patch, string[] Pre, string Text) : IComparable<SemVer>
    {
        public static SemVer? Parse(string? text)
        {
            if (string.IsNullOrWhiteSpace(text)) return null;
            var clean = text.Trim().Split('+')[0];
            var dash = clean.IndexOf('-');
            var core = (dash < 0 ? clean : clean[..dash]).Split('.');
            if (core.Length != 3 || !int.TryParse(core[0], out var major) || !int.TryParse(core[1], out var minor) || !int.TryParse(core[2], out var patch))
                return null;
            return new SemVer(major, minor, patch, dash < 0 ? [] : clean[(dash + 1)..].Split('.'), clean);
        }

        public int CompareTo(SemVer? other)
        {
            if (other is null) return 1;
            var c = (Major, Minor, Patch).CompareTo((other.Major, other.Minor, other.Patch));
            if (c != 0) return c;
            if (Pre.Length == 0 || other.Pre.Length == 0) return other.Pre.Length.CompareTo(Pre.Length);
            for (var i = 0; i < Math.Min(Pre.Length, other.Pre.Length); i++)
            {
                var a = Pre[i];
                var b = other.Pre[i];
                var an = int.TryParse(a, out var ai);
                var bn = int.TryParse(b, out var bi);
                c = an && bn ? ai.CompareTo(bi) : an ? -1 : bn ? 1 : string.CompareOrdinal(a, b);
                if (c != 0) return c;
            }
            return Pre.Length.CompareTo(other.Pre.Length);
        }
    }
}
