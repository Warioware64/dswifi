// SPDX-License-Identifier: MIT

#include <string.h>

#include "arm7/twl/ieee/tkip.h"

// ---- MD5 (RFC 1321)

typedef struct
{
    uint32_t state[4];
    uint64_t length; // bytes taken
    uint8_t block[64];
    size_t used;
} tkip_md5_ctx;

// The sine table: floor(abs(sin(i + 1)) * 2^32).
static const uint32_t md5_k[64] =
{
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t md5_r[64] =
{
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static uint32_t md5_rol(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32 - n));
}

static void md5_block(uint32_t state[4], const uint8_t block[64])
{
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
    {
        m[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; i++)
    {
        uint32_t f;
        int g;
        if (i < 16)
        {
            f = (b & c) | (~b & d);
            g = i;
        }
        else if (i < 32)
        {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        }
        else if (i < 48)
        {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        }
        else
        {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        uint32_t next = d;
        d = c;
        c = b;
        b = b + md5_rol(a + f + md5_k[i] + m[g], md5_r[i]);
        a = next;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_init(tkip_md5_ctx *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
    ctx->length = 0;
    ctx->used = 0;
}

static void md5_update(tkip_md5_ctx *ctx, const uint8_t *data, size_t len)
{
    ctx->length += len;
    while (len > 0)
    {
        size_t take = 64 - ctx->used;
        if (take > len)
            take = len;
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take;
        data += take;
        len -= take;
        if (ctx->used == 64)
        {
            md5_block(ctx->state, ctx->block);
            ctx->used = 0;
        }
    }
}

static void md5_final(tkip_md5_ctx *ctx, uint8_t out[16])
{
    const uint64_t bits = ctx->length * 8;
    const uint8_t one = 0x80, zero = 0;
    md5_update(ctx, &one, 1);
    while (ctx->used != 56)
        md5_update(ctx, &zero, 1);
    uint8_t length_le[8];
    for (int i = 0; i < 8; i++)
        length_le[i] = (uint8_t)(bits >> (8 * i));
    md5_update(ctx, length_le, 8);

    for (int i = 0; i < 4; i++)
    {
        out[i * 4] = (uint8_t)ctx->state[i];
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i] >> 24);
    }
}

void tkip_md5(const uint8_t *data, size_t len, uint8_t out[16])
{
    tkip_md5_ctx ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(&ctx, out);
}

// ---- HMAC-MD5 (RFC 2104)

void tkip_hmac_md5(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                   uint8_t out[16])
{
    uint8_t key_block[64] = { 0 };
    if (key_len > 64)
        tkip_md5(key, key_len, key_block);
    else
        memcpy(key_block, key, key_len);

    uint8_t pad[64];
    tkip_md5_ctx ctx;

    for (int i = 0; i < 64; i++)
        pad[i] = key_block[i] ^ 0x36;
    md5_init(&ctx);
    md5_update(&ctx, pad, 64);
    md5_update(&ctx, data, len);
    uint8_t inner[16];
    md5_final(&ctx, inner);

    for (int i = 0; i < 64; i++)
        pad[i] = key_block[i] ^ 0x5c;
    md5_init(&ctx);
    md5_update(&ctx, pad, 64);
    md5_update(&ctx, inner, 16);
    md5_final(&ctx, out);
}

// ---- RC4

void tkip_rc4(const uint8_t *key, size_t key_len, size_t skip, uint8_t *data, size_t len)
{
    uint8_t s[256];
    for (int i = 0; i < 256; i++)
        s[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++)
    {
        j = (j + s[i] + key[i % key_len]) & 0xFF;
        uint8_t t = s[i];
        s[i] = s[j];
        s[j] = t;
    }

    int i = 0, j = 0;
    for (size_t n = 0; n < skip + len; n++)
    {
        i = (i + 1) & 0xFF;
        j = (j + s[i]) & 0xFF;
        uint8_t t = s[i];
        s[i] = s[j];
        s[j] = t;
        if (n >= skip)
            data[n - skip] ^= s[(s[i] + s[j]) & 0xFF];
    }
}
