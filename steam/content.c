#include "content.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <zlib.h>
#include <zstd.h>

#define CALL_TIMEOUT_MS 20000

#define MANIFEST_PAYLOAD_MAGIC   0x71F617D0u
#define MANIFEST_METADATA_MAGIC  0x1F4812BEu
#define MANIFEST_SIGNATURE_MAGIC 0x1B81B817u
#define MANIFEST_END_MAGIC       0x32C415ABu

void hex_encode(const uint8_t *data, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 15];
    }
    out[2 * len] = 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---------------------------------------------------------------- Steam CM requests */

int content_servers(Cm *cm, CdnServer *out, int capacity)
{
    PbBuf request = {0}, response;
    pb_u64(&request, 1, cm_cell_id(cm));
    pb_u64(&request, 2, 20);
    int eresult;
    cm_call(cm, "ContentServerDirectory.GetServersForSteamPipe#1", &request, 1, CALL_TIMEOUT_MS, &response, &eresult);
    pb_free(&request);
    int count = 0;
    if (eresult == ERESULT_OK) {
        PbReader r = { response.data, response.data + response.len };
        PbField f;
        while (count < capacity && pb_next(&r, &f) == 1) {
            if (f.field != 1 || f.type != PB_BYTES) continue;
            char type[32] = "", https[32] = "";
            CdnServer server = {0};
            int usable = 1;
            PbReader sr = { f.data, f.data + f.len };
            PbField sf;
            while (pb_next(&sr, &sf) == 1) {
                size_t n;
                switch (sf.field) {
                case 1: n = sf.len < sizeof(type) - 1 ? sf.len : sizeof(type) - 1; memcpy(type, sf.data, n); type[n] = 0; break;
                case 4: server.load = (int)sf.value; break;
                case 7: case 10: if (sf.value) usable = 0; break;     /* China-only or proxy servers */
                case 8: n = sf.len < sizeof(server.host) - 1 ? sf.len : sizeof(server.host) - 1;
                        memcpy(server.host, sf.data, n); server.host[n] = 0; break;
                case 12: n = sf.len < sizeof(https) - 1 ? sf.len : sizeof(https) - 1; memcpy(https, sf.data, n); https[n] = 0; break;
                case 13: usable = 0; break;                           /* restricted to particular apps */
                }
            }
            if (!usable || !server.host[0] || (strcmp(type, "SteamCache") && strcmp(type, "CDN"))) continue;
            server.https = https[0] != 0;
            out[count++] = server;
        }
    }
    pb_free(&response);
    for (int i = 1; i < count; i++)                                    /* least loaded first */
        for (int j = i; j > 0 && out[j].load < out[j - 1].load; j--) {
            CdnServer t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return count;
}

int content_depot_key(Cm *cm, uint32_t app, uint32_t depot, uint8_t key[32])
{
    PbBuf request = {0};
    pb_u64(&request, 1, depot);
    pb_u64(&request, 2, app);
    CmPart *parts;
    int ok = cm_request(cm, EMSG_CLIENT_GET_DEPOT_DECRYPTION_KEY, &request, 0, CALL_TIMEOUT_MS, &parts);
    pb_free(&request);
    if (!ok) return ERESULT_TIMEOUT;
    int eresult = ERESULT_FAIL;
    PbField f;
    if (pb_find(parts->body.data, parts->body.len, 1, &f)) eresult = (int)(int32_t)f.value;
    if (eresult == ERESULT_OK) {
        if (pb_find(parts->body.data, parts->body.len, 3, &f) && f.len == 32) memcpy(key, f.data, 32);
        else eresult = ERESULT_FAIL;
    }
    cm_parts_free(parts);
    return eresult;
}

int content_manifest_code(Cm *cm, uint32_t app, uint32_t depot, uint64_t gid, const char *branch, uint64_t *code)
{
    *code = 0;
    PbBuf request = {0}, response;
    pb_u64(&request, 1, app);
    pb_u64(&request, 2, depot);
    pb_u64(&request, 3, gid);
    pb_string(&request, 4, branch && *branch ? branch : "public");
    int eresult;
    cm_call(cm, "ContentServerDirectory.GetManifestRequestCode#1", &request, 1, CALL_TIMEOUT_MS, &response, &eresult);
    pb_free(&request);
    PbField f;
    if (eresult == ERESULT_OK && pb_find(response.data, response.len, 1, &f)) *code = f.value;
    pb_free(&response);
    return eresult;
}

/* ---------------------------------------------------------------- crypto and compression */

int steam_symmetric_decrypt(const uint8_t *in, size_t len, const uint8_t key[32], uint8_t *out, size_t *out_len)
{
    if (len < 32 || (len - 16) % 16) return 0;
    uint8_t iv[16];
    int n = 0, tail = 0, ok = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_ecb(), NULL, key, NULL) == 1 && EVP_CIPHER_CTX_set_padding(ctx, 0) &&
        EVP_DecryptUpdate(ctx, iv, &n, in, 16) == 1 && n == 16 &&
        EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) == 1 &&
        EVP_CIPHER_CTX_set_padding(ctx, 1) &&            /* the ECB step turned padding off */
        EVP_DecryptUpdate(ctx, out, &n, in + 16, (int)(len - 16)) == 1 &&
        EVP_DecryptFinal_ex(ctx, out + n, &tail) == 1) {
        *out_len = (size_t)(n + tail);
        ok = 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

int steam_ecb_decrypt(const uint8_t *in, size_t len, const uint8_t key[32], uint8_t *out, size_t *out_len)
{
    if (!len || len % 16) return 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, tail = 0, ok = 0;
    if (ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_ecb(), NULL, key, NULL) == 1 &&
        EVP_DecryptUpdate(ctx, out, &n, in, (int)len) == 1 && EVP_DecryptFinal_ex(ctx, out + n, &tail) == 1) {
        *out_len = (size_t)(n + tail);
        ok = 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

/* Inflates raw deflate data into exactly size bytes. */
static int inflate_raw(const uint8_t *in, size_t len, uint8_t *out, size_t size)
{
    z_stream z = {0};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return 0;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)len;
    z.next_out = out;
    z.avail_out = (uInt)size;
    int status = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return status == Z_STREAM_END && z.total_out == size;
}

/* First entry of a PKZip archive, using the central directory for its sizes. */
static uint8_t *unzip_first(const uint8_t *zip, size_t len, size_t *out_len)
{
    if (len < 22) return NULL;
    size_t eocd = len - 22;
    while (eocd > 0 && le32(zip + eocd) != 0x06054b50u) {
        if (len - eocd > 22 + 65535) return NULL;
        eocd--;
    }
    if (le32(zip + eocd) != 0x06054b50u) return NULL;
    size_t central = le32(zip + eocd + 16);
    if (central + 46 > len || le32(zip + central) != 0x02014b50u) return NULL;
    uint16_t method = le16(zip + central + 10);
    uint32_t compressed = le32(zip + central + 20), size = le32(zip + central + 24);
    size_t local = le32(zip + central + 42);
    if (local + 30 > len || le32(zip + local) != 0x04034b50u) return NULL;
    size_t data = local + 30 + le16(zip + local + 26) + le16(zip + local + 28);
    if (data > len || compressed > len - data || size > (256u << 20)) return NULL;
    uint8_t *out = malloc(size ? size : 1);
    if (!out) return NULL;
    if (method == 0 && compressed == size) memcpy(out, zip + data, size);
    else if (method != 8 || !inflate_raw(zip + data, compressed, out, size)) { free(out); return NULL; }
    *out_len = size;
    return out;
}

/* liblzma, loaded at run time (only the runtime library is required). */
typedef struct {
    const uint8_t *next_in; size_t avail_in; uint64_t total_in;
    uint8_t *next_out; size_t avail_out; uint64_t total_out;
    const void *allocator; void *internal;
    void *reserved_ptr1, *reserved_ptr2, *reserved_ptr3, *reserved_ptr4;
    uint64_t reserved_int1, reserved_int2;
    size_t reserved_int3, reserved_int4;
    int reserved_enum1, reserved_enum2;
} LzmaStream;
static struct {
    int (*alone_decoder)(LzmaStream *, uint64_t);
    int (*code)(LzmaStream *, int);
    void (*end)(LzmaStream *);
} lzma;

static void lzma_load(void)
{
    void *lib = dlopen("liblzma.so.5", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
    lzma.alone_decoder = (int (*)(LzmaStream *, uint64_t))dlsym(lib, "lzma_alone_decoder");
    lzma.code = (int (*)(LzmaStream *, int))dlsym(lib, "lzma_code");
    lzma.end = (void (*)(LzmaStream *))dlsym(lib, "lzma_end");
    if (!lzma.alone_decoder || !lzma.code || !lzma.end) memset(&lzma, 0, sizeof(lzma));
}

/* Valve's "VZa" container: 7-byte header, 5 bytes of LZMA properties, data, 10-byte footer. */
static int vzip_decode(const uint8_t *in, size_t len, uint8_t *out, uint32_t size)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, lzma_load);
    if (!lzma.alone_decoder || len < 7 + 5 + 10 || in[len - 2] != 'z' || in[len - 1] != 'v' ||
        le32(in + len - 6) != size)
        return 0;
    /* lzma_alone_decoder wants props + 64-bit size before the stream. */
    size_t body = len - 12 - 10;
    uint8_t *alone = malloc(13 + body);
    if (!alone) return 0;
    memcpy(alone, in + 7, 5);
    for (int i = 0; i < 8; i++) alone[5 + i] = (uint8_t)((uint64_t)size >> (8 * i));
    memcpy(alone + 13, in + 12, body);
    LzmaStream s;
    memset(&s, 0, sizeof(s));
    int ok = 0;
    if (lzma.alone_decoder(&s, UINT64_MAX) == 0) {
        s.next_in = alone;
        s.avail_in = 13 + body;
        s.next_out = out;
        s.avail_out = size;
        int status = lzma.code(&s, 3 /* LZMA_FINISH */);
        ok = (status == 1 /* LZMA_STREAM_END */ || (status == 0 && s.total_out == size)) && s.total_out == size;
        lzma.end(&s);
    }
    free(alone);
    return ok;
}

