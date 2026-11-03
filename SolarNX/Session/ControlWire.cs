using System.Buffers.Binary;

namespace SolarNX.Session;

/// <summary>
/// Control messages during a session, both directions (protocol 1.1, see
/// <see cref="ProtocolVersion"/>). Mirrored by hand in Client/control/control.h - change both sides
/// together.
///
/// Carried on the session's own channel so they share its fate and ordering:
///   Wi-Fi, PC → Switch:   after the 28-byte SessionConfig on the HID TCP connection, frames of
///                         u16 length (type + payload), u8 type, payload. A 0.2.4 Switch never reads
///                         past the SessionConfig, so it never sees them.
///   Wi-Fi, Switch → PC:   8×0xFE marker, u16 length, u8 type, payload - between HID packets (marker
///                         8×0xFF). <see cref="SolarNX.Input.HidFrameReader"/> tells them apart; a
///                         0.2.4 host skips the frame as noise while it looks for the next 0xFF marker.
///   Internet:             sealed <see cref="InternetWire.Type.Control"/> /
///                         <see cref="InternetWire.Type.ClientControl"/>, payload u8 type + payload.
///
/// Each side opens a session with Hello; anything else is sent only after the peer's Hello arrived -
/// that is the whole negotiation, and why a peer that never says Hello (0.2.4) only ever gets the
/// first message. Little-endian.
/// </summary>
public static class ControlWire
{
    public const int MaxPayload = 1024;

    /// <summary>The Switch → PC frame marker (HID packets use 8×0xFF).</summary>
    public static readonly byte[] ClientMarker = [0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE];

    public enum HostMessage : byte
    {
        /// <summary><see cref="PeerVersion.WireSize"/> bytes: this PC's protocol and app version.</summary>
        Hello = 1,

        /// <summary>8 bytes: per player 1-4, u8 large motor, u8 small motor (0-255, XInput meaning:
        /// large = low frequency, small = high frequency). Sent when a value changes; the Switch keeps
        /// the last levels until the next message and stops on its own when the session ends.</summary>
        Rumble = 2,

        /// <summary>4 bytes: u8 flags (<see cref="OptionFlags"/>), u8[3] zero. Sent when the Switch
        /// said Hello and whenever one of them changes in the settings.</summary>
        Options = 3,

        /// <summary>Debug mode only - what the Switch's overlay and debug menu show:
        ///   u32 configured bitrate kbps, u32 running bitrate kbps (0 = not streaming),
        ///   u16 width, u16 height (running), u8 fps, u8 running codec (0 H.265, 1 H.264),
        ///   u8 codec setting, u8 running encoder (<see cref="Capture.EncoderKind"/>),
        ///   u8 resolution setting (0 = auto, 1.. = <see cref="DebugResolutions"/> + 1),
        ///   u8 NVENC preset index (<see cref="Capture.EncoderSelection.NvencPresets"/>),
        ///   u8 NVENC tune index (<see cref="Capture.EncoderSelection.NvencTunes"/>), u8 flags
        ///   (<see cref="DebugInfoFlags"/>), u8[12] zero, then u8 length + UTF-8 encoder name.
        /// Sent when the Switch says Hello, after every encoder (re)start and after a setting change.</summary>
        DebugInfo = 4,

        /// <summary>Debug mode only: the 8 bytes of a <see cref="ClientMessage.Ping"/>, echoed back.</summary>
        Pong = 5,
    }

    [Flags]
    public enum DebugInfoFlags : byte
    {
        None = 0,
        AdaptiveBitrate = 0x01,
        TrafficPriority = 0x02,
        Nvenc = 0x04,
        AdaptivePreset = 0x08,
    }

    /// <summary>Settings the Switch's debug menu can change - <see cref="ClientMessage.SetSetting"/>.</summary>
    public enum DebugSetting : byte
    {
        BitrateKbps = 1,      // value: kbps (1000..50000)
        AdaptiveBitrate = 2,  // 0/1
        Codec = 3,            // 0 H.265, 1 H.264
        Resolution = 4,       // 0 auto, 1.. = DebugResolutions + 1
        Fps = 5,              // one of DebugFps
        NvencPreset = 6,      // index into EncoderSelection.NvencPresets
        NvencTune = 7,        // index into EncoderSelection.NvencTunes
        TrafficPriority = 8,  // 0/1
        AdaptivePreset = 9,   // 0/1 - a manual NvencPreset/NvencTune turns it off
    }

    /// <summary>The fixed resolutions of the PC settings, in order - mirrored by the Switch's menu.</summary>
    public static readonly (int Width, int Height)[] DebugResolutions = [(640, 360), (854, 480), (960, 540), (1280, 720), (1920, 1080)];

    /// <summary>The frame rates of the PC settings - mirrored by the Switch's menu.</summary>
    public static readonly int[] DebugFps = [24, 30, 45, 60];

    [Flags]
    public enum OptionFlags : byte
    {
        None = 0,
        /// <summary>Mark the Switch's own input (HID) packets with DSCP EF.</summary>
        TrafficPriority = 0x01,
        /// <summary>Debug mode: statistics overlay, debug menu, pings.</summary>
        DebugMode = 0x02,
    }

    public enum ClientMessage : byte
    {
        /// <summary><see cref="PeerVersion.WireSize"/> bytes: the Switch's protocol and app version.</summary>
        Hello = 1,

        /// <summary>Debug mode: 8 opaque bytes (the Switch's clock) - the PC answers with
        /// <see cref="HostMessage.Pong"/> right away, the Switch measures the round trip.</summary>
        Ping = 2,

        /// <summary>Debug menu: u8 <see cref="DebugSetting"/>, u8[3] zero, i32 value. The PC applies
        /// it as if changed in its settings, restarts the encoder when streaming, and answers with a
        /// fresh <see cref="HostMessage.DebugInfo"/>.</summary>
        SetSetting = 3,
    }

    /// <summary>A PC → Switch frame for the TCP stream: u16 length, u8 type, payload.</summary>
    public static byte[] TcpFrame(HostMessage type, ReadOnlySpan<byte> payload)
    {
        var frame = new byte[3 + payload.Length];
        BinaryPrimitives.WriteUInt16LittleEndian(frame, (ushort)(1 + payload.Length));
        frame[2] = (byte)type;
        payload.CopyTo(frame.AsSpan(3));
        return frame;
    }

    /// <summary>The Internet-mode sealed payload: u8 type, payload.</summary>
    public static byte[] DatagramPayload(HostMessage type, ReadOnlySpan<byte> payload)
    {
        var data = new byte[1 + payload.Length];
        data[0] = (byte)type;
        payload.CopyTo(data.AsSpan(1));
        return data;
    }
}
