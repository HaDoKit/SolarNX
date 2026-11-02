using System.Buffers.Binary;

namespace SolarNX.Input;

/// <summary>
/// One decoded controller's state from the SolarNX HID packet.
/// </summary>
public readonly struct ControllerState
{
    public required ushort HeldKeys { get; init; }
    public required int LJoyX { get; init; }
    public required int LJoyY { get; init; }
    public required int RJoyX { get; init; }
    public required int RJoyY { get; init; }

    /// <summary>Real acceleration (g) and angular velocity (rad/s, NOT an accumulated angle) from
    /// this controller's own motion sensor, per the client's libnx HidSixAxisSensorState. Each
    /// controller now carries its own reading - an earlier protocol version had a single shared
    /// motion block applied to controller index 0 only ("the primary player"); see
    /// VirtualGamepadHub.ApplyDs4State, the only consumer of this today.</summary>
    public required float AccelX { get; init; }
    public required float AccelY { get; init; }
    public required float AccelZ { get; init; }
    public required float GyroX { get; init; }
    public required float GyroY { get; init; }
    public required float GyroZ { get; init; }

    public bool A => IsSet(0);
    public bool B => IsSet(1);
    public bool X => IsSet(2);
    public bool Y => IsSet(3);
    public bool LS => IsSet(4);
    public bool RS => IsSet(5);
    public bool L => IsSet(6);
    public bool R => IsSet(7);
    public bool ZL => IsSet(8);
    public bool ZR => IsSet(9);
    public bool Plus => IsSet(10);
    public bool Minus => IsSet(11);
    public bool Left => IsSet(12);
    public bool Up => IsSet(13);
    public bool Right => IsSet(14);
    public bool Down => IsSet(15);

    private bool IsSet(int bit) => ((HeldKeys >> bit) & 1) != 0;
}

/// <summary>Which app the Switch client wants the PC host to launch/focus, chosen once via the
/// on-screen menu (menu.c) before the input thread starts and sent unchanged on every JoyPkg for
/// the rest of the process's life. Wire value carried in JoyPkg.appTarget (see network.h on the
/// client). Declared as a ulong-backed enum to mirror the wire field's uint64_t type exactly - see
/// network.h's comment on why that field is a uint64_t rather than uint32_t (alignment/padding).</summary>
public enum AppTarget : ulong
{
    BigPicture = 0,
    ShadPs4 = 1,

    /// <summary>A game from the host-side library (Library/GameLibrary) - which one is in
    /// <see cref="HidPacket.LibraryEntryId"/>.</summary>
    Library = 2,
}

/// <summary>
/// Sent once by this host, immediately after the HID TCP connection is accepted and before any
/// HidPacket traffic - lets the client size its video/audio receive buffers (fifo_size, etc.) from
/// this session's real configured values instead of guessing a default or measuring the stream
/// after the fact. Mirrors the client's SolarNxSessionConfig (network.h) field-for-field: 7x
/// uint32 little-endian, 28 bytes total, no framing/marker needed since it's always the first 28
/// bytes written to a freshly-accepted connection.
/// </summary>
public sealed class SessionConfig
{
    public const int WireSize = 28;

    public required uint VideoBitrateKbps { get; init; }
    public required uint VideoWidth { get; init; }
    public required uint VideoHeight { get; init; }
    public required uint VideoFps { get; init; }
    public required uint AudioBitrateKbps { get; init; }
    public required uint AudioSampleRate { get; init; }
    public required uint AudioChannels { get; init; }

    public byte[] ToBytes()
    {
        var buffer = new byte[WireSize];
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(0), VideoBitrateKbps);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(4), VideoWidth);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(8), VideoHeight);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(12), VideoFps);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(16), AudioBitrateKbps);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(20), AudioSampleRate);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(24), AudioChannels);
        return buffer;
    }
}

public sealed class HidPacket
{
    public const int PayloadSize = 224;

    public required ControllerState[] Controllers { get; init; }
    public required uint TouchX1 { get; init; }
    public required uint TouchY1 { get; init; }
    public required uint ControllerCount { get; init; }
    public required AppTarget AppTarget { get; init; }