/* Valve's "VSZa" container: a zstd frame between a small header and footer. */
static int vszip_decode(const uint8_t *in, size_t len, uint8_t *out, uint32_t size)
{
    for (size_t at = 4; at + 4 <= len && at < 32; at++) {
        if (le32(in + at) != 0xFD2FB528u) continue;
        size_t frame = ZSTD_findFrameCompressedSize(in + at, len - at);
        if (ZSTD_isError(frame)) return 0;
        size_t got = ZSTD_decompress(out, size, in + at, frame);
        return !ZSTD_isError(got) && got == size;
    }
    return 0;
}

int chunk_decode(const uint8_t *data, size_t len, const uint8_t key[32], uint8_t *out, uint32_t size)
{
    uint8_t *plain = malloc(len);
    size_t plain_len = 0;
    if (!plain || !steam_symmetric_decrypt(data, len, key, plain, &plain_len)) { free(plain); return 0; }
    int ok = 0;
    const char *dump = getenv("BEER_STEAM_CHUNK_DUMP");      /* diagnostics: keep decrypted chunk data */
    if (plain_len >= 3 && plain[0] == 'V' && plain[1] == 'Z' && plain[2] == 'a') {
        ok = vzip_decode(plain, plain_len, out, size);
    } else if (plain_len >= 4 && !memcmp(plain, "VSZa", 4)) {
        ok = vszip_decode(plain, plain_len, out, size);
    } else if (plain_len >= 4 && le32(plain) == 0x04034b50u) {
        size_t unzipped_len;
        uint8_t *unzipped = unzip_first(plain, plain_len, &unzipped_len);
        if (unzipped && unzipped_len == size) { memcpy(out, unzipped, size); ok = 1; }
        free(unzipped);
    } else if (plain_len == size) {
        memcpy(out, plain, size);
        ok = 1;
    }
    if (dump && *dump) {
        char path[4200];
        static int serial;
        snprintf(path, sizeof(path), "%s/chunk-%d-%u.bin", dump, __atomic_fetch_add(&serial, 1, __ATOMIC_RELAXED), size);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(plain, 1, plain_len, f); fclose(f); }
    }
    free(plain);
    return ok;
}

