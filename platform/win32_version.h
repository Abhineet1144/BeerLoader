#ifndef BEER_WIN32_VERSION_H
#define BEER_WIN32_VERSION_H

#include <stdint.h>

/* Reads the RT_VERSION resource of a PE file. On success *data is malloc'ed (caller frees). */
int beer_pe_version_resource(const char *host_path, uint8_t **data, uint32_t *size);

/* VerQueryValueW over a VS_VERSIONINFO block. Returns 1 and sets *value/*length (length in
 * characters for string values, bytes otherwise) or 0 when the sub-block does not exist. */
int beer_version_query(const uint8_t *block, uint32_t block_size, const uint16_t *sub_block,
                       const void **value, uint32_t *length);

#endif
