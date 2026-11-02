using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace SolarNX.Session;

/// <summary>
/// Cryptography of the Internet mode - mirrored by hand in Client/transport/crypto.c (same
/// algorithms, constants and byte layouts; change both together).
///
/// Key agreement: X25519 (RFC 7748), a port of TweetNaCl's crypto_scalarmult (public domain) - .NET
/// has no X25519, and the Switch side uses the very same code, verified against RFC 7748 test
/// vectors. Key derivation: HKDF-SHA256. Packets: AES-256-CTR, then HMAC-SHA256 over the header,
/// counter and ciphertext, truncated to 16 bytes (encrypt-then-MAC). The CTR IV is the packet counter,
/// big-endian, in the first 8 bytes, the low 8 bytes counting the 16-byte blocks. A per-direction
/// 64-packet window rejects replays.
/// </summary>
internal static class InternetCrypto
{
    public const int KeySize = 32;
    public const int NonceSize = 16;
    public const int TagSize = 16;
    public const int CounterSize = 8;
    public const int Overhead = CounterSize + TagSize;

    public static byte[] RandomBytes(int count) => RandomNumberGenerator.GetBytes(count);

    // --- X25519 (TweetNaCl crypto_scalarmult, 16 limbs of 16 bits in longs) ---

    private static readonly byte[] BasePoint = CreateBasePoint();
    private static readonly long[] A24 = { 0xDB41, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    private static byte[] CreateBasePoint()
    {
        var b = new byte[32];
        b[0] = 9;
        return b;
    }

    public static (byte[] Private, byte[] Public) NewKeyPair()
    {
        var priv = RandomBytes(KeySize);
        return (priv, ScalarMult(priv, BasePoint));
    }

    public static byte[] SharedSecret(byte[] ownPrivate, byte[] peerPublic) => ScalarMult(ownPrivate, peerPublic);

    public static byte[] ScalarMult(byte[] scalar, byte[] point)
    {
        var z = new byte[32];
        Array.Copy(scalar, z, 32);
        z[31] = (byte)((scalar[31] & 127) | 64);
        z[0] &= 248;

        var x = new long[80];
        Unpack(x, point);
        long[] a = new long[16], b = new long[16], c = new long[16], d = new long[16], e = new long[16], f = new long[16];
        for (var i = 0; i < 16; i++)
        {
            b[i] = x[i];
            a[i] = c[i] = d[i] = 0;
        }
        a[0] = d[0] = 1;

        for (var i = 254; i >= 0; --i)
        {
            var r = (z[i >> 3] >> (i & 7)) & 1;
            Sel(a, b, r);
            Sel(c, d, r);
            Add(e, a, c);
            Sub(a, a, c);
            Add(c, b, d);
            Sub(b, b, d);
            Mul(d, e, e);
            Mul(f, a, a);
            Mul(a, c, a);
            Mul(c, b, e);
            Add(e, a, c);
            Sub(a, a, c);
            Mul(b, a, a);
            Sub(c, d, f);
            Mul(a, c, A24);
            Add(a, a, d);
            Mul(c, c, a);
            Mul(a, d, f);
            Mul(d, b, x);
            Mul(b, e, e);
            Sel(a, b, r);
            Sel(c, d, r);
        }

        var inv = new long[16];
        Inverse(inv, c);
        var result = new long[16];
        Mul(result, a, inv);
        var output = new byte[32];
        Pack(output, result);
        return output;
    }

    private static void Carry(long[] o)
    {
        for (var i = 0; i < 16; i++)
        {
            o[i] += 1L << 16;
            var c = o[i] >> 16;
            if (i < 15) o[i + 1] += c - 1;
            else o[0] += 38 * (c - 1);
            o[i] -= c << 16;
        }
    }

    private static void Sel(long[] p, long[] q, int b)
    {
        var c = ~((long)b - 1);
        for (var i = 0; i < 16; i++)
        {
            var t = c & (p[i] ^ q[i]);
            p[i] ^= t;
            q[i] ^= t;
        }
    }

    private static void Pack(byte[] o, long[] n)
    {
        long[] m = new long[16], t = new long[16];
        Array.Copy(n, t, 16);
        Carry(t);
        Carry(t);
        Carry(t);
        for (var j = 0; j < 2; j++)
        {
            m[0] = t[0] - 0xffed;
            for (var i = 1; i < 15; i++)
            {
                m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
                m[i - 1] &= 0xffff;
            }
            m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
            var b = (int)((m[15] >> 16) & 1);
            m[14] &= 0xffff;
            Sel(t, m, 1 - b);
        }
        for (var i = 0; i < 16; i++)
        {
            o[2 * i] = (byte)(t[i] & 0xff);
            o[2 * i + 1] = (byte)(t[i] >> 8);
        }
    }

    private static void Unpack(long[] o, byte[] n)
    {
        for (var i = 0; i < 16; i++) o[i] = n[2 * i] + ((long)n[2 * i + 1] << 8);
        o[15] &= 0x7fff;
    }

    private static void Add(long[] o, long[] a, long[] b)
    {
        for (var i = 0; i < 16; i++) o[i] = a[i] + b[i];
    }

    private static void Sub(long[] o, long[] a, long[] b)
    {
        for (var i = 0; i < 16; i++) o[i] = a[i] - b[i];
    }

    private static void Mul(long[] o, long[] a, long[] b)
    {
        var t = new long[31];
        for (var i = 0; i < 16; i++)
            for (var j = 0; j < 16; j++)
                t[i + j] += a[i] * b[j];
        for (var i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
        for (var i = 0; i < 16; i++) o[i] = t[i];
        Carry(o);
        Carry(o);
    }

    private static void Inverse(long[] o, long[] i)
    {
        var c = new long[16];
        Array.Copy(i, c, 16);
        for (var a = 253; a >= 0; a--)
        {
            Mul(c, c, c);
            if (a != 2 && a != 4) Mul(c, c, i);
        }
        Array.Copy(c, o, 16);
    }

    // --- Derivation ---

    private static byte[] Concat(params byte[][] parts)
    {
        var result = new byte[parts.Sum(p => p.Length)];
        var offset = 0;
        foreach (var p in parts)
        {
            p.CopyTo(result, offset);
            offset += p.Length;
        }
        return result;
    }

    /// <summary>What the PC commits to before it learns the Switch's nonce - stops a man in the middle
    /// from grinding keys until the two pairing codes match.</summary>
    public static byte[] PairCommit(byte[] pcPublic, byte[] switchPublic, byte[] pcNonce) =>
        SHA256.HashData(Concat(pcPublic, switchPublic, pcNonce));

    /// <summary>The 6-digit code both screens show during pairing.</summary>
    public static int PairCode(byte[] switchPublic, byte[] pcPublic, byte[] switchNonce, byte[] pcNonce)
    {
        var hash = SHA256.HashData(Concat(Encoding.ASCII.GetBytes("SolarNX pair code"), switchPublic, pcPublic, switchNonce, pcNonce));
        return (int)(BinaryPrimitives.ReadUInt32LittleEndian(hash) % 1_000_000);
    }

    public static byte[] PairKey(byte[] sharedSecret, byte[] switchNonce, byte[] pcNonce) =>
        HKDF.DeriveKey(HashAlgorithmName.SHA256, sharedSecret, KeySize, Concat(switchNonce, pcNonce), Encoding.ASCII.GetBytes("SolarNX pair key"));

    /// <summary>Proof in PairResult that the PC derived the same pairing key.</summary>
    public static byte[] PairProof(byte[] pairKey, byte[] deviceId, byte[] hostId) =>
        HMACSHA256.HashData(pairKey, Concat(Encoding.ASCII.GetBytes("SolarNX pair ok"), deviceId, hostId));

    public sealed class SessionKeys
    {
        public required byte[] EncryptSwitchToPc { get; init; }
        public required byte[] MacSwitchToPc { get; init; }
        public required byte[] EncryptPcToSwitch { get; init; }
        public required byte[] MacPcToSwitch { get; init; }
    }

    public static SessionKeys DeriveSessionKeys(byte[] pairKey, byte[] switchNonce, byte[] pcNonce)
    {
        var okm = HKDF.DeriveKey(HashAlgorithmName.SHA256, pairKey, 4 * KeySize, Concat(switchNonce, pcNonce), Encoding.ASCII.GetBytes("SolarNX session"));
        return new SessionKeys
        {
            EncryptSwitchToPc = okm[..32],
            MacSwitchToPc = okm[32..64],
            EncryptPcToSwitch = okm[64..96],
            MacPcToSwitch = okm[96..128],
        };
    }

    // --- Packets ---

    private static void AesCtr(Aes aes, ulong counter, ReadOnlySpan<byte> input, Span<byte> output)
    {
        var blocks = (input.Length + 15) / 16;
        var counterBlocks = new byte[blocks * 16];
        for (var i = 0; i < blocks; i++)
        {
            BinaryPrimitives.WriteUInt64BigEndian(counterBlocks.AsSpan(i * 16), counter);
            BinaryPrimitives.WriteUInt64BigEndian(counterBlocks.AsSpan(i * 16 + 8), (ulong)i);
        }
        var keystream = aes.EncryptEcb(counterBlocks, PaddingMode.None);
        for (var i = 0; i < input.Length; i++) output[i] = (byte)(input[i] ^ keystream[i]);
    }

    private static byte[] Tag(byte[] macKey, ReadOnlySpan<byte> authenticated)
    {
        var mac = HMACSHA256.HashData(macKey, authenticated);
        return mac[..TagSize];
    }

    /// <summary>aad (sent in clear, e.g. the packet header) + counter (LE) + AES-CTR(plaintext) + tag.</summary>
    public static byte[] Seal(byte[] encKey, byte[] macKey, ulong counter, ReadOnlySpan<byte> aad, ReadOnlySpan<byte> plaintext)
    {
        using var channel = new Channel(encKey, macKey);
        return channel.Seal(counter, aad, plaintext);
    }

    /// <summary>Checks the tag and decrypts; null if the packet was forged or damaged.</summary>
    public static byte[]? Open(byte[] encKey, byte[] macKey, ReadOnlySpan<byte> packet, int aadLength, out ulong counter)
    {
        using var channel = new Channel(encKey, macKey);
        return channel.Open(packet, aadLength, out counter);
    }

    /// <summary>One direction's keys with the AES key schedule prepared once - video alone is several
    /// hundred packets a second. Thread-safe: video, audio and the receive loop share it.</summary>
    public sealed class Channel : IDisposable
    {
        private readonly object _lock = new();
        private readonly Aes _aes;
        private readonly byte[] _macKey;

        public Channel(byte[] encKey, byte[] macKey)
        {
            _aes = Aes.Create();
            _aes.Key = encKey;
            _macKey = macKey;
        }

        public byte[] Seal(ulong counter, ReadOnlySpan<byte> aad, ReadOnlySpan<byte> plaintext)
        {
            var packet = new byte[aad.Length + CounterSize + plaintext.Length + TagSize];
            aad.CopyTo(packet);
            BinaryPrimitives.WriteUInt64LittleEndian(packet.AsSpan(aad.Length), counter);
            lock (_lock) AesCtr(_aes, counter, plaintext, packet.AsSpan(aad.Length + CounterSize, plaintext.Length));
            Tag(_macKey, packet.AsSpan(0, packet.Length - TagSize)).CopyTo(packet, packet.Length - TagSize);
            return packet;
        }

        public byte[]? Open(ReadOnlySpan<byte> packet, int aadLength, out ulong counter)
        {
            counter = 0;
            if (packet.Length < aadLength + Overhead) return null;
            var expected = Tag(_macKey, packet[..^TagSize]);
            if (!CryptographicOperations.FixedTimeEquals(expected, packet[^TagSize..])) return null;
            counter = BinaryPrimitives.ReadUInt64LittleEndian(packet[aadLength..]);
            var ciphertext = packet[(aadLength + CounterSize)..^TagSize];
            var plaintext = new byte[ciphertext.Length];
            lock (_lock) AesCtr(_aes, counter, ciphertext, plaintext);
            return plaintext;
        }

        public void Dispose() => _aes.Dispose();
    }

    /// <summary>Accepts each counter once, within 64 of the highest seen.</summary>
    public sealed class ReplayWindow
    {
        private ulong _highest;
        private ulong _seen; // bit i = (_highest - i) seen

        public bool Accept(ulong counter)
        {
            if (counter == 0) return false;
            if (counter > _highest)
            {
                var shift = counter - _highest;
                _seen = shift >= 64 ? 0 : _seen << (int)shift;
                _seen |= 1;
                _highest = counter;
                return true;
            }
            var back = _highest - counter;
            if (back >= 64) return false;
            var bit = 1UL << (int)back;
            if ((_seen & bit) != 0) return false;
            _seen |= bit;
            return true;
        }
    }
}