int chunk_download(HttpConn *conn, uint32_t depot, const ManifestChunk *chunk, const uint8_t key[32],
                   uint8_t *out, char *err, size_t errlen)
{
    char sha[41], path[128];
    hex_encode(chunk->sha, 20, sha);
    snprintf(path, sizeof(path), "/depot/%u/chunk/%s", depot, sha);
    uint8_t *body;
    size_t len;
    int status = http_get(conn, path, &body, &len, err, errlen);
    if (status != 200) {
        if (status) snprintf(err, errlen, "chunk %s: HTTP %d from %s", sha, status, conn->host);
        free(body);
        return status ? -1 : 0;
    }
    int ok = chunk_decode(body, len, key, out, chunk->size);
    free(body);
    uint8_t digest[20];
    if (ok) SHA1(out, chunk->size, digest);
    if (!ok || memcmp(digest, chunk->sha, 20)) {
        snprintf(err, errlen, "chunk %s from %s failed verification", sha, conn->host);
        return -1;
    }
    return 1;
}

/* ---------------------------------------------------------------- manifests */

void manifest_free(Manifest *m)
{
    for (size_t i = 0; i < m->count; i++) {
        free(m->files[i].name);
        free(m->files[i].chunks);
        free(m->files[i].link);
    }
    free(m->files);
    memset(m, 0, sizeof(*m));
}

