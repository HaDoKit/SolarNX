#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Cryptography of the Internet mode - mirrors SolarNX/Session/InternetCrypto.cs byte for byte
// (change both together): X25519 key agreement (TweetNaCl, public domain), HKDF-SHA256, and packets
// sealed with AES-256-CTR + HMAC-SHA256 truncated to 16 bytes (encrypt-then-MAC). AES, SHA-256 and
// HMAC come from libnx (hardware accelerated).

#define CRYPTO_KEY_SIZE     32
#define CRYPTO_NONCE_SIZE   16
#define CRYPTO_TAG_SIZE     16
#define CRYPTO_COUNTER_SIZE 8
#define CRYPTO_OVERHEAD     (CRYPTO_COUNTER_SIZE + CRYPTO_TAG_SIZE)

void cryptoRandom(void *out, size_t size);

// out = scalar * point (X25519). cryptoKeyPair makes a fresh private key and its public key.
void cryptoScalarMult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void cryptoKeyPair(uint8_t privateKey[32], uint8_t publicKey[32]);

// HKDF-SHA256 (RFC 5869).
void cryptoHkdf(uint8_t *out, size_t outSize, const void *ikm, size_t ikmSize,
                const void *salt, size_t saltSize, const char *info);

// SHA256(pcPublic || switchPublic || pcNonce) - the PC's commitment during pairing.
void cryptoPairCommit(uint8_t out[32], const uint8_t pcPublic[32], const uint8_t switchPublic[32], const uint8_t pcNonce[16]);
// The 6-digit code both screens show.
uint32_t cryptoPairCode(const uint8_t switchPublic[32], const uint8_t pcPublic[32], const uint8_t switchNonce[16], const uint8_t pcNonce[16]);
void cryptoPairKey(uint8_t out[32], const uint8_t shared[32], const uint8_t switchNonce[16], const uint8_t pcNonce[16]);
void cryptoPairProof(uint8_t out[32], const uint8_t pairKey[32], const uint8_t deviceId[16], const uint8_t hostId[8]);

typedef struct {
    uint8_t encSwitchToPc[32];
    uint8_t macSwitchToPc[32];
    uint8_t encPcToSwitch[32];
    uint8_t macPcToSwitch[32];
} CryptoSessionKeys;

void cryptoSessionKeys(CryptoSessionKeys *out, const uint8_t pairKey[32], const uint8_t switchNonce[16], const uint8_t pcNonce[16]);

// packet = aad || counter (LE) || AES-CTR(plaintext) || tag. Returns the packet size;
// out must hold aadSize + plaintextSize + CRYPTO_OVERHEAD.
size_t cryptoSeal(uint8_t *out, const uint8_t encKey[32], const uint8_t macKey[32], uint64_t counter,
                  const void *aad, size_t aadSize, const void *plaintext, size_t plaintextSize);

// Verifies and decrypts in place-free fashion into out (packetSize - aadSize - CRYPTO_OVERHEAD
// bytes). Returns the plaintext size, or -1 if the tag doesn't match.
long cryptoOpen(uint8_t *out, const uint8_t encKey[32], const uint8_t macKey[32],
                const uint8_t *packet, size_t packetSize, size_t aadSize, uint64_t *counter);

// Accepts each counter once, within 64 of the highest seen.
typedef struct {
    uint64_t highest;
    uint64_t seen;
} CryptoReplayWindow;

bool cryptoReplayAccept(CryptoReplayWindow *window, uint64_t counter);
