using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;

namespace SolarNX.Native;

/// <summary>
/// DSCP marks on the streaming sockets (Config.TrafficPriority), so a Wi-Fi router with WMM puts
/// them in its video/voice queues ahead of downloads on the same network. Only helps on routers that
/// honour DSCP (most home ones map its top 3 bits to the Wi-Fi priority); never hurts otherwise.
///
/// Windows ignores IP_TOS from applications, so this goes through qWAVE: the socket joins a flow and
/// QOSSetFlow(QOSSetOutgoingDSCPValue) sets the mark - which needs administrator rights, and SolarNX
/// runs elevated (app.manifest). Loopback sockets (Internet mode's relays) aren't marked: the mark
/// would never leave the machine. Failure is logged once and otherwise ignored.
/// </summary>
internal static class QosMarking
{
    /// <summary>AF41 - WMM "video" on common routers.</summary>
    public const uint DscpVideo = 34;

    /// <summary>EF - audio and input, the most delay-sensitive.</summary>
    public const uint DscpInteractive = 46;

    /// <summary>Config.TrafficPriority - read when a socket is created (an encoder restart picks up a
    /// change).</summary>
    public static volatile bool Enabled = true;

    private static readonly object Lock = new();
    private static IntPtr _handle;
    private static bool _failed;

    /// <summary>Marks a connected socket's outgoing packets with <paramref name="dscp"/>; null when
    /// disabled, on loopback, or if qWAVE refused. Dispose the result before the socket.</summary>
    public static IDisposable? Mark(Socket socket, uint dscp, string what)
    {
        if (!Enabled || socket.RemoteEndPoint is not IPEndPoint { } remote || IPAddress.IsLoopback(remote.Address))
            return null;

        lock (Lock)
        {
            if (_failed) return null;
            if (_handle == IntPtr.Zero)
            {
                var version = new QosVersion { Major = 1, Minor = 0 };
                if (!QOSCreateHandle(ref version, out _handle))
                {
                    Fail($"QOSCreateHandle failed ({Marshal.GetLastWin32Error()})");
                    return null;
                }
            }

            uint flowId = 0;
            // Connected sockets: no destination needed (qWAVE takes the socket's own).
            if (!QOSAddSocketToFlow(_handle, socket.Handle, IntPtr.Zero, QosTrafficType.AudioVideo, QosNonAdaptiveFlow, ref flowId))
            {
                Logger.Log("QosMarking", $"{what}: QOSAddSocketToFlow failed ({Marshal.GetLastWin32Error()})");
                return null;
            }
            var value = dscp;
            if (!QOSSetFlow(_handle, flowId, QosSetFlow.OutgoingDscpValue, sizeof(uint), ref value, 0, IntPtr.Zero))
            {
                Logger.Log("QosMarking", $"{what}: QOSSetFlow(DSCP {dscp}) failed ({Marshal.GetLastWin32Error()})");
                QOSRemoveSocketFromFlow(_handle, IntPtr.Zero, flowId, 0);
                return null;
            }
            Logger.Log("QosMarking", $"{what} to {remote}: DSCP {dscp}");
            return new Flow(flowId);
        }
    }

    private static void Fail(string reason)
    {
        _failed = true;
        Logger.Log("QosMarking", $"traffic priority unavailable: {reason}");
    }

    private sealed class Flow(uint flowId) : IDisposable
    {
        private bool _disposed;

        public void Dispose()
        {
            lock (Lock)
            {
                if (_disposed || _handle == IntPtr.Zero) return;
                _disposed = true;
                QOSRemoveSocketFromFlow(_handle, IntPtr.Zero, flowId, 0);
            }
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct QosVersion
    {
        public ushort Major;
        public ushort Minor;
    }

    private enum QosTrafficType
    {
        BestEffort = 0,
        Background = 1,
        ExcellentEffort = 2,
        AudioVideo = 3,
        Voice = 4,
        Control = 5,
    }

    private enum QosSetFlow
    {
        TrafficType = 0,
        OutgoingRate = 1,
        OutgoingDscpValue = 2,
    }

    private const uint QosNonAdaptiveFlow = 0x00000002;

    [DllImport("qwave.dll", SetLastError = true)]
    private static extern bool QOSCreateHandle(ref QosVersion version, out IntPtr handle);

    [DllImport("qwave.dll", SetLastError = true)]
    private static extern bool QOSAddSocketToFlow(IntPtr handle, IntPtr socket, IntPtr destAddr, QosTrafficType trafficType, uint flags, ref uint flowId);

    [DllImport("qwave.dll", SetLastError = true)]
    private static extern bool QOSSetFlow(IntPtr handle, uint flowId, QosSetFlow operation, uint size, ref uint buffer, uint flags, IntPtr overlapped);

    [DllImport("qwave.dll", SetLastError = true)]
    private static extern bool QOSRemoveSocketFromFlow(IntPtr handle, IntPtr socket, uint flowId, uint flags);
}