/* Base64 + Steam symmetric encryption, as used for encrypted file names. */
static char *decrypt_name(const uint8_t *text, size_t len, const uint8_t key[32])
{
    char *b64 = calloc(len + 1, 1);
    uint8_t *raw = malloc(len + 1), *plain = malloc(len + 1);
    char *out = NULL;
    size_t clean = 0;
    if (!b64 || !raw || !plain) { free(b64); free(raw); free(plain); return NULL; }
    for (size_t i = 0; i < len; i++)
        if (text[i] != '\n' && text[i] != '\r') b64[clean++] = (char)text[i];
    if (clean % 4 == 0) {
        int n = EVP_DecodeBlock(raw, (const unsigned char *)b64, (int)clean);
        if (n > 0) {
            for (size_t i = clean; i > 0 && b64[i - 1] == '='; i--) n--;
            size_t plain_len;
            if (steam_symmetric_decrypt(raw, (size_t)n, key, plain, &plain_len)) {
                while (plain_len && !plain[plain_len - 1]) plain_len--;
                if ((out = malloc(plain_len + 1))) {
                    memcpy(out, plain, plain_len);
                    out[plain_len] = 0;
                }
            }
        }
    }
    free(b64);
    free(raw);
    free(plain);
    return out;
}

static char *plain_name(const uint8_t *text, size_t len)
{
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, text, len);
    out[len] = 0;
    while (len && !out[len - 1]) len--;
    out[len] = 0;
    return out;
}

static void normalize_path(char *name)
{
    for (char *p = name; *p; p++) if (*p == '\\') *p = '/';
}

static int parse_file(const uint8_t *data, size_t len, const uint8_t *key, ManifestFile *file)
{
    memset(file, 0, sizeof(*file));
    size_t cap = 0;
    PbReader r = { data, data + len };
    PbField f;
    while (pb_next(&r, &f) == 1) {
        switch (f.field) {
        case 1: free(file->name); file->name = key ? decrypt_name(f.data, f.len, key) : plain_name(f.data, f.len); break;
        case 2: file->size = f.value; break;
        case 3: file->flags = (uint32_t)f.value; break;
        case 5: if (f.len == 20) memcpy(file->sha, f.data, 20); break;
        case 6: {
            if (file->chunk_count == cap) {
                cap = cap ? cap * 2 : 4;
                ManifestChunk *grown = realloc(file->chunks, cap * sizeof(*grown));
                if (!grown) return 0;
                file->chunks = grown;
            }
            ManifestChunk *c = &file->chunks[file->chunk_count++];
            memset(c, 0, sizeof(*c));
            PbReader cr = { f.data, f.data + f.len };
            PbField cf;
            while (pb_next(&cr, &cf) == 1) {
                if (cf.field == 1 && cf.len == 20) memcpy(c->sha, cf.data, 20);
                else if (cf.field == 3) c->offset = cf.value;
                else if (cf.field == 4) c->size = (uint32_t)cf.value;
                else if (cf.field == 5) c->compressed = (uint32_t)cf.value;
            }
            break;
        }
        case 7:
            if (f.len) { free(file->link); file->link = key ? decrypt_name(f.data, f.len, key) : plain_name(f.data, f.len); }
            break;
        }
    }
    if (!file->name) return 0;
    normalize_path(file->name);
    if (file->link) normalize_path(file->link);
    return 1;
}

/* Parses the section stream of a depot manifest. key is NULL when names are already plain. */
static int manifest_parse(const uint8_t *data, size_t len, const uint8_t key[32], Manifest *out)
{
    memset(out, 0, sizeof(*out));
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    int encrypted = 0;
    for (size_t at = 0; at + 4 <= len;) {
        uint32_t magic = le32(data + at);
        if (magic == MANIFEST_END_MAGIC) break;
        if (at + 8 > len) return 0;
        uint32_t size = le32(data + at + 4);
        if (size > len - at - 8) return 0;
        const uint8_t *section = data + at + 8;
        if (magic == MANIFEST_PAYLOAD_MAGIC) {
            payload = section;
            payload_len = size;
        } else if (magic == MANIFEST_METADATA_MAGIC) {
            PbReader r = { section, section + size };
            PbField f;
            while (pb_next(&r, &f) == 1) {
                if (f.field == 1) out->depot = (uint32_t)f.value;
                else if (f.field == 2) out->gid = f.value;
                else if (f.field == 4) encrypted = f.value != 0;
            }
        } else if (magic != MANIFEST_SIGNATURE_MAGIC) {
            return 0;
        }
        at += 8 + size;
    }
    if (!payload || (encrypted && !key)) return 0;
    size_t cap = 0;
    PbReader r = { payload, payload + payload_len };
    PbField f;
    while (pb_next(&r, &f) == 1) {
        if (f.field != 1 || f.type != PB_BYTES) continue;
        if (out->count == cap) {
            cap = cap ? cap * 2 : 256;
            ManifestFile *grown = realloc(out->files, cap * sizeof(*grown));
            if (!grown) { manifest_free(out); return 0; }
            out->files = grown;
        }
        ManifestFile *file = &out->files[out->count];
        if (!parse_file(f.data, f.len, encrypted ? key : NULL, file)) {
            free(file->name); free(file->chunks); free(file->link);
            manifest_free(out);
            return 0;
        }
        out->count++;
        out->size += file->size;
        for (size_t i = 0; i < file->chunk_count; i++) out->compressed += file->chunks[i].compressed;
    }
    return 1;
}

