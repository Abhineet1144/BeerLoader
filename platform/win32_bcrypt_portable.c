#include "win32_bcrypt.h"

#include "beer_hash.h"

#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#define STATUS_SUCCESS 0u
#define STATUS_NOT_SUPPORTED 0xC00000BBu
#define STATUS_INVALID_PARAMETER 0xC000000Du
#define STATUS_INVALID_HANDLE 0xC0000008u
#define STATUS_BUFFER_TOO_SMALL 0xC0000023u
#define STATUS_NO_MEMORY 0xC0000017u
#define STATUS_NOT_FOUND 0xC0000225u

#define ALG_MAGIC 0x42414C47u   /* "BALG" */
#define HASH_MAGIC 0x42484153u  /* "BHAS" */
#define BCRYPT_ALG_HANDLE_HMAC_FLAG 0x8u
#define BCRYPT_HASH_REUSABLE_FLAG 0x20u

typedef struct {
    uint32_t magic;
    int has_alg;
    BeerHashAlg alg;
    int hmac;
    int rng;
} Algorithm;

/* Plain data only, so BCryptDuplicateHash is a struct copy. */
typedef struct {
    uint32_t magic;
    BeerHashAlg alg;
    int is_hmac;
    int reusable;
    BeerHashCtx ctx;
    BeerHmacCtx hmac;
} Hash;

static const char *const alg_names[] = {"MD5", "SHA1", "SHA256", "SHA384", "SHA512"};

/* Pseudo-handles Windows defines for the default providers: (BCRYPT_ALG_HANDLE)0x11..0xF1, which include
 * BCRYPT_MD5/SHA1/SHA256/SHA384/SHA512_ALG_HANDLE (0x21..0x61), BCRYPT_RNG_ALG_HANDLE (0x81) and the HMAC set. */
static int pseudo_handle(uint64_t h) { return h >= 0x11 && h <= 0xF1 && (h & 1); }

static int alg_from_name(const uint16_t *name, BeerHashAlg *out)
{
    char narrow[32];
    size_t i = 0;
    for (; name && name[i] && i < sizeof(narrow) - 1; ++i) narrow[i] = (char)name[i];
    narrow[i] = 0;
    for (size_t k = 0; k < sizeof(alg_names) / sizeof(alg_names[0]); ++k)
        if (!strcmp(narrow, alg_names[k])) { *out = (BeerHashAlg)k; return 1; }
    return 0;
}

static int wide_equals(const uint16_t *wide, const char *narrow)
{
    size_t i = 0;
    for (; narrow[i]; ++i) if (!wide || wide[i] != (uint16_t)(uint8_t)narrow[i]) return 0;
    return wide && wide[i] == 0;
}

static Algorithm *algorithm_of(uint64_t h)
{
    Algorithm *a = (Algorithm *)(uintptr_t)h;
    return h && !pseudo_handle(h) && a->magic == ALG_MAGIC ? a : NULL;
}

