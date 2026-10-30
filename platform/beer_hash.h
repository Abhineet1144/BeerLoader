#ifndef BEER_HASH_H
#define BEER_HASH_H

/* Self-contained MD5 / SHA-1 / SHA-2 and HMAC for the BCrypt shim, so Beer needs
 * no external crypto library (OpenSSL is not part of the Android NDK). Streaming
 * contexts are plain structs: copy them with assignment to duplicate a hash. */

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BEER_HASH_MD5,
    BEER_HASH_SHA1,
    BEER_HASH_SHA256,
    BEER_HASH_SHA384,
    BEER_HASH_SHA512
} BeerHashAlg;

#define BEER_HASH_MAX_DIGEST 64
#define BEER_HASH_MAX_BLOCK 128

typedef struct {
    BeerHashAlg alg;
    uint64_t length;                       /* bytes absorbed so far */
    uint32_t buffered;
    uint8_t buffer[BEER_HASH_MAX_BLOCK];
    union { uint32_t w[8]; uint64_t q[8]; } state;
} BeerHashCtx;

typedef struct {
    BeerHashCtx inner;                     /* running inner hash */
    BeerHashCtx inner0;                    /* keyed inner state, for reuse */
    BeerHashCtx outer0;                    /* keyed outer state */
} BeerHmacCtx;

size_t beer_hash_digest_size(BeerHashAlg alg);
size_t beer_hash_block_size(BeerHashAlg alg);

void beer_hash_init(BeerHashCtx *ctx, BeerHashAlg alg);
void beer_hash_update(BeerHashCtx *ctx, const void *data, size_t size);
/* Writes beer_hash_digest_size() bytes. The context must be re-initialised before reuse. */
void beer_hash_final(BeerHashCtx *ctx, uint8_t *out);
void beer_hash(BeerHashAlg alg, const void *data, size_t size, uint8_t *out);

void beer_hmac_init(BeerHmacCtx *ctx, BeerHashAlg alg, const void *key, size_t key_size);
void beer_hmac_update(BeerHmacCtx *ctx, const void *data, size_t size);
/* Writes the MAC and rewinds the context to the freshly keyed state. */
void beer_hmac_final(BeerHmacCtx *ctx, uint8_t *out);
void beer_hmac(BeerHashAlg alg, const void *key, size_t key_size,
               const void *data, size_t size, uint8_t *out);

#endif
