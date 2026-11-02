#include "rs.h"

#include <string.h>

// GF(2^8) with the polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D), generator 2.
static uint8_t s_exp[512];
static uint8_t s_log[256];
static uint8_t s_inv[256];
// s_mul[a][b] = a * b - one 64KB table, so the per-byte inner loops are a single lookup.
static uint8_t s_mul[256][256];

void rsInit(void)
{
    unsigned x = 1;
    for (int i = 0; i < 255; i++)
    {
        s_exp[i] = (uint8_t)x;
        s_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++)
        s_exp[i] = s_exp[i - 255];

    s_inv[0] = 0; // never used: Cauchy denominators are never 0
    for (int a = 1; a < 256; a++)
        s_inv[a] = s_exp[255 - s_log[a]];

    for (int a = 0; a < 256; a++)
        for (int b = 0; b < 256; b++)
            s_mul[a][b] = (a == 0 || b == 0) ? 0 : s_exp[s_log[a] + s_log[b]];
}

static inline uint8_t coefficient(int parity, int data)
{
    return s_inv[(uint8_t)((255 - parity) ^ data)];
}

// dst ^= c * src
static void mulAdd(uint8_t *dst, const uint8_t *src, uint8_t c, size_t size)
{
    if (c == 0)
        return;
    const uint8_t *row = s_mul[c];
    for (size_t i = 0; i < size; i++)
        dst[i] ^= row[src[i]];
}

void rsEncode(const uint8_t *const *data, int dataCount, uint8_t *const *parity, int parityCount, size_t shardSize)
{
    for (int p = 0; p < parityCount; p++)
    {
        memset(parity[p], 0, shardSize);
        for (int k = 0; k < dataCount; k++)
            mulAdd(parity[p], data[k], coefficient(p, k), shardSize);
    }
}

// Inverts the n x n matrix m (row-major, stride RS_MAX_PARITY) into inv by Gauss-Jordan
// elimination. A Cauchy submatrix is always invertible, so a missing pivot means a bug.
static bool invert(uint8_t m[][RS_MAX_PARITY], uint8_t inv[][RS_MAX_PARITY], int n)
{
    for (int r = 0; r < n; r++)
    {
        memset(inv[r], 0, (size_t)n);
        inv[r][r] = 1;
    }

    for (int col = 0; col < n; col++)
    {
        int pivot = col;
        while (pivot < n && m[pivot][col] == 0)
            pivot++;
        if (pivot == n)
            return false;
        if (pivot != col)
        {
            uint8_t tmp[RS_MAX_PARITY];
            memcpy(tmp, m[col], (size_t)n); memcpy(m[col], m[pivot], (size_t)n); memcpy(m[pivot], tmp, (size_t)n);
            memcpy(tmp, inv[col], (size_t)n); memcpy(inv[col], inv[pivot], (size_t)n); memcpy(inv[pivot], tmp, (size_t)n);
        }

        uint8_t scale = s_inv[m[col][col]];
        for (int c = 0; c < n; c++)
        {
            m[col][c] = s_mul[scale][m[col][c]];
            inv[col][c] = s_mul[scale][inv[col][c]];
        }

        for (int r = 0; r < n; r++)
        {
            uint8_t factor = m[r][col];
            if (r == col || factor == 0)
                continue;
            for (int c = 0; c < n; c++)
            {
                m[r][c] ^= s_mul[factor][m[col][c]];
                inv[r][c] ^= s_mul[factor][inv[col][c]];
            }
        }
    }
    return true;
}

bool rsReconstruct(uint8_t *const *shards, const bool *present, int dataCount, int parityCount, size_t shardSize)
{
    if (dataCount <= 0 || parityCount < 0 || parityCount > RS_MAX_PARITY || dataCount + parityCount > RS_MAX_SHARDS)
        return false;

    int missing[RS_MAX_PARITY];
    int missingCount = 0;
    for (int k = 0; k < dataCount; k++)
    {
        if (present[k])
            continue;
        if (missingCount == parityCount)
            return false; // more data lost than there is parity at all
        missing[missingCount++] = k;
    }
    if (missingCount == 0)
        return true;

    int used[RS_MAX_PARITY];
    int usedCount = 0;
    for (int p = 0; p < parityCount && usedCount < missingCount; p++)
    {
        if (present[dataCount + p])
            used[usedCount++] = p;
    }
    if (usedCount < missingCount)
        return false;

    // Each used parity shard minus what the data shards that did arrive contributed to it leaves
    // sum over the missing k of C(p, k) * data k - computed in place in the parity shard.
    for (int j = 0; j < usedCount; j++)
    {
        uint8_t *syndrome = shards[dataCount + used[j]];
        for (int k = 0; k < dataCount; k++)
        {
            if (present[k])
                mulAdd(syndrome, shards[k], coefficient(used[j], k), shardSize);
        }
    }

    // Solve the missingCount x missingCount system: A[j][i] = C(used j, missing i).
    static uint8_t a[RS_MAX_PARITY][RS_MAX_PARITY];
    static uint8_t inv[RS_MAX_PARITY][RS_MAX_PARITY];
    for (int j = 0; j < missingCount; j++)
        for (int i = 0; i < missingCount; i++)
            a[j][i] = coefficient(used[j], missing[i]);
    if (!invert(a, inv, missingCount))
        return false;

    for (int i = 0; i < missingCount; i++)
    {
        uint8_t *out = shards[missing[i]];
        memset(out, 0, shardSize);
        for (int j = 0; j < missingCount; j++)
            mulAdd(out, shards[dataCount + used[j]], inv[i][j], shardSize);
    }
    return true;
}
