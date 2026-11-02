#include "crypto.h"

#include <string.h>

#ifdef CRYPTO_HOST_TEST
#include "crypto_host_shim.h" // PC-side test build only (not in the repo) - libnx stand-ins
#else
#include <switch.h>
#endif

void cryptoRandom(void *out, size_t size)
{
    // csrng is the system's CSPRNG; randomGet (kernel-entropy seeded ChaCha) if it isn't available.
    static bool csrngReady = false;
    if (!csrngReady)
        csrngReady = R_SUCCEEDED(csrngInitialize());
    if (!csrngReady || R_FAILED(csrngGetRandomBytes(out, size)))
        randomGet(out, size);
}

// --- X25519: TweetNaCl's crypto_scalarmult, 16 limbs of 16 bits ---

typedef int64_t gf[16];
static const gf kA24 = {0xDB41, 1};
static const uint8_t kBasePoint[32] = {9};

static void car25519(gf o)
{
    for (int i = 0; i < 16; i++)
    {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        if (i < 15)
            o[i + 1] += c - 1;
        else
            o[0] += 38 * (c - 1);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b)
{
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++)
    {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n)
{
    gf m, t;
    for (int i = 0; i < 16; i++)
        t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; j++)
    {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++)
        {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++)
    {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack25519(gf o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++)
        o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++)
        o[i] = a[i] + b[i];
}

static void fsub(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++)
        o[i] = a[i] - b[i];
}

static void fmul(gf o, const gf a, const gf b)
{
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++)
        t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++)
        o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void inv25519(gf o, const gf in)
{
    gf c;
    for (int a = 0; a < 16; a++)
        c[a] = in[a];
    for (int a = 253; a >= 0; a--)
    {
        fmul(c, c, c);
        if (a != 2 && a != 4)
            fmul(c, c, in);
    }
    for (int a = 0; a < 16; a++)
        o[a] = c[a];
}