int manifest_download(HttpConn *conn, uint32_t depot, uint64_t gid, uint64_t code, const uint8_t key[32],
                      Manifest *out, char *err, size_t errlen)
{
    char path[160];
    if (code) snprintf(path, sizeof(path), "/depot/%u/manifest/%llu/5/%llu", depot, (unsigned long long)gid,
                       (unsigned long long)code);
    else snprintf(path, sizeof(path), "/depot/%u/manifest/%llu/5", depot, (unsigned long long)gid);
    uint8_t *body;
    size_t len;
    int status = http_get(conn, path, &body, &len, err, errlen);
    if (status != 200) {
        if (status) snprintf(err, errlen, "manifest %u/%llu: HTTP %d from %s", depot, (unsigned long long)gid,
                             status, conn->host);
        free(body);
        return 0;
    }
    size_t raw_len = len;
    uint8_t *raw = len >= 4 && le32(body) == 0x04034b50u ? unzip_first(body, len, &raw_len) : body;
    int ok = raw && manifest_parse(raw, raw_len, key, out);
    if (raw != body) free(raw);
    free(body);
    if (!ok) snprintf(err, errlen, "manifest %u/%llu could not be read", depot, (unsigned long long)gid);
    return ok;
}

static void put_section(FILE *f, uint32_t magic, const PbBuf *b)
{
    uint32_t header[2] = { magic, (uint32_t)b->len };
    fwrite(header, 4, 2, f);
    if (b->len) fwrite(b->data, 1, b->len, f);
}

int manifest_save(const Manifest *m, const char *path)
{
    PbBuf payload = {0}, metadata = {0};
    for (size_t i = 0; i < m->count; i++) {
        const ManifestFile *file = &m->files[i];
        PbBuf mapping = {0};
        pb_string(&mapping, 1, file->name);
        pb_u64(&mapping, 2, file->size);
        pb_u64(&mapping, 3, file->flags);
        pb_bytes(&mapping, 5, file->sha, 20);
        for (size_t c = 0; c < file->chunk_count; c++) {
            PbBuf chunk = {0};
            pb_bytes(&chunk, 1, file->chunks[c].sha, 20);
            pb_u64(&chunk, 3, file->chunks[c].offset);
            pb_u64(&chunk, 4, file->chunks[c].size);
            pb_u64(&chunk, 5, file->chunks[c].compressed);
            pb_msg(&mapping, 6, &chunk);
            pb_free(&chunk);
        }
        if (file->link) pb_string(&mapping, 7, file->link);
        pb_msg(&payload, 1, &mapping);
        pb_free(&mapping);
    }
    pb_u64(&metadata, 1, m->depot);
    pb_u64(&metadata, 2, m->gid);
    pb_u64(&metadata, 4, 0);
    int ok = !payload.failed && !metadata.failed;
    char tmp[4200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = ok ? fopen(tmp, "wb") : NULL;
    if (f) {
        put_section(f, MANIFEST_PAYLOAD_MAGIC, &payload);
        put_section(f, MANIFEST_METADATA_MAGIC, &metadata);
        uint32_t end = MANIFEST_END_MAGIC;
        fwrite(&end, 4, 1, f);
        ok = !ferror(f);
        ok = fclose(f) == 0 && ok && rename(tmp, path) == 0;
    } else {
        ok = 0;
    }
    pb_free(&payload);
    pb_free(&metadata);
    return ok;
}

int manifest_load(const char *path, Manifest *out)
{
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = size > 0 ? malloc((size_t)size) : NULL;
    int ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    ok = ok && manifest_parse(data, (size_t)size, NULL, out);
    free(data);
    return ok;
}
