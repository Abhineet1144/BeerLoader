#ifndef BEER_WIN32_BCRYPT_H
#define BEER_WIN32_BCRYPT_H

#include <stdint.h>

/* CNG hash/RNG surface (bcrypt.dll). All entry points use the Windows x64 ABI and return NTSTATUS. */
#define BEER_BCRYPT_API __attribute__((ms_abi))

BEER_BCRYPT_API uint64_t beer_BCryptOpenAlgorithmProvider(uint64_t *handle, const uint16_t *algorithm, const uint16_t *implementation, uint32_t flags);
BEER_BCRYPT_API uint64_t beer_BCryptCloseAlgorithmProvider(uint64_t handle, uint32_t flags);
BEER_BCRYPT_API uint64_t beer_BCryptGetProperty(uint64_t object, const uint16_t *property, uint8_t *output, uint32_t size, uint32_t *result, uint32_t flags);
BEER_BCRYPT_API uint64_t beer_BCryptCreateHash(uint64_t algorithm, uint64_t *hash, uint8_t *object, uint32_t object_size, const uint8_t *secret, uint32_t secret_size, uint32_t flags);
BEER_BCRYPT_API uint64_t beer_BCryptHashData(uint64_t hash, const uint8_t *input, uint32_t size, uint32_t flags);
BEER_BCRYPT_API uint64_t beer_BCryptFinishHash(uint64_t hash, uint8_t *output, uint32_t size, uint32_t flags);
BEER_BCRYPT_API uint64_t beer_BCryptDuplicateHash(uint64_t hash, uint64_t *duplicate, uint8_t *object, uint32_t object_size, uint32_t flags);
BEER_BCRYPT_API uint64_t beer_BCryptDestroyHash(uint64_t hash);
BEER_BCRYPT_API uint64_t beer_BCryptHash(uint64_t algorithm, const uint8_t *secret, uint32_t secret_size, const uint8_t *input, uint32_t input_size, uint8_t *output, uint32_t output_size);
BEER_BCRYPT_API uint64_t beer_BCryptGenRandom(uint64_t algorithm, uint8_t *buffer, uint32_t size, uint32_t flags);

#endif
