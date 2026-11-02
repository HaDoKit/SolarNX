namespace SolarNX.Audio;

/// <summary>
/// Minimal RTP (RFC 3550)-inspired packetizer - a plain 12-byte header (no padding, no extension,
/// no CSRC list) followed by a 2-byte big-endian length prefix and then the payload (one Opus frame).
///
/// The length prefix is NOT part of real RTP - it's a deliberate deviation, needed because multiple
/// UDP sends made close together in time can arrive at the Switch client coalesced into a single
/// read (see SolarNX/source/audio.c's top doc comment). Bare RTP/Opus has no self-contained length
/// field, so once datagram boundaries can't be trusted to survive the trip, this prefix is what
/// lets the client walk a single read as however many complete [header][length][payload] units
/// actually fit in it.
///
/// Kept separate from AudioCaptureSender so the header-writing logic is independently readable
/// (and testable) rather than buried in the send loop.
/// </summary>
public sealed class RtpPacketizer
{
    private const int HeaderBytes = 12;
    private const int LengthPrefixBytes = 2;

    private readonly byte _payloadType;
    private readonly uint _ssrc;
    private ushort _sequenceNumber;
    private uint _timestamp;

    public RtpPacketizer(byte payloadType)
    {
        _payloadType = payloadType;
        _ssrc = (uint)Random.Shared.Next();
        _sequenceNumber = (ushort)Random.Shared.Next(ushort.MaxValue);
        _timestamp = (uint)Random.Shared.Next();
    }

    /// <param name="payload">The already-encoded Opus frame.</param>
    /// <param name="samplesPerChannel">How many samples this frame covers, at the clock rate the
    /// receiver expects the RTP timestamp in - RFC 7587 mandates 48000Hz for Opus regardless of the
    /// codec's actual internal processing rate, which happens to already match this app's
    /// AudioSampleRate default, so no separate conversion is needed here. Advances the timestamp
    /// for the packet after this one.</param>
    public byte[] Wrap(ReadOnlySpan<byte> payload, int samplesPerChannel)
    {
        if (payload.Length > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(payload), "Opus frame too large for the 16-bit length prefix - shouldn't happen at any bitrate this app actually uses.");

        var packet = new byte[HeaderBytes + LengthPrefixBytes + payload.Length];
        packet[0] = 0x80; // V=2, P=0, X=0, CC=0
        packet[1] = _payloadType; // M=0, PT=payloadType
        packet[2] = (byte)(_sequenceNumber >> 8);
        packet[3] = (byte)_sequenceNumber;
        packet[4] = (byte)(_timestamp >> 24);
        packet[5] = (byte)(_timestamp >> 16);
        packet[6] = (byte)(_timestamp >> 8);
        packet[7] = (byte)_timestamp;
        packet[8] = (byte)(_ssrc >> 24);
        packet[9] = (byte)(_ssrc >> 16);
        packet[10] = (byte)(_ssrc >> 8);
        packet[11] = (byte)_ssrc;
        packet[12] = (byte)(payload.Length >> 8);
        packet[13] = (byte)payload.Length;
        payload.CopyTo(packet.AsSpan(HeaderBytes + LengthPrefixBytes));

        _sequenceNumber++;
        _timestamp += (uint)samplesPerChannel;
        return packet;
    }
}
