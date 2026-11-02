using System.Text.Json;

namespace SolarNX.Library;

/// <summary>
/// The user's own game list, persisted to library.json next to the exe (separate from config.json:
/// it grows, holds cover references and is edited from its own screen). Thread-safe: the UI edits it
/// while LibraryPusher and AppOrchestrator read it from thread-pool threads, so every read hands out
/// clones and every write goes through Upsert/Remove, which save immediately and raise Changed.
/// </summary>
public sealed class GameLibrary
{
    private sealed class Stored
    {
        public uint NextId { get; set; } = 1;
        public List<GameEntry> Games { get; set; } = new();
    }

    private readonly object _lock = new();
    private Stored _data = new();

    /// <summary>Raised after any successful change (on the caller's thread).</summary>
    public event Action? Changed;

    public static string LibraryPath => Path.Combine(AppContext.BaseDirectory, "library.json");
    public static string CoversDirectory => Path.Combine(AppContext.BaseDirectory, "covers");

    public static GameLibrary Load()
    {
        var library = new GameLibrary();
        try
        {
            if (File.Exists(LibraryPath))
            {
                var loaded = JsonSerializer.Deserialize<Stored>(File.ReadAllText(LibraryPath));
                if (loaded is not null)
                {
                    library._data = loaded;
                    // Guard against a hand-edited file: NextId must stay ahead of every existing id or
                    // a new entry would collide with one the Switch might still have cached.
                    var maxId = loaded.Games.Count == 0 ? 0 : loaded.Games.Max(g => g.Id);
                    if (loaded.NextId <= maxId) loaded.NextId = maxId + 1;
                }
            }
        }
        catch (Exception ex)
        {
            Logger.Log("GameLibrary", $"failed to load {LibraryPath}, starting empty: {ex.Message}");
        }
        return library;
    }

    public IReadOnlyList<GameEntry> Snapshot()
    {
        lock (_lock) return _data.Games.Select(g => g.Clone()).ToList();
    }

    public GameEntry? Find(uint id)
    {
        lock (_lock) return _data.Games.FirstOrDefault(g => g.Id == id)?.Clone();
    }

    /// <summary>Adds (Id == 0 → a new id is assigned and written back into <paramref name="entry"/>)
    /// or replaces the entry with the same Id.</summary>
    public void Upsert(GameEntry entry)
    {
        lock (_lock)
        {
            if (entry.Id == 0) entry.Id = _data.NextId++;
            var index = _data.Games.FindIndex(g => g.Id == entry.Id);
            if (index >= 0) _data.Games[index] = entry.Clone();
            else _data.Games.Add(entry.Clone());
            SaveLocked();
        }
        Changed?.Invoke();
    }

    public void Remove(uint id)
    {
        string? coverToDelete = null;
        lock (_lock)
        {
            var entry = _data.Games.FirstOrDefault(g => g.Id == id);
            if (entry is null) return;
            _data.Games.Remove(entry);
            if (!string.IsNullOrEmpty(entry.CoverFile)) coverToDelete = Path.Combine(CoversDirectory, entry.CoverFile);
            SaveLocked();
        }
        if (coverToDelete is not null)
        {
            try { File.Delete(coverToDelete); } catch { /* best-effort - an orphaned image is harmless */ }
        }
        Changed?.Invoke();
    }

    /// <summary>Copies an image into CoversDirectory under a name unique to this entry and returns
    /// that file name (for GameEntry.CoverFile). The entry itself isn't modified or saved here.</summary>
    public static string ImportCover(uint entryId, string sourcePath)
    {
        Directory.CreateDirectory(CoversDirectory);
        // Timestamped so replacing a cover never overwrites a file the previous version still
        // references until the entry is saved (and so the Switch-side hash changes with it).
        var fileName = $"{entryId}-{DateTime.UtcNow:yyyyMMddHHmmssfff}{Path.GetExtension(sourcePath).ToLowerInvariant()}";
        File.Copy(sourcePath, Path.Combine(CoversDirectory, fileName), overwrite: true);
        return fileName;
    }

    public static string? CoverPath(GameEntry entry) =>
        string.IsNullOrEmpty(entry.CoverFile) ? null : Path.Combine(CoversDirectory, entry.CoverFile);

    private void SaveLocked()
    {
        try
        {
            File.WriteAllText(LibraryPath, JsonSerializer.Serialize(_data, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception ex)
        {
            Logger.Log("GameLibrary", $"failed to save {LibraryPath}: {ex.Message}");
        }
    }
}
