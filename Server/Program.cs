using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using SolarNX.Rendezvous;

// Matchmaking server for SolarNX's Internet mode. PCs register here under their host id from the
// same UDP socket they serve the Switch on, so the server sees the public address their router maps
// that socket to. A Switch looks a PC up by its host id: the server tells it the PC's public address
// and tells the PC the Switch's, both start sending to each other at once, and the routers let the
// traffic through (UDP hole punching). From then on the server is out of the path - unless relaying
// is on and the direct path never opens, in which case it forwards the (end-to-end encrypted)
// datagrams. See RendezvousWire for the protocol and docs/server.md for running it.

var config = ServerConfig.Load(args);
using var socket = new UdpClient(new IPEndPoint(IPAddress.Any, config.Port));
const int SioUdpConnReset = -1744830452; // ICMP "port unreachable" must not break the receive loop on Windows
if (OperatingSystem.IsWindows()) socket.Client.IOControl(SioUdpConnReset, new byte[] { 0 }, null);

var accessKey = KeyBytes(config.AccessKey);
var hosts = new Dictionary<string, (IPEndPoint Endpoint, IPEndPoint? Local, DateTime Seen)>();
var relayPairs = new Dictionary<(string Host, IPEndPoint Switch), RelayBudget>();
var lastCleanup = DateTime.UtcNow;

Log($"SolarNX rendezvous server listening on UDP {config.Port} - access key {(accessKey is null ? "off (open server)" : "on")}, relay {(config.Relay ? $"on ({config.RelayMaxKbps} kbps per Switch)" : "off")}");

using var stop = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; stop.Cancel(); };
AppDomain.CurrentDomain.ProcessExit += (_, _) => stop.Cancel();

while (!stop.IsCancellationRequested)
{
    UdpReceiveResult received;
    try
    {
        received = await socket.ReceiveAsync(stop.Token);
    }
    catch (OperationCanceledException) { break; }
    catch (SocketException) { continue; }

    try
    {
        Handle(received.Buffer, received.RemoteEndPoint);
    }
    catch (Exception ex)
    {
        Log($"bad datagram from {received.RemoteEndPoint}: {ex.Message}");
    }

    if (DateTime.UtcNow - lastCleanup > TimeSpan.FromSeconds(10)) Cleanup();
}
Log("stopped");
return;

void Handle(byte[] data, IPEndPoint from)
{
    var type = RendezvousWire.Parse(data);
    if (type is null) return;
    var payload = data.AsSpan(RendezvousWire.HeaderSize);
    const int idAndKey = RendezvousWire.HostIdSize + RendezvousWire.KeySize;

    switch (type)
    {
        case RendezvousWire.Type.Register when payload.Length >= idAndKey:
        {
            if (!KeyMatches(payload.Slice(RendezvousWire.HostIdSize, RendezvousWire.KeySize))) return;
            var hostId = HostId(payload);
            var isNew = !hosts.TryGetValue(hostId, out var previous) || !previous.Endpoint.Equals(from);
            if (isNew && !hosts.ContainsKey(hostId) && hosts.Count >= config.MaxHosts)
            {
                Log($"host limit ({config.MaxHosts}) reached - ignoring {hostId} from {from}");
                return;
            }
            var local = payload.Length >= idAndKey + RendezvousWire.EndpointSize
                ? RendezvousWire.ReadEndpoint(payload[idAndKey..])
                : null;
            hosts[hostId] = (from, local, DateTime.UtcNow);
            if (isNew || config.Verbose) Log($"PC {hostId} registered from {from}");
            var reply = RendezvousWire.Packet(RendezvousWire.Type.Registered, RendezvousWire.EndpointSize);
            RendezvousWire.WriteEndpoint(reply.AsSpan(RendezvousWire.HeaderSize), from);
            Send(reply, from);
            return;
        }

        case RendezvousWire.Type.Lookup when payload.Length >= idAndKey:
        {
            var hostId = HostId(payload);
            var reply = RendezvousWire.Packet(RendezvousWire.Type.Peer, 1 + RendezvousWire.EndpointSize + 1);
            var body = reply.AsSpan(RendezvousWire.HeaderSize);
            if (!KeyMatches(payload.Slice(RendezvousWire.HostIdSize, RendezvousWire.KeySize)))
            {
                body[0] = (byte)RendezvousWire.PeerStatus.BadKey;
                Send(reply, from);
                return;
            }
            if (!hosts.TryGetValue(hostId, out var host) || Expired(host.Seen))
            {
                body[0] = (byte)RendezvousWire.PeerStatus.UnknownHost;
                Send(reply, from);
                if (config.Verbose) Log($"Switch {from} looked up unknown PC {hostId}");
                return;
            }
            body[0] = (byte)RendezvousWire.PeerStatus.Ok;
            // Behind the same router as the PC: its local address - see RendezvousWire.
            var sameNetwork = host.Local is not null && from.Address.Equals(host.Endpoint.Address);
            RendezvousWire.WriteEndpoint(body[1..], sameNetwork ? host.Local! : host.Endpoint);
            body[1 + RendezvousWire.EndpointSize] = config.Relay ? (byte)1 : (byte)0;
            Send(reply, from);

            // The PC punches towards the Switch at the same moment the Switch starts sending to it.
            var introduce = RendezvousWire.Packet(RendezvousWire.Type.Introduce, RendezvousWire.EndpointSize);
            RendezvousWire.WriteEndpoint(introduce.AsSpan(RendezvousWire.HeaderSize), from);
            Send(introduce, host.Endpoint);
            if (config.Verbose || sameNetwork)
                Log($"Switch {from} -> PC {hostId} at {(sameNetwork ? $"{host.Local} (same network as the PC)" : host.Endpoint.ToString())}");
            return;
        }

        case RendezvousWire.Type.RelayUp when config.Relay && payload.Length > idAndKey:
        {
            if (!KeyMatches(payload.Slice(RendezvousWire.HostIdSize, RendezvousWire.KeySize))) return;
            var hostId = HostId(payload);
            if (!hosts.TryGetValue(hostId, out var host) || Expired(host.Seen)) return;
            var datagram = payload[idAndKey..];
            if (!Budget(hostId, from).Take(datagram.Length)) return;
            var down = RendezvousWire.Packet(RendezvousWire.Type.RelayDown, RendezvousWire.EndpointSize + datagram.Length);
            RendezvousWire.WriteEndpoint(down.AsSpan(RendezvousWire.HeaderSize), from);
            datagram.CopyTo(down.AsSpan(RendezvousWire.HeaderSize + RendezvousWire.EndpointSize));
            Send(down, host.Endpoint);
            return;
        }

        case RendezvousWire.Type.RelayBack when config.Relay && payload.Length > RendezvousWire.EndpointSize:
        {
            // Only a registered PC, and only to a Switch that relayed to that PC recently.
            var host = hosts.FirstOrDefault(h => h.Value.Endpoint.Equals(from));
            if (host.Key is null) return;
            var target = RendezvousWire.ReadEndpoint(payload);
            if (!relayPairs.TryGetValue((host.Key, target), out var budget) || !budget.Take(payload.Length)) return;
            var datagram = payload[RendezvousWire.EndpointSize..];
            var back = RendezvousWire.Packet(RendezvousWire.Type.RelayFrom, datagram.Length);
            datagram.CopyTo(back.AsSpan(RendezvousWire.HeaderSize));
            Send(back, target);
            return;
        }
    }
}

