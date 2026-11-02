using System.Security.Cryptography;
using System.Text.Json;

namespace SolarNX.Session;

/// <summary>A Switch paired for the Internet mode: its random device id, the name it reported, and
/// the 32-byte pairing key both sides derived (InternetCrypto.PairKey).</summary>
public sealed record PairedDevice(string DeviceId, string Name, byte[] Key, DateTime PairedUtc);

/// <summary>
/// The paired Switches, in paired-devices.json next to the exe. The keys are stored encrypted with
/// Windows DPAPI (current user) - a copy of the file is useless on another account or machine.
/// Thread-safe: the session reads it from the network thread, the settings screen changes it.
/// </summary>
public static class PairedDevices
{
    private static readonly object Lock = new();
    private static readonly string FilePath = Path.Combine(AppContext.BaseDirectory, "paired-devices.json");
    private static List<PairedDevice>? _devices;

    private sealed record StoredDevice(string DeviceId, string Name, string ProtectedKey, DateTime PairedUtc);

    public static event Action? Changed;

    public static IReadOnlyList<PairedDevice> All
    {
        get
        {
            lock (Lock) return Load().ToList();
        }
    }

    public static PairedDevice? Find(byte[] deviceId)
    {
        var id = Convert.ToHexString(deviceId);
        lock (Lock) return Load().FirstOrDefault(d => d.DeviceId == id);
    }

    public static void Add(byte[] deviceId, string name, byte[] key)
    {
        var id = Convert.ToHexString(deviceId);
        lock (Lock)
        {
            var devices = Load();
            devices.RemoveAll(d => d.DeviceId == id);
            devices.Add(new PairedDevice(id, name, key, DateTime.UtcNow));
            Save(devices);
        }
        Changed?.Invoke();
    }

    public static void RemoveAll()
    {
        lock (Lock) Save(Load().Take(0).ToList());
        Changed?.Invoke();
    }

    private static List<PairedDevice> Load()
    {
        if (_devices is not null) return _devices;
        _devices = new List<PairedDevice>();
        try
        {
            if (!File.Exists(FilePath)) return _devices;
            var stored = JsonSerializer.Deserialize<List<StoredDevice>>(File.ReadAllText(FilePath)) ?? new();
            foreach (var s in stored)
            {
                try
                {
                    var key = ProtectedData.Unprotect(Convert.FromBase64String(s.ProtectedKey), null, DataProtectionScope.CurrentUser);
                    _devices.Add(new PairedDevice(s.DeviceId, s.Name, key, s.PairedUtc));
                }
                catch (CryptographicException)
                {
                    Logger.Log("PairedDevices", $"cannot decrypt the key of '{s.Name}' (another Windows user?) - skipped");
                }
            }
        }
        catch (Exception ex)
        {
            Logger.Log("PairedDevices", $"cannot read {FilePath}: {ex.Message}");
        }
        return _devices;
    }

    private static void Save(List<PairedDevice> devices)
    {
        _devices = devices;
        var stored = devices.Select(d => new StoredDevice(d.DeviceId, d.Name,
            Convert.ToBase64String(ProtectedData.Protect(d.Key, null, DataProtectionScope.CurrentUser)), d.PairedUtc));
        File.WriteAllText(FilePath, JsonSerializer.Serialize(stored, new JsonSerializerOptions { WriteIndented = true }));
    }
}
