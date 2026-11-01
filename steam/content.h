/* Steam content delivery: content servers, depot keys, depot manifests and chunks. */
#ifndef BEER_STEAM_CONTENT_H
#define BEER_STEAM_CONTENT_H

#include <stddef.h>
#include <stdint.h>

#include "cm.h"
#include "http.h"

enum {
    DEPOT_FILE_EXECUTABLE = 32,
    DEPOT_FILE_DIRECTORY = 64,
    DEPOT_FILE_SYMLINK = 512,
};

typedef struct {
    char host[256];
    int https;
    int load;
} CdnServer;

typedef struct {
    uint8_t sha[20];             /* SHA-1 of the decompressed chunk; also its name on the server */
    uint64_t offset;             /* where it goes in the file */
    uint32_t size;               /* decompressed */
    uint32_t compressed;
} ManifestChunk;

typedef struct {
    char *name;                  /* '/'-separated, relative to the install directory */
    uint64_t size;
    uint32_t flags;
    uint8_t sha[20];             /* SHA-1 of the whole file */
    ManifestChunk *chunks;
    size_t chunk_count;
    char *link;                  /* symlink target */
} ManifestFile;

typedef struct {
    uint32_t depot;
    uint64_t gid;
    ManifestFile *files;
    size_t count;
    uint64_t size;               /* sum of file sizes */
    uint64_t compressed;         /* sum of chunk download sizes */
} Manifest;

/* Content servers for the client's cell, best first. Returns the count. */
int content_servers(Cm *cm, CdnServer *out, int capacity);
/* EResult; key receives 32 bytes. */
int content_depot_key(Cm *cm, uint32_t app, uint32_t depot, uint8_t key[32]);
/* EResult; code is 0 when Steam does not require one. */
int content_manifest_code(Cm *cm, uint32_t app, uint32_t depot, uint64_t gid, const char *branch, uint64_t *code);

/* Downloads, decrypts the file names of, and parses a depot manifest. */
int manifest_download(HttpConn *conn, uint32_t depot, uint64_t gid, uint64_t code, const uint8_t key[32],
                      Manifest *out, char *err, size_t errlen);
/* Plain-text-name copy of a manifest, kept with the install for updates and verification. */
int manifest_save(const Manifest *m, const char *path);
int manifest_load(const char *path, Manifest *out);
void manifest_free(Manifest *m);

/* Downloads one chunk into out (chunk->size bytes): decrypt, decompress, verify SHA-1.
 * Returns 1 on success, 0 on a transport error, -1 when the data was bad. */
int chunk_download(HttpConn *conn, uint32_t depot, const ManifestChunk *chunk, const uint8_t key[32],
                   uint8_t *out, char *err, size_t errlen);
/* Decrypts and decompresses chunk data that is already in memory (also used by tests). */
int chunk_decode(const uint8_t *data, size_t len, const uint8_t key[32], uint8_t *out, uint32_t size);

/* AES-256: 16-byte ECB-encrypted IV followed by CBC data with PKCS#7 padding, as Steam uses. */
int steam_symmetric_decrypt(const uint8_t *in, size_t len, const uint8_t key[32], uint8_t *out, size_t *out_len);

/* AES-256-ECB with PKCS#7 padding (beta branch manifest IDs). */
int steam_ecb_decrypt(const uint8_t *in, size_t len, const uint8_t key[32], uint8_t *out, size_t *out_len);

void hex_encode(const uint8_t *data, size_t len, char *out);

#endif
