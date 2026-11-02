#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Reed-Solomon erasure code over GF(2^8) for the video stream's forward error correction - the
// receiving half of SolarNX/Capture/ReedSolomon.cs on the PC. Both must compute byte-for-byte the
// same parity, so the field (polynomial 0x11D) and the coefficient formula below may only change
// together with that file.
//
// A block is dataCount data shards followed by parityCount parity shards, all shardSize bytes.
// Parity shard p = sum over k of C(p, k) * data shard k, with the Cauchy coefficient
// C(p, k) = 1 / ((255 - p) xor k). Every square submatrix of a Cauchy matrix is invertible, so ANY
// dataCount of the dataCount + parityCount shards are enough to rebuild the block - up to
// parityCount lost shards per block, wherever they are.

#define RS_MAX_SHARDS 255 // dataCount + parityCount
#define RS_MAX_PARITY 64

// Builds the field tables. Call once before anything else here.
void rsInit(void);

// Rebuilds the missing data shards in place. shards[0..dataCount) are the data shards (a missing
// one must still point at a writable shardSize buffer - it receives the rebuilt bytes), then the
// parity shards; present[i] says which arrived. Only data shards are rebuilt. The parity shards
// used for that are overwritten (they are scratch space afterwards). Returns false when fewer than
// dataCount shards arrived, or the block's shape is out of range - nothing is rebuilt then.
// Not reentrant (static scratch matrices) - the video decode thread is its only caller.
bool rsReconstruct(uint8_t *const *shards, const bool *present, int dataCount, int parityCount, size_t shardSize);

// Computes the parity shards from the data shards - only needed by the tests (the PC encodes).
void rsEncode(const uint8_t *const *data, int dataCount, uint8_t *const *parity, int parityCount, size_t shardSize);
