#include "win32_bcrypt.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
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
    const EVP_MD *md;
    int hmac;
    int rng;
} Algorithm;

typedef struct {
    uint32_t magic;
    const EVP_MD *md;
    EVP_MD_CTX *ctx;
    HMAC_CTX *hmac;
    uint8_t *secret;
    uint32_t secret_size;
    int reusable;
} Hash;

/* Pseudo-handles Windows defines for the default providers: (BCRYPT_ALG_HANDLE)0x81..0xA1. */
static int pseudo_handle(uint64_t h) { return h >= 0x81 && h <= 0xF1 && (h & 1); }

static const EVP_MD *md_from_name(const uint16_t *name)
{
    char narrow[32];
    size_t i = 0;
    for (; name && name[i] && i < sizeof(narrow) - 1; ++i) narrow[i] = (char)name[i];
    narrow[i] = 0;
    if (!strcmp(narrow, "MD5")) return EVP_md5();
    if (!strcmp(narrow, "SHA1")) return EVP_sha1();
    if (!strcmp(narrow, "SHA256")) return EVP_sha256();
    if (!strcmp(narrow, "SHA384")) return EVP_sha384();
    if (!strcmp(narrow, "SHA512")) return EVP_sha512();
    return NULL;
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
        a->md = md_from_name(algorithm);
        if (!a->md) { free(a); return STATUS_NOT_SUPPORTED; }
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
    const EVP_MD *md = NULL;
    Algorithm *a = algorithm_of(object);
    Hash *h = (Hash *)(uintptr_t)object;
    if (a) md = a->md;
    else if (object && !pseudo_handle(object) && h->magic == HASH_MAGIC) md = h->md;
    else return STATUS_INVALID_HANDLE;
    if (wide_equals(property, "ObjectLength")) return put_u32(sizeof(Hash), output, size, result);
    if (md && wide_equals(property, "HashDigestLength")) return put_u32((uint32_t)EVP_MD_size(md), output, size, result);
    if (md && wide_equals(property, "HashBlockLength")) return put_u32((uint32_t)EVP_MD_block_size(md), output, size, result);
    if (wide_equals(property, "AlgorithmName") && md) {
        static const char *names[] = {"MD5", "SHA1", "SHA256", "SHA384", "SHA512"};
        const EVP_MD *mds[] = {EVP_md5(), EVP_sha1(), EVP_sha256(), EVP_sha384(), EVP_sha512()};
        for (int i = 0; i < 5; ++i)
            if (md == mds[i]) {
                const uint32_t need = (uint32_t)(strlen(names[i]) + 1) * 2;
                if (result) *result = need;
                if (!output) return STATUS_SUCCESS;
                if (size < need) return STATUS_BUFFER_TOO_SMALL;
                for (uint32_t k = 0; names[i][k]; ++k) { output[k * 2] = (uint8_t)names[i][k]; output[k * 2 + 1] = 0; }
                output[need - 2] = output[need - 1] = 0;
                return STATUS_SUCCESS;
            }
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
    h->magic = 0;
    if (h->ctx) EVP_MD_CTX_free(h->ctx);
    if (h->hmac) HMAC_CTX_free(h->hmac);
    free(h->secret);
    free(h);
}

static uint64_t hash_begin(const EVP_MD *md, int hmac, const uint8_t *secret, uint32_t secret_size, int reusable, Hash **out)
{
    Hash *h = calloc(1, sizeof(*h));
    if (!h) return STATUS_NO_MEMORY;
    h->magic = HASH_MAGIC;
    h->md = md;
    h->reusable = reusable;
    if (hmac) {
        h->secret_size = secret_size;
        h->secret = malloc(secret_size ? secret_size : 1);
        h->hmac = HMAC_CTX_new();
        if (!h->secret || !h->hmac) { hash_free(h); return STATUS_NO_MEMORY; }
        if (secret_size) memcpy(h->secret, secret, secret_size);
        if (!HMAC_Init_ex(h->hmac, h->secret, (int)secret_size, md, NULL)) { hash_free(h); return STATUS_INVALID_PARAMETER; }
    } else {
        h->ctx = EVP_MD_CTX_new();
        if (!h->ctx || !EVP_DigestInit_ex(h->ctx, md, NULL)) { hash_free(h); return STATUS_NO_MEMORY; }
    }
    *out = h;
    return STATUS_SUCCESS;
}

BEER_BCRYPT_API uint64_t beer_BCryptCreateHash(uint64_t algorithm, uint64_t *hash, uint8_t *object, uint32_t object_size, const uint8_t *secret, uint32_t secret_size, uint32_t flags)
{
    (void)object; (void)object_size;
    Algorithm *a = algorithm_of(algorithm);
    if (!a || !a->md) return STATUS_INVALID_HANDLE;
    if (!hash) return STATUS_INVALID_PARAMETER;
    *hash = 0;
    Hash *h = NULL;
    const uint64_t status = hash_begin(a->md, a->hmac || secret_size, secret, secret_size, (flags & BCRYPT_HASH_REUSABLE_FLAG) != 0, &h);
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
    if (h->hmac) HMAC_Update(h->hmac, input, size);
    else EVP_DigestUpdate(h->ctx, input, size);
    return STATUS_SUCCESS;
}

BEER_BCRYPT_API uint64_t beer_BCryptFinishHash(uint64_t hash, uint8_t *output, uint32_t size, uint32_t flags)
{
    (void)flags;
    Hash *h = hash_of(hash);
    if (!h) return STATUS_INVALID_HANDLE;
    const uint32_t digest = (uint32_t)EVP_MD_size(h->md);
    if (!output || size != digest) return STATUS_INVALID_PARAMETER;
    unsigned int length = 0;
    if (h->hmac) {
        HMAC_Final(h->hmac, output, &length);
        if (h->reusable) HMAC_Init_ex(h->hmac, h->secret, (int)h->secret_size, h->md, NULL);
    } else {
        EVP_DigestFinal_ex(h->ctx, output, &length);
        if (h->reusable) EVP_DigestInit_ex(h->ctx, h->md, NULL);
    }
    return STATUS_SUCCESS;
}

BEER_BCRYPT_API uint64_t beer_BCryptDuplicateHash(uint64_t hash, uint64_t *duplicate, uint8_t *object, uint32_t object_size, uint32_t flags)
{
    (void)object; (void)object_size; (void)flags;
    Hash *h = hash_of(hash);
    if (!h) return STATUS_INVALID_HANDLE;
    if (!duplicate) return STATUS_INVALID_PARAMETER;
    Hash *copy = NULL;
    uint64_t status = hash_begin(h->md, h->hmac != NULL, h->secret, h->secret_size, h->reusable, &copy);
    if (status != STATUS_SUCCESS) return status;
    if (h->hmac) HMAC_CTX_copy(copy->hmac, h->hmac);
    else EVP_MD_CTX_copy_ex(copy->ctx, h->ctx);
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
    const EVP_MD *md = a ? a->md : NULL;
    if (pseudo_handle(algorithm)) {
        switch (algorithm) {   /* BCRYPT_MD5/SHA1/SHA256/SHA384/SHA512_ALG_HANDLE (+HMAC variants set bit 0x8 differently) */
        case 0x21: md = EVP_md5(); break;
        case 0x31: md = EVP_sha1(); break;
        case 0x41: md = EVP_sha256(); break;
        case 0x51: md = EVP_sha384(); break;
        case 0x61: md = EVP_sha512(); break;
        default: break;
        }
    }
    if (!md) return STATUS_INVALID_HANDLE;
    if ((!input && input_size) || !output || output_size != (uint32_t)EVP_MD_size(md)) return STATUS_INVALID_PARAMETER;
    unsigned int length = 0;
    if (secret_size) HMAC(md, secret, (int)secret_size, input, input_size, output, &length);
    else EVP_Digest(input, input_size, output, &length, md, NULL);
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