void cryptoScalarMult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t z[32];
    gf x, a, b, c, d, e, f;
    memcpy(z, scalar, 32);
    z[31] = (scalar[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, point);
    for (int i = 0; i < 16; i++)
    {
        b[i] = x[i];
        a[i] = c[i] = d[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i)
    {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        fadd(e, a, c);
        fsub(a, a, c);
        fadd(c, b, d);
        fsub(b, b, d);
        fmul(d, e, e);
        fmul(f, a, a);
        fmul(a, c, a);
        fmul(c, b, e);
        fadd(e, a, c);
        fsub(a, a, c);
        fmul(b, a, a);
        fsub(c, d, f);
        fmul(a, c, kA24);
        fadd(a, a, d);
        fmul(c, c, a);
        fmul(a, d, f);
        fmul(d, b, x);
        fmul(b, e, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    gf inverse, result;
    inv25519(inverse, c);
    fmul(result, a, inverse);
    pack25519(out, result);
}

void cryptoKeyPair(uint8_t privateKey[32], uint8_t publicKey[32])
{
    cryptoRandom(privateKey, 32);
    cryptoScalarMult(publicKey, privateKey, kBasePoint);
}

// --- Derivation ---

void cryptoHkdf(uint8_t *out, size_t outSize, const void *ikm, size_t ikmSize,
                const void *salt, size_t saltSize, const char *info)
{
    uint8_t prk[32];
    hmacSha256CalculateMac(prk, salt, saltSize, ikm, ikmSize);

    uint8_t t[32];
    size_t tSize = 0;
    size_t done = 0;
    for (uint8_t counter = 1; done < outSize; counter++)
    {
        HmacSha256Context ctx;
        hmacSha256ContextCreate(&ctx, prk, sizeof(prk));
        hmacSha256ContextUpdate(&ctx, t, tSize);
        hmacSha256ContextUpdate(&ctx, info, strlen(info));
        hmacSha256ContextUpdate(&ctx, &counter, 1);
        hmacSha256ContextGetMac(&ctx, t);
        tSize = 32;
        size_t n = outSize - done < 32 ? outSize - done : 32;
        memcpy(out + done, t, n);
        done += n;
    }
}

void cryptoPairCommit(uint8_t out[32], const uint8_t pcPublic[32], const uint8_t switchPublic[32], const uint8_t pcNonce[16])
{
    uint8_t data[32 + 32 + 16];
    memcpy(data, pcPublic, 32);
    memcpy(data + 32, switchPublic, 32);
    memcpy(data + 64, pcNonce, 16);
    sha256CalculateHash(out, data, sizeof(data));
}

uint32_t cryptoPairCode(const uint8_t switchPublic[32], const uint8_t pcPublic[32], const uint8_t switchNonce[16], const uint8_t pcNonce[16])
{
    static const char label[] = "SolarNX pair code";
    uint8_t data[sizeof(label) - 1 + 32 + 32 + 16 + 16];
    size_t o = 0;
    memcpy(data + o, label, sizeof(label) - 1);
    o += sizeof(label) - 1;
    memcpy(data + o, switchPublic, 32);
    o += 32;
    memcpy(data + o, pcPublic, 32);
    o += 32;
    memcpy(data + o, switchNonce, 16);
    o += 16;
    memcpy(data + o, pcNonce, 16);
    uint8_t hash[32];
    sha256CalculateHash(hash, data, sizeof(data));
    uint32_t value;
    memcpy(&value, hash, 4); // little-endian, like the PC
    return value % 1000000u;
}

void cryptoPairKey(uint8_t out[32], const uint8_t shared[32], const uint8_t switchNonce[16], const uint8_t pcNonce[16])
{
    uint8_t salt[32];
    memcpy(salt, switchNonce, 16);
    memcpy(salt + 16, pcNonce, 16);
    cryptoHkdf(out, 32, shared, 32, salt, sizeof(salt), "SolarNX pair key");
}

void cryptoPairProof(uint8_t out[32], const uint8_t pairKey[32], const uint8_t deviceId[16], const uint8_t hostId[8])
{
    static const char label[] = "SolarNX pair ok";
    uint8_t data[sizeof(label) - 1 + 16 + 8];
    memcpy(data, label, sizeof(label) - 1);
    memcpy(data + sizeof(label) - 1, deviceId, 16);
    memcpy(data + sizeof(label) - 1 + 16, hostId, 8);
    hmacSha256CalculateMac(out, pairKey, 32, data, sizeof(data));
}

void cryptoSessionKeys(CryptoSessionKeys *out, const uint8_t pairKey[32], const uint8_t switchNonce[16], const uint8_t pcNonce[16])
{
    uint8_t salt[32];
    memcpy(salt, switchNonce, 16);
    memcpy(salt + 16, pcNonce, 16);
    uint8_t okm[128];
    cryptoHkdf(okm, sizeof(okm), pairKey, 32, salt, sizeof(salt), "SolarNX session");
    memcpy(out->encSwitchToPc, okm, 32);
    memcpy(out->macSwitchToPc, okm + 32, 32);
    memcpy(out->encPcToSwitch, okm + 64, 32);
    memcpy(out->macPcToSwitch, okm + 96, 32);
}

// --- Packets ---

static void aesCtr(const uint8_t key[32], uint64_t counter, const uint8_t *in, uint8_t *out, size_t size)
{
    uint8_t iv[16] = {0};
    for (int i = 0; i < 8; i++)
        iv[i] = (uint8_t)(counter >> (56 - 8 * i)); // big-endian; the low 8 bytes count blocks
    Aes256CtrContext ctx;
    aes256CtrContextCreate(&ctx, key, iv);
    aes256CtrCrypt(&ctx, out, in, size);
}

size_t cryptoSeal(uint8_t *out, const uint8_t encKey[32], const uint8_t macKey[32], uint64_t counter,
                  const void *aad, size_t aadSize, const void *plaintext, size_t plaintextSize)
{
    memcpy(out, aad, aadSize);
    memcpy(out + aadSize, &counter, CRYPTO_COUNTER_SIZE); // little-endian
    aesCtr(encKey, counter, (const uint8_t *)plaintext, out + aadSize + CRYPTO_COUNTER_SIZE, plaintextSize);
    size_t macked = aadSize + CRYPTO_COUNTER_SIZE + plaintextSize;
    uint8_t mac[32];
    hmacSha256CalculateMac(mac, macKey, 32, out, macked);
    memcpy(out + macked, mac, CRYPTO_TAG_SIZE);
    return macked + CRYPTO_TAG_SIZE;
}

long cryptoOpen(uint8_t *out, const uint8_t encKey[32], const uint8_t macKey[32],
                const uint8_t *packet, size_t packetSize, size_t aadSize, uint64_t *counter)
{
    if (packetSize < aadSize + CRYPTO_OVERHEAD)
        return -1;
    size_t macked = packetSize - CRYPTO_TAG_SIZE;
    uint8_t mac[32];
    hmacSha256CalculateMac(mac, macKey, 32, packet, macked);
    uint8_t diff = 0;
    for (int i = 0; i < CRYPTO_TAG_SIZE; i++)
        diff |= mac[i] ^ packet[macked + i];
    if (diff != 0)
        return -1;
    memcpy(counter, packet + aadSize, CRYPTO_COUNTER_SIZE);
    size_t size = macked - aadSize - CRYPTO_COUNTER_SIZE;
    aesCtr(encKey, *counter, packet + aadSize + CRYPTO_COUNTER_SIZE, out, size);
    return (long)size;
}

bool cryptoReplayAccept(CryptoReplayWindow *window, uint64_t counter)
{
    if (counter == 0)
        return false;
    if (counter > window->highest)
    {
        uint64_t shift = counter - window->highest;
        window->seen = shift >= 64 ? 0 : window->seen << shift;
        window->seen |= 1;
        window->highest = counter;
        return true;
    }
    uint64_t back = window->highest - counter;
    if (back >= 64)
        return false;
    uint64_t bit = (uint64_t)1 << back;
    if (window->seen & bit)
        return false;
    window->seen |= bit;
    return true;
}