    /// <summary>GameEntry.Id picked on the Switch when <see cref="AppTarget"/> is Library (0
    /// otherwise). Carried in the slot that used to be the never-read frameRate field, so the packet
    /// size and every other offset are unchanged.</summary>
    public required uint LibraryEntryId { get; init; }

    /// <summary>Link-quality telemetry, free-running counters/last-known-values from the client -
    /// see the client's network.h doc comment on SolarNxHidPacket's matching fields. Consumed by
    /// <see cref="SolarNX.Session.AdaptiveBitrateController"/>, which diffs successive packets
    /// rather than reading these as absolute values (see that class's doc comment for why).</summary>
    public required uint VideoReconnectCount { get; init; }
    public required uint VideoMeasuredBitrateKbps { get; init; }
    public required uint AudioReconnectCount { get; init; }
    public required uint AudioConcealedFrameCount { get; init; }

    /// <summary>Video frames the Switch could not rebuild from the parity (it then waits for the
    /// next keyframe) and datagrams it did rebuild - free-running counters from the client's video
    /// reassembly (Client/video/reassembly.h). On the wire where touchX2/Y2 used to be.</summary>
    public required uint VideoLostFrameCount { get; init; }
    public required uint VideoRecoveredPacketCount { get; init; }

    /// <summary>
    /// Mirrors the client's SolarNxHidPacket (network.h) field-for-field: 4x controller blocks
    /// (HeldKeys u32, LJoyX/Y i32, RJoyX/Y i32, accelX/Y/Z f32, gyroX/Y/Z f32 - 44 bytes each),
    /// touchX1,Y1 (u32), videoLostFrameCount, videoRecoveredPacketCount (u32, formerly touchX2/Y2),
    /// libraryEntryId (u32, formerly frameRate), controllerCount (u32),
    /// appTarget (u64), then the 4 telemetry u32 fields above (16 bytes).
    /// </summary>
    public static HidPacket Parse(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != PayloadSize)
            throw new ArgumentException($"Expected {PayloadSize} bytes, got {payload.Length}");

        var controllers = new ControllerState[4];
        var offset = 0;
        for (var i = 0; i < 4; i++)
        {
            var heldKeys = BinaryPrimitives.ReadUInt32LittleEndian(payload[offset..]);
            var lJoyX = BinaryPrimitives.ReadInt32LittleEndian(payload[(offset + 4)..]);
            var lJoyY = BinaryPrimitives.ReadInt32LittleEndian(payload[(offset + 8)..]);
            var rJoyX = BinaryPrimitives.ReadInt32LittleEndian(payload[(offset + 12)..]);
            var rJoyY = BinaryPrimitives.ReadInt32LittleEndian(payload[(offset + 16)..]);
            var accelX = BinaryPrimitives.ReadSingleLittleEndian(payload[(offset + 20)..]);
            var accelY = BinaryPrimitives.ReadSingleLittleEndian(payload[(offset + 24)..]);
            var accelZ = BinaryPrimitives.ReadSingleLittleEndian(payload[(offset + 28)..]);
            var gyroX = BinaryPrimitives.ReadSingleLittleEndian(payload[(offset + 32)..]);
            var gyroY = BinaryPrimitives.ReadSingleLittleEndian(payload[(offset + 36)..]);
            var gyroZ = BinaryPrimitives.ReadSingleLittleEndian(payload[(offset + 40)..]);
            controllers[i] = new ControllerState
            {
                HeldKeys = (ushort)heldKeys,
                LJoyX = lJoyX,
                LJoyY = lJoyY,
                RJoyX = rJoyX,
                RJoyY = rJoyY,
                AccelX = accelX,
                AccelY = accelY,
                AccelZ = accelZ,
                GyroX = gyroX,
                GyroY = gyroY,
                GyroZ = gyroZ,
            };
            offset += 44;
        }

        var touchX1 = BinaryPrimitives.ReadUInt32LittleEndian(payload[offset..]);
        var touchY1 = BinaryPrimitives.ReadUInt32LittleEndian(payload[(offset + 4)..]);
        var videoLostFrameCount = BinaryPrimitives.ReadUInt32LittleEndian(payload[(offset + 8)..]);
        var videoRecoveredPacketCount = BinaryPrimitives.ReadUInt32LittleEndian(payload[(offset + 12)..]);
        offset += 16;

