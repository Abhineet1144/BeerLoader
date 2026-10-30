#include "beer_hash.h"

#include <string.h>

static const uint32_t md5_k[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull,
    0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull,
    0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull,
    0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull,
    0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull,
    0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull,
    0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull,
    0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull,
    0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
    0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull,
    0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull,
    0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
    0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull,
    0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

static const uint32_t sha256_iv[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

static const uint64_t sha512_iv[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
    0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
    0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

static const uint64_t sha384_iv[8] = {
    0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull,
    0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
    0x67332667ffc00b31ull, 0x8eb44a8768581511ull,
    0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull,
};

static uint32_t rol32(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ror32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static uint64_t ror64(uint64_t x, unsigned n) { return (x >> n) | (x << (64 - n)); }

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t load_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint64_t load_be64(const uint8_t *p)
{
    return (uint64_t)load_be32(p) << 32 | load_be32(p + 4);
}

static void store_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

static void store_be32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (24 - 8 * i));
}

static void store_be64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static void md5_block(uint32_t s[4], const uint8_t *p)
{
    static const unsigned shift[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = load_le32(p + 4 * i);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t f;
        unsigned g;
        switch (i / 16) {
        case 0: f = (b & c) | (~b & d); g = i; break;
        case 1: f = (d & b) | (~d & c); g = (5 * i + 1) & 15; break;
        case 2: f = b ^ c ^ d; g = (3 * i + 5) & 15; break;
        default: f = c ^ (b | ~d); g = (7 * i) & 15; break;
        }
        const uint32_t t = d;
        d = c;
        c = b;
        b = b + rol32(a + f + md5_k[i] + m[g], shift[i / 16][i % 4]);
        a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

static void sha1_block(uint32_t s[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = load_be32(p + 4 * i);
    for (int i = 16; i < 80; ++i) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
        else { f = b ^ c ^ d; k = 0xca62c1d6u; }
        const uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e;
}

static void sha256_block(uint32_t s[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = load_be32(p + 4 * i);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t v[8];
    memcpy(v, s, sizeof(v));
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = ror32(v[4], 6) ^ ror32(v[4], 11) ^ ror32(v[4], 25);
        const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        const uint32_t t1 = v[7] + s1 + ch + sha256_k[i] + w[i];
        const uint32_t s0 = ror32(v[0], 2) ^ ror32(v[0], 13) ^ ror32(v[0], 22);
        const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        const uint32_t t2 = s0 + maj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; ++i) s[i] += v[i];
}

static void sha512_block(uint64_t s[8], const uint8_t *p)
{
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = load_be64(p + 8 * i);
    for (int i = 16; i < 80; ++i) {
        const uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t v[8];
    memcpy(v, s, sizeof(v));
    for (int i = 0; i < 80; ++i) {
        const uint64_t s1 = ror64(v[4], 14) ^ ror64(v[4], 18) ^ ror64(v[4], 41);
        const uint64_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        const uint64_t t1 = v[7] + s1 + ch + sha512_k[i] + w[i];
        const uint64_t s0 = ror64(v[0], 28) ^ ror64(v[0], 34) ^ ror64(v[0], 39);
        const uint64_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        const uint64_t t2 = s0 + maj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; ++i) s[i] += v[i];
}

size_t beer_hash_digest_size(BeerHashAlg alg)
{
    switch (alg) {
    case BEER_HASH_MD5: return 16;
    case BEER_HASH_SHA1: return 20;
    case BEER_HASH_SHA256: return 32;
    case BEER_HASH_SHA384: return 48;
    default: return 64;
    }
}

size_t beer_hash_block_size(BeerHashAlg alg)
{
    return alg == BEER_HASH_SHA384 || alg == BEER_HASH_SHA512 ? 128 : 64;
}

static void compress(BeerHashCtx *ctx, const uint8_t *block)
{
    switch (ctx->alg) {
    case BEER_HASH_MD5: md5_block(ctx->state.w, block); break;
    case BEER_HASH_SHA1: sha1_block(ctx->state.w, block); break;
    case BEER_HASH_SHA256: sha256_block(ctx->state.w, block); break;
    default: sha512_block(ctx->state.q, block); break;
    }
}

void beer_hash_init(BeerHashCtx *ctx, BeerHashAlg alg)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->alg = alg;
    switch (alg) {
    case BEER_HASH_MD5:
        ctx->state.w[0] = 0x67452301u; ctx->state.w[1] = 0xefcdab89u;
        ctx->state.w[2] = 0x98badcfeu; ctx->state.w[3] = 0x10325476u;
        break;
    case BEER_HASH_SHA1:
        ctx->state.w[0] = 0x67452301u; ctx->state.w[1] = 0xefcdab89u;
        ctx->state.w[2] = 0x98badcfeu; ctx->state.w[3] = 0x10325476u;
        ctx->state.w[4] = 0xc3d2e1f0u;
        break;
    case BEER_HASH_SHA256: memcpy(ctx->state.w, sha256_iv, sizeof(sha256_iv)); break;
    case BEER_HASH_SHA384: memcpy(ctx->state.q, sha384_iv, sizeof(sha384_iv)); break;
    default: memcpy(ctx->state.q, sha512_iv, sizeof(sha512_iv)); break;
    }
}

void beer_hash_update(BeerHashCtx *ctx, const void *data, size_t size)
{
    const uint8_t *p = data;
    const size_t block = beer_hash_block_size(ctx->alg);
    ctx->length += size;
    if (ctx->buffered) {
        size_t take = block - ctx->buffered;
        if (take > size) take = size;
        memcpy(ctx->buffer + ctx->buffered, p, take);
        ctx->buffered += (uint32_t)take;
        p += take;
        size -= take;
        if (ctx->buffered < block) return;
        compress(ctx, ctx->buffer);
        ctx->buffered = 0;
    }
    for (; size >= block; p += block, size -= block) compress(ctx, p);
    if (size) {
        memcpy(ctx->buffer, p, size);
        ctx->buffered = (uint32_t)size;
    }
}

void beer_hash_final(BeerHashCtx *ctx, uint8_t *out)
{
    const size_t block = beer_hash_block_size(ctx->alg);
    const size_t length_bytes = block == 128 ? 16 : 8;
    const uint64_t bits_low = ctx->length << 3;
    const uint64_t bits_high = ctx->length >> 61;

    ctx->buffer[ctx->buffered++] = 0x80;
    if (ctx->buffered > block - length_bytes) {
        memset(ctx->buffer + ctx->buffered, 0, block - ctx->buffered);
        compress(ctx, ctx->buffer);
        ctx->buffered = 0;
    }
    memset(ctx->buffer + ctx->buffered, 0, block - ctx->buffered);
    if (ctx->alg == BEER_HASH_MD5) {
        store_le32(ctx->buffer + 56, (uint32_t)bits_low);
        store_le32(ctx->buffer + 60, (uint32_t)(bits_low >> 32));
    } else {
        store_be64(ctx->buffer + block - 8, bits_low);
        if (block == 128) store_be64(ctx->buffer + 112, bits_high);
    }
    compress(ctx, ctx->buffer);

    const size_t digest = beer_hash_digest_size(ctx->alg);
    if (ctx->alg == BEER_HASH_MD5) {
        for (int i = 0; i < 4; ++i) store_le32(out + 4 * i, ctx->state.w[i]);
    } else if (block == 64) {
        for (size_t i = 0; i < digest / 4; ++i) store_be32(out + 4 * i, ctx->state.w[i]);
    } else {
        for (size_t i = 0; i < digest / 8; ++i) store_be64(out + 8 * i, ctx->state.q[i]);
    }
}

void beer_hash(BeerHashAlg alg, const void *data, size_t size, uint8_t *out)
{
    BeerHashCtx ctx;
    beer_hash_init(&ctx, alg);
    beer_hash_update(&ctx, data, size);
    beer_hash_final(&ctx, out);
}

void beer_hmac_init(BeerHmacCtx *ctx, BeerHashAlg alg, const void *key, size_t key_size)
{
    const size_t block = beer_hash_block_size(alg);
    uint8_t pad[BEER_HASH_MAX_BLOCK];
    memset(pad, 0, sizeof(pad));
    if (key_size > block) beer_hash(alg, key, key_size, pad);
    else if (key_size) memcpy(pad, key, key_size);

    for (size_t i = 0; i < block; ++i) pad[i] ^= 0x36;
    beer_hash_init(&ctx->inner0, alg);
    beer_hash_update(&ctx->inner0, pad, block);
    for (size_t i = 0; i < block; ++i) pad[i] ^= 0x36 ^ 0x5c;
    beer_hash_init(&ctx->outer0, alg);
    beer_hash_update(&ctx->outer0, pad, block);
    ctx->inner = ctx->inner0;
    memset(pad, 0, sizeof(pad));
}

void beer_hmac_update(BeerHmacCtx *ctx, const void *data, size_t size)
{
    beer_hash_update(&ctx->inner, data, size);
}

void beer_hmac_final(BeerHmacCtx *ctx, uint8_t *out)
{
    uint8_t digest[BEER_HASH_MAX_DIGEST];
    BeerHashCtx outer = ctx->outer0;
    beer_hash_final(&ctx->inner, digest);
    beer_hash_update(&outer, digest, beer_hash_digest_size(outer.alg));
    beer_hash_final(&outer, out);
    ctx->inner = ctx->inner0;
}

void beer_hmac(BeerHashAlg alg, const void *key, size_t key_size,
               const void *data, size_t size, uint8_t *out)
{
    BeerHmacCtx ctx;
    beer_hmac_init(&ctx, alg, key, key_size);
    beer_hmac_update(&ctx, data, size);
    beer_hmac_final(&ctx, out);
}