BEER_BCRYPT_API uint64_t beer_BCryptOpenAlgorithmProvider(uint64_t *handle, const uint16_t *algorithm, const uint16_t *implementation, uint32_t flags)
{
    (void)implementation;
    if (!handle || !algorithm) return STATUS_INVALID_PARAMETER;
    *handle = 0;
    Algorithm *a = calloc(1, sizeof(*a));
    if (!a) return STATUS_NO_MEMORY;
    a->magic = ALG_MAGIC;
    a->hmac = (flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0;
    if (wide_equals(algorithm, "RNG")) a->rng = 1;
    else {
        a->has_alg = alg_from_name(algorithm, &a->alg);
        if (!a->has_alg) { free(a); return STATUS_NOT_SUPPORTED; }
    }
    *handle = (uint64_t)(uintptr_t)a;
    return STATUS_SUCCESS;
}

BEER_BCRYPT_API uint64_t beer_BCryptCloseAlgorithmProvider(uint64_t handle, uint32_t flags)
{
    (void)flags;
    Algorithm *a = algorithm_of(handle);
    if (!a) return STATUS_INVALID_HANDLE;
    a->magic = 0;
    free(a);
    return STATUS_SUCCESS;
}

static uint64_t put_u32(uint32_t value, uint8_t *output, uint32_t size, uint32_t *result)
{
    if (result) *result = sizeof(value);
    if (!output) return STATUS_SUCCESS;
    if (size < sizeof(value)) return STATUS_BUFFER_TOO_SMALL;
    memcpy(output, &value, sizeof(value));
    return STATUS_SUCCESS;
}

BEER_BCRYPT_API uint64_t beer_BCryptGetProperty(uint64_t object, const uint16_t *property, uint8_t *output, uint32_t size, uint32_t *result, uint32_t flags)
{
    (void)flags;
    int has_alg = 0;
    BeerHashAlg alg = BEER_HASH_MD5;
    Algorithm *a = algorithm_of(object);
    Hash *h = (Hash *)(uintptr_t)object;
    if (a) { has_alg = a->has_alg; alg = a->alg; }
    else if (object && !pseudo_handle(object) && h->magic == HASH_MAGIC) { has_alg = 1; alg = h->alg; }
    else return STATUS_INVALID_HANDLE;
    if (wide_equals(property, "ObjectLength")) return put_u32(sizeof(Hash), output, size, result);
    if (has_alg && wide_equals(property, "HashDigestLength")) return put_u32((uint32_t)beer_hash_digest_size(alg), output, size, result);
    if (has_alg && wide_equals(property, "HashBlockLength")) return put_u32((uint32_t)beer_hash_block_size(alg), output, size, result);
    if (wide_equals(property, "AlgorithmName") && has_alg) {
        const char *name = alg_names[alg];
        const uint32_t need = (uint32_t)(strlen(name) + 1) * 2;
        if (result) *result = need;
        if (!output) return STATUS_SUCCESS;
        if (size < need) return STATUS_BUFFER_TOO_SMALL;
        for (uint32_t k = 0; name[k]; ++k) { output[k * 2] = (uint8_t)name[k]; output[k * 2 + 1] = 0; }
        output[need - 2] = output[need - 1] = 0;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_FOUND;
}

static Hash *hash_of(uint64_t h)
{
    Hash *hash = (Hash *)(uintptr_t)h;
    return h && !pseudo_handle(h) && hash->magic == HASH_MAGIC ? hash : NULL;
}

static void hash_free(Hash *h)
{
    memset(h, 0, sizeof(*h));   /* clears magic and any keyed HMAC state */
    free(h);
}

static uint64_t hash_begin(BeerHashAlg alg, int hmac, const uint8_t *secret, uint32_t secret_size, int reusable, Hash **out)
{
    Hash *h = calloc(1, sizeof(*h));
    if (!h) return STATUS_NO_MEMORY;
    h->magic = HASH_MAGIC;
    h->alg = alg;
    h->reusable = reusable;
    h->is_hmac = hmac;
    if (hmac) beer_hmac_init(&h->hmac, alg, secret, secret_size);
    else beer_hash_init(&h->ctx, alg);
    *out = h;
    return STATUS_SUCCESS;
}

BEER_BCRYPT_API uint64_t beer_BCryptCreateHash(uint64_t algorithm, uint64_t *hash, uint8_t *object, uint32_t object_size, const uint8_t *secret, uint32_t secret_size, uint32_t flags)
{
    (void)object; (void)object_size;
    Algorithm *a = algorithm_of(algorithm);
    if (!a || !a->has_alg) return STATUS_INVALID_HANDLE;
    if (!hash) return STATUS_INVALID_PARAMETER;
    *hash = 0;
    Hash *h = NULL;
    const uint64_t status = hash_begin(a->alg, a->hmac || secret_size, secret, secret_size, (flags & BCRYPT_HASH_REUSABLE_FLAG) != 0, &h);
    if (status != STATUS_SUCCESS) return status;
    *hash = (uint64_t)(uintptr_t)h;
    return STATUS_SUCCESS;
}

BEER_BCRYPT_API uint64_t beer_BCryptHashData(uint64_t hash, const uint8_t *input, uint32_t size, uint32_t flags)
{
    (void)flags;
    Hash *h = hash_of(hash);
    if (!h) return STATUS_INVALID_HANDLE;
    if (!input && size) return STATUS_INVALID_PARAMETER;
    if (h->is_hmac) beer_hmac_update(&h->hmac, input, size);
    else beer_hash_update(&h->ctx, input, size);
    return STATUS_SUCCESS;
}

BEER_BCRYPT_API uint64_t beer_BCryptFinishHash(uint64_t hash, uint8_t *output, uint32_t size, uint32_t flags)
{
    (void)flags;
    Hash *h = hash_of(hash);
    if (!h) return STATUS_INVALID_HANDLE;
    const uint32_t digest = (uint32_t)beer_hash_digest_size(h->alg);
    if (!output || size != digest) return STATUS_INVALID_PARAMETER;
    if (h->is_hmac) {
        beer_hmac_final(&h->hmac, output);   /* rewinds to the keyed state */
    } else {
        beer_hash_final(&h->ctx, output);
        if (h->reusable) beer_hash_init(&h->ctx, h->alg);
    }
    return STATUS_SUCCESS;
}

BEER_BCRYPT_API uint64_t beer_BCryptDuplicateHash(uint64_t hash, uint64_t *duplicate, uint8_t *object, uint32_t object_size, uint32_t flags)
{
    (void)object; (void)object_size; (void)flags;
    Hash *h = hash_of(hash);
    if (!h) return STATUS_INVALID_HANDLE;
    if (!duplicate) return STATUS_INVALID_PARAMETER;
    Hash *copy = malloc(sizeof(*copy));
    if (!copy) return STATUS_NO_MEMORY;
    *copy = *h;
    *duplicate = (uint64_t)(uintptr_t)copy;
    return STATUS_SUCCESS;
}

BEER_BCRYPT_API uint64_t beer_BCryptDestroyHash(uint64_t hash)
{
    Hash *h = hash_of(hash);
    if (!h) return STATUS_INVALID_HANDLE;
    hash_free(h);
    return STATUS_SUCCESS;
}

BEER_BCRYPT_API uint64_t beer_BCryptHash(uint64_t algorithm, const uint8_t *secret, uint32_t secret_size, const uint8_t *input, uint32_t input_size, uint8_t *output, uint32_t output_size)
{
    Algorithm *a = algorithm_of(algorithm);
    int have = a && a->has_alg;
    BeerHashAlg alg = have ? a->alg : BEER_HASH_MD5;
    if (pseudo_handle(algorithm)) {
        switch (algorithm) {   /* BCRYPT_MD5/SHA1/SHA256/SHA384/SHA512_ALG_HANDLE (+HMAC variants set bit 0x8 differently) */
        case 0x21: alg = BEER_HASH_MD5; have = 1; break;
        case 0x31: alg = BEER_HASH_SHA1; have = 1; break;
        case 0x41: alg = BEER_HASH_SHA256; have = 1; break;
        case 0x51: alg = BEER_HASH_SHA384; have = 1; break;
        case 0x61: alg = BEER_HASH_SHA512; have = 1; break;
        default: break;
        }
    }
    if (!have) return STATUS_INVALID_HANDLE;
    if ((!input && input_size) || !output || output_size != (uint32_t)beer_hash_digest_size(alg)) return STATUS_INVALID_PARAMETER;
    if (secret_size) beer_hmac(alg, secret, secret_size, input, input_size, output);
    else beer_hash(alg, input, input_size, output);
    return STATUS_SUCCESS;
}

BEER_BCRYPT_API uint64_t beer_BCryptGenRandom(uint64_t algorithm, uint8_t *buffer, uint32_t size, uint32_t flags)
{
    (void)algorithm; (void)flags;
    if (!buffer && size) return STATUS_INVALID_PARAMETER;
    uint32_t done = 0;
    while (done < size) {
        const ssize_t got = getrandom(buffer + done, size - done, 0);
        if (got <= 0) return 0xC0000001u;
        done += (uint32_t)got;
    }
    return STATUS_SUCCESS;
}