        var libraryEntryId = BinaryPrimitives.ReadUInt32LittleEndian(payload[offset..]);
        var controllerCount = BinaryPrimitives.ReadUInt32LittleEndian(payload[(offset + 4)..]);
        offset += 8;

        var appTargetRaw = BinaryPrimitives.ReadUInt64LittleEndian(payload[offset..]);
        // Defensive default: anything other than a known value (e.g. a mismatched/older client
        // build) falls back to Big Picture rather than an undefined enum value propagating further.
        var appTarget = appTargetRaw is (ulong)AppTarget.BigPicture or (ulong)AppTarget.ShadPs4 or (ulong)AppTarget.Library
            ? (AppTarget)appTargetRaw
            : AppTarget.BigPicture;
        offset += 8;

        var videoReconnectCount = BinaryPrimitives.ReadUInt32LittleEndian(payload[offset..]);
        var videoMeasuredBitrateKbps = BinaryPrimitives.ReadUInt32LittleEndian(payload[(offset + 4)..]);
        var audioReconnectCount = BinaryPrimitives.ReadUInt32LittleEndian(payload[(offset + 8)..]);
        var audioConcealedFrameCount = BinaryPrimitives.ReadUInt32LittleEndian(payload[(offset + 12)..]);

        return new HidPacket
        {
            Controllers = controllers,
            TouchX1 = touchX1,
            TouchY1 = touchY1,
            ControllerCount = controllerCount,
            AppTarget = appTarget,
            LibraryEntryId = libraryEntryId,
            VideoReconnectCount = videoReconnectCount,
            VideoMeasuredBitrateKbps = videoMeasuredBitrateKbps,
            AudioReconnectCount = audioReconnectCount,
            AudioConcealedFrameCount = audioConcealedFrameCount,
            VideoLostFrameCount = videoLostFrameCount,
            VideoRecoveredPacketCount = videoRecoveredPacketCount,
        };
    }
}

/// <summary>
/// Extracts framed 224-byte HID payloads out of a raw byte stream.
/// The client wraps each frame with an 8x0xFF start marker followed by the fixed-size payload;
/// the original main.js end-marker is a substring match on a hex string, not byte-aligned, so instead
/// of reproducing that exact (ambiguous) pattern we rely on the unambiguous start marker + known
/// payload length and resync on the next start marker for whatever trailing bytes follow.
/// NOTE: verify this framing against a live capture from the real client before relying on it.
/// </summary>
public sealed class HidFrameReader
{
    private static readonly byte[] StartMarker = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    private readonly List<byte> _buffer = new();

    // Takes a plain array/length rather than Span<byte>: iterator methods (this uses yield)
    // cannot declare or use ref struct types like Span<T> anywhere in their body.
    public IEnumerable<HidPacket> Feed(byte[] chunk, int length)
    {
        for (var i = 0; i < length; i++) _buffer.Add(chunk[i]);

        while (true)
        {
            var startIdx = IndexOf(_buffer, StartMarker, 0);
            if (startIdx < 0)
            {
                // Keep only a start-marker's worth of tail in case it's split across chunks.
                if (_buffer.Count > StartMarker.Length)
                    _buffer.RemoveRange(0, _buffer.Count - StartMarker.Length);
                yield break;
            }

            var payloadStart = startIdx + StartMarker.Length;
            if (_buffer.Count < payloadStart + HidPacket.PayloadSize)
            {
                _buffer.RemoveRange(0, startIdx);
                yield break;
            }

            var payload = _buffer.GetRange(payloadStart, HidPacket.PayloadSize).ToArray();
            yield return HidPacket.Parse(payload);

            _buffer.RemoveRange(0, payloadStart + HidPacket.PayloadSize);
        }
    }

    private static int IndexOf(List<byte> haystack, byte[] needle, int from)
    {
        for (var i = from; i <= haystack.Count - needle.Length; i++)
        {
            var match = true;
            for (var j = 0; j < needle.Length; j++)
            {
                if (haystack[i + j] != needle[j]) { match = false; break; }
            }
            if (match) return i;
        }
        return -1;
    }
}