RelayBudget Budget(string hostId, IPEndPoint switchEndpoint)
{
    if (!relayPairs.TryGetValue((hostId, switchEndpoint), out var budget))
    {
        budget = new RelayBudget(config.RelayMaxKbps);
        relayPairs[(hostId, switchEndpoint)] = budget;
        Log($"relaying between Switch {switchEndpoint} and PC {hostId}");
    }
    return budget;
}

bool Expired(DateTime seen) => DateTime.UtcNow - seen > TimeSpan.FromSeconds(config.HostTimeoutSeconds);

void Cleanup()
{
    lastCleanup = DateTime.UtcNow;
    foreach (var (id, host) in hosts.Where(h => Expired(h.Value.Seen)).ToList())
    {
        hosts.Remove(id);
        Log($"PC {id} timed out ({host.Endpoint})");
    }
    foreach (var pair in relayPairs.Where(p => DateTime.UtcNow - p.Value.LastUsed > TimeSpan.FromSeconds(60)).ToList())
    {
        relayPairs.Remove(pair.Key);
        Log($"relay between Switch {pair.Key.Switch} and PC {pair.Key.Host} idle - closed");
    }
}

bool KeyMatches(ReadOnlySpan<byte> presented) =>
    accessKey is null || CryptographicOperations.FixedTimeEquals(accessKey, presented);

static byte[]? KeyBytes(string key)
{
    if (string.IsNullOrEmpty(key)) return null;
    var bytes = new byte[RendezvousWire.KeySize];
    var utf8 = Encoding.UTF8.GetBytes(key);
    utf8.AsSpan(0, Math.Min(utf8.Length, bytes.Length)).CopyTo(bytes);
    return bytes;
}

static string HostId(ReadOnlySpan<byte> payload) =>
    Encoding.ASCII.GetString(payload[..RendezvousWire.HostIdSize]).TrimEnd('\0').ToUpperInvariant();

void Send(byte[] packet, IPEndPoint to)
{
    try { socket.Send(packet, packet.Length, to); }
    catch (SocketException) { /* lost like any datagram */ }
}

static void Log(string message) => Console.WriteLine($"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {message}");

/// <summary>Token bucket per relayed Switch: one second of burst at the configured rate.</summary>
sealed class RelayBudget(int maxKbps)
{
    private readonly double _bytesPerSecond = maxKbps * 1000.0 / 8;
    private double _tokens = maxKbps * 1000.0 / 8;
    private DateTime _refilled = DateTime.UtcNow;

    public DateTime LastUsed { get; private set; } = DateTime.UtcNow;

    public bool Take(int bytes)
    {
        var now = DateTime.UtcNow;
        _tokens = Math.Min(_bytesPerSecond, _tokens + (now - _refilled).TotalSeconds * _bytesPerSecond);
        _refilled = now;
        LastUsed = now;
        if (_tokens < bytes) return false;
        _tokens -= bytes;
        return true;
    }
}
