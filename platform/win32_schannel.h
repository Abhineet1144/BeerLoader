#ifndef BEER_WIN32_SCHANNEL_H
#define BEER_WIN32_SCHANNEL_H
#include <stdint.h>

/* SSPI with the Schannel (TLS client) package, on the host's OpenSSL (libssl loaded at run time; without it every
 * call reports SEC_E_SECPKG_NOT_FOUND). The loader builds the InitSecurityInterface tables from these entry points. */
#define BEER_SSPI_MS __attribute__((ms_abi))
typedef uint64_t u64_sspi;

uint64_t BEER_SSPI_MS beer_AcquireCredentialsHandleA(const char *principal, const char *package, uint32_t use, void *logon_id,
                                                     const uint8_t *auth_data, void *get_key, void *get_key_argument,
                                                     uint64_t *credential, int64_t *expiry);
uint64_t BEER_SSPI_MS beer_AcquireCredentialsHandleW(const uint16_t *principal, const uint16_t *package, uint32_t use,
                                                     void *logon_id, const uint8_t *auth_data, void *get_key,
                                                     void *get_key_argument, uint64_t *credential, int64_t *expiry);
uint64_t BEER_SSPI_MS beer_FreeCredentialsHandle(uint64_t *credential);
uint64_t BEER_SSPI_MS beer_InitializeSecurityContextA(uint64_t *credential, uint64_t *context, const char *target,
                                                      uint32_t requirements, uint32_t reserved1, uint32_t representation,
                                                      uint8_t *input, uint32_t reserved2, uint64_t *new_context,
                                                      uint8_t *output, uint32_t *attributes, int64_t *expiry);
uint64_t BEER_SSPI_MS beer_InitializeSecurityContextW(uint64_t *credential, uint64_t *context, const uint16_t *target,
                                                      uint32_t requirements, uint32_t reserved1, uint32_t representation,
                                                      uint8_t *input, uint32_t reserved2, uint64_t *new_context,
                                                      uint8_t *output, uint32_t *attributes, int64_t *expiry);
uint64_t BEER_SSPI_MS beer_DeleteSecurityContext(uint64_t *context);
uint64_t BEER_SSPI_MS beer_ApplyControlToken(uint64_t *context, uint8_t *input);
uint64_t BEER_SSPI_MS beer_QueryContextAttributesA(uint64_t *context, uint32_t attribute, void *buffer);
uint64_t BEER_SSPI_MS beer_QueryContextAttributesW(uint64_t *context, uint32_t attribute, void *buffer);
uint64_t BEER_SSPI_MS beer_FreeContextBuffer(void *buffer);
uint64_t BEER_SSPI_MS beer_EncryptMessage(uint64_t *context, uint32_t quality, uint8_t *message, uint32_t sequence);
uint64_t BEER_SSPI_MS beer_DecryptMessage(uint64_t *context, uint8_t *message, uint32_t sequence, uint32_t *quality);
uint64_t BEER_SSPI_MS beer_QuerySecurityPackageInfoA(const char *package, void **info);
uint64_t BEER_SSPI_MS beer_QuerySecurityPackageInfoW(const uint16_t *package, void **info);
uint64_t BEER_SSPI_MS beer_EnumerateSecurityPackagesA(uint32_t *count, void **info);
uint64_t BEER_SSPI_MS beer_EnumerateSecurityPackagesW(uint32_t *count, void **info);
uint64_t BEER_SSPI_MS beer_CompleteAuthToken(uint64_t *context, uint8_t *token);
#endif
