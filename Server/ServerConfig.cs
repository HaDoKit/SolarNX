using System.Text.Json;

namespace SolarNX.Rendezvous;

/// <summary>
/// Settings, from lowest to highest priority: built-in defaults, rendezvous.json next to the
/// program (or the file given with --config), environment variables SOLARNX_RV_*, command-line
/// options. See docs/server.md for the full list.
/// </summary>
public sealed class ServerConfig
{
    /// <summary>UDP port to listen on.</summary>
    public int Port { get; set; } = 19020;

    /// <summary>If set, PCs and Switches must present this key - keeps strangers off your server.
    /// Empty = anyone may use it.</summary>
    public string AccessKey { get; set; } = "";

    /// <summary>Relay the traffic through this server when the PC and the Switch can't reach each
    /// other directly (symmetric NAT, carrier-grade NAT). Costs the VPS's bandwidth - a whole stream.</summary>
    public bool Relay { get; set; } = false;

    /// <summary>Per Switch, when relaying: bytes per second above this are dropped.</summary>
    public int RelayMaxKbps { get; set; } = 20000;

    /// <summary>How many PCs may be registered at once.</summary>
    public int MaxHosts { get; set; } = 64;

    /// <summary>A PC that hasn't re-registered for this long is forgotten (PCs re-register every 15s).</summary>
    public int HostTimeoutSeconds { get; set; } = 60;

    /// <summary>Log every lookup and registration, not only changes.</summary>
    public bool Verbose { get; set; } = false;

    public static ServerConfig Load(string[] args)
    {
        var configPath = Path.Combine(AppContext.BaseDirectory, "rendezvous.json");
        for (var i = 0; i < args.Length - 1; i++)
            if (args[i] == "--config") configPath = args[i + 1];

        var config = new ServerConfig();
        if (File.Exists(configPath))
        {
            config = JsonSerializer.Deserialize<ServerConfig>(File.ReadAllText(configPath),
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true, ReadCommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true })
                ?? config;
            Console.WriteLine($"config: {configPath}");
        }

        string? Env(string name) => Environment.GetEnvironmentVariable("SOLARNX_RV_" + name);
        if (int.TryParse(Env("PORT"), out var port)) config.Port = port;
        if (Env("ACCESS_KEY") is { } key) config.AccessKey = key;
        if (bool.TryParse(Env("RELAY"), out var relay)) config.Relay = relay;
        if (int.TryParse(Env("RELAY_MAX_KBPS"), out var kbps)) config.RelayMaxKbps = kbps;
        if (int.TryParse(Env("MAX_HOSTS"), out var maxHosts)) config.MaxHosts = maxHosts;
        if (int.TryParse(Env("HOST_TIMEOUT"), out var timeout)) config.HostTimeoutSeconds = timeout;
        if (bool.TryParse(Env("VERBOSE"), out var verbose)) config.Verbose = verbose;

        for (var i = 0; i < args.Length; i++)
        {
            var next = i + 1 < args.Length ? args[i + 1] : "";
            switch (args[i])
            {
                case "--port": config.Port = int.Parse(next); i++; break;
                case "--key": config.AccessKey = next; i++; break;
                case "--relay": config.Relay = true; break;
                case "--no-relay": config.Relay = false; break;
                case "--relay-max-kbps": config.RelayMaxKbps = int.Parse(next); i++; break;
                case "--max-hosts": config.MaxHosts = int.Parse(next); i++; break;
                case "--host-timeout": config.HostTimeoutSeconds = int.Parse(next); i++; break;
                case "--verbose": config.Verbose = true; break;
                case "--config": i++; break;
                case "--help" or "-h":
                    Console.WriteLine("""
                        solarnx-rendezvous - matchmaking (and optional relay) server for SolarNX's Internet mode

                          --port <n>             UDP port (default 19020)
                          --key <text>           access key PCs and Switches must present (default: none)
                          --relay / --no-relay   relay traffic when a direct connection fails (default: off)
                          --relay-max-kbps <n>   per-Switch relay limit (default 20000)
                          --max-hosts <n>        PCs registered at once (default 64)
                          --host-timeout <s>     forget a PC after this many silent seconds (default 60)
                          --config <file>        settings file (default: rendezvous.json next to the program)
                          --verbose              log every request

                        Environment: SOLARNX_RV_PORT, _ACCESS_KEY, _RELAY, _RELAY_MAX_KBPS, _MAX_HOSTS, _HOST_TIMEOUT, _VERBOSE.
                        """);
                    Environment.Exit(0);
                    break;
            }
        }
        return config;
    }
}
