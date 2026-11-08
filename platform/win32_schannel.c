/* SSPI Schannel: a TLS client on the host's OpenSSL.
 *
 * Callers (libcurl's schannel backend, WinHTTP-style stacks) own the socket and move TLS records through these calls:
 *  - InitializeSecurityContext runs the handshake. Input tokens are fed one complete TLS record at a time, so bytes
 *    that follow the end of the handshake are reported back as SECBUFFER_EXTRA, as Windows does.
 *  - EncryptMessage turns the plaintext in the SECBUFFER_DATA buffer into one TLS record laid out over the
 *    STREAM_HEADER, DATA and STREAM_TRAILER buffers (callers send them back to back).
 *  - DecryptMessage decrypts the first complete record in place: header, data and trailer buffers describe it and the
 *    bytes after it come back as SECBUFFER_EXTRA. An incomplete record gives SEC_E_INCOMPLETE_MESSAGE.
 * Server certificates are verified against the host's CA store and the target name, unless the credentials ask for
 * manual validation (SCH_CRED_MANUAL_CRED_VALIDATION) or no name check (SCH_CRED_NO_SERVERNAME_CHECK). */
#include "win32_schannel.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* ---- OpenSSL, resolved at run time ---- */
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_method_st SSL_METHOD;
typedef struct bio_st BIO;
typedef struct bio_method_st BIO_METHOD;
static struct {
    int state;                                        /* 0 not tried, 1 ready, -1 unavailable */
    const SSL_METHOD *(*TLS_client_method)(void);
    SSL_CTX *(*SSL_CTX_new)(const SSL_METHOD *);
    void (*SSL_CTX_free)(SSL_CTX *);
    int (*SSL_CTX_set_default_verify_paths)(SSL_CTX *);
    void (*SSL_CTX_set_verify)(SSL_CTX *, int, void *);
    long (*SSL_CTX_ctrl)(SSL_CTX *, int, long, void *);
    SSL *(*SSL_new)(SSL_CTX *);
    void (*SSL_free)(SSL *);
    void (*SSL_set_bio)(SSL *, BIO *, BIO *);
    void (*SSL_set_connect_state)(SSL *);
    int (*SSL_do_handshake)(SSL *);
    int (*SSL_get_error)(const SSL *, int);
    int (*SSL_read)(SSL *, void *, int);
    int (*SSL_write)(SSL *, const void *, int);
    int (*SSL_shutdown)(SSL *);
    int (*SSL_set1_host)(SSL *, const char *);
    long (*SSL_ctrl)(SSL *, int, long, void *);
    int (*SSL_set_alpn_protos)(SSL *, const unsigned char *, unsigned int);
    void (*SSL_get0_alpn_selected)(const SSL *, const unsigned char **, unsigned int *);
    int (*SSL_version)(const SSL *);
    long (*SSL_get_verify_result)(const SSL *);
    const void *(*SSL_get_current_cipher)(const SSL *);
    const char *(*SSL_CIPHER_get_name)(const void *);
    const BIO_METHOD *(*BIO_s_mem)(void);
    BIO *(*BIO_new)(const BIO_METHOD *);
    int (*BIO_write)(BIO *, const void *, int);
    int (*BIO_read)(BIO *, void *, int);
    long (*BIO_ctrl)(BIO *, int, long, void *);
    unsigned long (*ERR_get_error)(void);
    void (*ERR_error_string_n)(unsigned long, char *, size_t);
    void (*ERR_clear_error)(void);
    const char *(*X509_verify_cert_error_string)(long);
} O;

static pthread_mutex_t g_load_lock = PTHREAD_MUTEX_INITIALIZER;

static int openssl_ready(void)
{
    pthread_mutex_lock(&g_load_lock);
    if (O.state) { int ready = O.state > 0; pthread_mutex_unlock(&g_load_lock); return ready; }
    O.state = -1;
    void *ssl = dlopen("libssl.so.3", RTLD_NOW | RTLD_GLOBAL);
    if (!ssl) ssl = dlopen("libssl.so", RTLD_NOW | RTLD_GLOBAL);
    if (!ssl) {
        fprintf(stderr, "[SCHANNEL] libssl is not available: TLS connections fail (%s)\n", dlerror());
        pthread_mutex_unlock(&g_load_lock);
        return 0;
    }
    int complete = 1;
#define LOAD(name) do { *(void **)&O.name = dlsym(ssl, #name); if (!O.name) { \
        fprintf(stderr, "[SCHANNEL] libssl lacks %s\n", #name); complete = 0; } } while (0)
    LOAD(TLS_client_method); LOAD(SSL_CTX_new); LOAD(SSL_CTX_free); LOAD(SSL_CTX_set_default_verify_paths);
    LOAD(SSL_CTX_set_verify); LOAD(SSL_CTX_ctrl); LOAD(SSL_new); LOAD(SSL_free); LOAD(SSL_set_bio);
    LOAD(SSL_set_connect_state); LOAD(SSL_do_handshake); LOAD(SSL_get_error); LOAD(SSL_read); LOAD(SSL_write);
    LOAD(SSL_shutdown); LOAD(SSL_set1_host); LOAD(SSL_ctrl); LOAD(SSL_set_alpn_protos); LOAD(SSL_get0_alpn_selected);
    LOAD(SSL_version); LOAD(SSL_get_verify_result); LOAD(SSL_get_current_cipher); LOAD(SSL_CIPHER_get_name);
    LOAD(BIO_s_mem); LOAD(BIO_new); LOAD(BIO_write); LOAD(BIO_read); LOAD(BIO_ctrl);
    LOAD(ERR_get_error); LOAD(ERR_error_string_n); LOAD(ERR_clear_error); LOAD(X509_verify_cert_error_string);
#undef LOAD
    O.state = complete ? 1 : -1;
    pthread_mutex_unlock(&g_load_lock);
    return complete;
}

#define BIO_CTRL_PENDING 10
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define SSL_CTRL_SET_MIN_PROTO_VERSION 123
#define SSL_ERROR_WANT_READ 2
#define SSL_ERROR_WANT_WRITE 3
#define SSL_ERROR_ZERO_RETURN 6
#define X509_V_ERR_HOSTNAME_MISMATCH 62

/* ---- SSPI constants ---- */
#define SEC_E_OK                    0x00000000u
#define SEC_I_CONTINUE_NEEDED       0x00090312u
#define SEC_I_CONTEXT_EXPIRED       0x00090317u
#define SEC_E_INSUFFICIENT_MEMORY   0x80090300u
#define SEC_E_INVALID_HANDLE        0x80090301u
#define SEC_E_UNSUPPORTED_FUNCTION  0x80090302u
#define SEC_E_SECPKG_NOT_FOUND      0x80090305u
#define SEC_E_INVALID_TOKEN         0x80090308u
#define SEC_E_INCOMPLETE_MESSAGE    0x80090318u
#define SEC_E_BUFFER_TOO_SMALL      0x80090321u
#define SEC_E_WRONG_PRINCIPAL       0x80090322u
#define SEC_E_UNTRUSTED_ROOT        0x80090325u
#define SEC_E_ILLEGAL_MESSAGE       0x80090326u
#define SEC_E_INTERNAL_ERROR        0x80090304u

#define SECBUFFER_EMPTY 0
#define SECBUFFER_DATA 1
#define SECBUFFER_TOKEN 2
#define SECBUFFER_MISSING 4
#define SECBUFFER_EXTRA 5
#define SECBUFFER_STREAM_TRAILER 6
#define SECBUFFER_STREAM_HEADER 7
#define SECBUFFER_ALERT 17
#define SECBUFFER_APPLICATION_PROTOCOLS 18

#define ISC_REQ_ALLOCATE_MEMORY 0x00000100u
#define ISC_RET_REPLAY_DETECT   0x00000004u
#define ISC_RET_SEQUENCE_DETECT 0x00000008u
#define ISC_RET_CONFIDENTIALITY 0x00000010u
#define ISC_RET_ALLOCATED_MEMORY 0x00000100u
#define ISC_RET_STREAM          0x00008000u

#define SCH_CRED_MANUAL_CRED_VALIDATION 0x00000008u
#define SCH_CRED_NO_SERVERNAME_CHECK    0x00000004u

#define SECPKG_ATTR_STREAM_SIZES 4
#define SECPKG_ATTR_APPLICATION_PROTOCOL 35
#define SECPKG_ATTR_CONNECTION_INFO 0x5a

#define TLS_HEADER 5u
#define TLS_TRAILER 96u                 /* worst-case per-record overhead after the 5-byte header */
#define TLS_MAX_PLAINTEXT 16384u

/* SecBufferDesc { ULONG ulVersion; ULONG cBuffers; SecBuffer *pBuffers; }, SecBuffer { ULONG cb; ULONG type; void *pv; } */
typedef struct { u32 size; u32 type; u8 *data; } SecBuffer;
static u32 desc_count(const u8 *desc) { return desc ? *(const u32 *)(desc + 4) : 0; }
static SecBuffer *desc_buffers(u8 *desc) { return desc ? *(SecBuffer **)(desc + 8) : NULL; }
static SecBuffer *desc_find(u8 *desc, u32 type)
{
    SecBuffer *b = desc_buffers(desc);
    for (u32 i = 0; b && i < desc_count(desc); ++i) if ((b[i].type & 0x0fffffffu) == type) return &b[i];
    return NULL;
}

/* ---- Credentials and contexts; a handle is { pointer, magic } ---- */
#define CRED_MAGIC 0x43524544u
#define CTX_MAGIC  0x544c5343u
typedef struct { SSL_CTX *ctx; int verify; int check_name; } Credential;
typedef struct {
    pthread_mutex_t lock;
    Credential *credential;
    SSL *ssl;
    BIO *in, *out;
    int done, failed, shutting_down;
    char host[256];
} Context;

static Credential *credential_of(const u64 *handle)
    { return handle && handle[1] == CRED_MAGIC ? (Credential *)(uintptr_t)handle[0] : NULL; }
static Context *context_of(const u64 *handle)
    { return handle && handle[1] == CTX_MAGIC ? (Context *)(uintptr_t)handle[0] : NULL; }

static void log_openssl(const char *what)
{
    unsigned long code;
    char text[256];
    int any = 0;
    while ((code = O.ERR_get_error())) {
        O.ERR_error_string_n(code, text, sizeof(text));
        fprintf(stderr, "[SCHANNEL] %s: %s\n", what, text);
        any = 1;
    }
    if (!any) fprintf(stderr, "[SCHANNEL] %s\n", what);
}

static int package_is_schannel(const char *name)
{
    if (!name) return 0;
    return strstr(name, "Unified Security Protocol") || !strcasecmp(name, "Schannel") ||
           strstr(name, "Schannel") || !strcasecmp(name, "Default TLS SSP");
}

static void narrow(const u16 *wide, char *out, size_t size)
{
    size_t i = 0;
    for (; wide && wide[i] && i + 1 < size; ++i) out[i] = (char)(wide[i] < 0x80 ? wide[i] : '?');
    out[i] = 0;
}

static void never_expires(int64_t *expiry) { if (expiry) *expiry = 0x7fffffffffffffffLL; }

uint64_t BEER_SSPI_MS beer_AcquireCredentialsHandleA(const char *principal, const char *package, uint32_t use,
                                                     void *logon_id, const uint8_t *auth_data, void *get_key,
                                                     void *get_key_argument, uint64_t *credential, int64_t *expiry)
{
    (void)principal; (void)logon_id; (void)get_key; (void)get_key_argument;
    if (!package_is_schannel(package)) return SEC_E_SECPKG_NOT_FOUND;
    if (!(use & 2)) return SEC_E_UNSUPPORTED_FUNCTION;        /* only SECPKG_CRED_OUTBOUND (TLS client) */
    if (!credential) return SEC_E_INTERNAL_ERROR;
    if (!openssl_ready()) return SEC_E_SECPKG_NOT_FOUND;
    u32 flags = 0;
    if (auth_data) {
        u32 version = *(const u32 *)auth_data;
        if (version == 4) flags = *(const u32 *)(auth_data + 72);      /* SCHANNEL_CRED.dwFlags */
        else if (version == 5) flags = *(const u32 *)(auth_data + 52); /* SCH_CREDENTIALS.dwFlags */
    }
    Credential *c = calloc(1, sizeof(*c));
    if (!c) return SEC_E_INSUFFICIENT_MEMORY;
    c->ctx = O.SSL_CTX_new(O.TLS_client_method());
    if (!c->ctx) { log_openssl("SSL_CTX_new failed"); free(c); return SEC_E_INTERNAL_ERROR; }
    O.SSL_CTX_ctrl(c->ctx, SSL_CTRL_SET_MIN_PROTO_VERSION, 0x0303, NULL);   /* TLS 1.2 or later */
    c->verify = !(flags & SCH_CRED_MANUAL_CRED_VALIDATION);
    c->check_name = c->verify && !(flags & SCH_CRED_NO_SERVERNAME_CHECK);
    if (c->verify) {
        O.SSL_CTX_set_default_verify_paths(c->ctx);
        O.SSL_CTX_set_verify(c->ctx, 1 /* SSL_VERIFY_PEER */, NULL);
    } else {
        O.SSL_CTX_set_verify(c->ctx, 0, NULL);
    }
    credential[0] = (u64)(uintptr_t)c;
    credential[1] = CRED_MAGIC;
    never_expires(expiry);
    return SEC_E_OK;
}

uint64_t BEER_SSPI_MS beer_AcquireCredentialsHandleW(const uint16_t *principal, const uint16_t *package, uint32_t use,
                                                     void *logon_id, const uint8_t *auth_data, void *get_key,
                                                     void *get_key_argument, uint64_t *credential, int64_t *expiry)
{
    (void)principal;
    char name[128];
    narrow(package, name, sizeof(name));
    return beer_AcquireCredentialsHandleA(NULL, package ? name : NULL, use, logon_id, auth_data, get_key,
                                          get_key_argument, credential, expiry);
}

uint64_t BEER_SSPI_MS beer_FreeCredentialsHandle(uint64_t *credential)
{
    Credential *c = credential_of(credential);
    if (!c) return SEC_E_INVALID_HANDLE;
    O.SSL_CTX_free(c->ctx);
    free(c);
    credential[0] = credential[1] = 0;
    return SEC_E_OK;
}

/* Moves what OpenSSL wrote into the caller's output token (allocated when ISC_REQ_ALLOCATE_MEMORY). */
static u32 emit_output(Context *x, u8 *output, u32 requirements)
{
    SecBuffer *token = desc_find(output, SECBUFFER_TOKEN);
    SecBuffer *alert = desc_find(output, SECBUFFER_ALERT);
    if (alert) alert->size = 0;
    long pending = O.BIO_ctrl(x->out, BIO_CTRL_PENDING, 0, NULL);
    if (!token) return pending > 0 ? SEC_E_BUFFER_TOO_SMALL : SEC_E_OK;
    if (pending <= 0) {
        if (requirements & ISC_REQ_ALLOCATE_MEMORY) token->data = NULL;
        token->size = 0;
        return SEC_E_OK;
    }
    if (requirements & ISC_REQ_ALLOCATE_MEMORY) {
        token->data = malloc((size_t)pending);
        if (!token->data) return SEC_E_INSUFFICIENT_MEMORY;
    } else if (!token->data || token->size < (u32)pending) {
        return SEC_E_BUFFER_TOO_SMALL;
    }
    token->size = (u32)O.BIO_read(x->out, token->data, (int)pending);
    return SEC_E_OK;
}

static u32 handshake_failure(Context *x)
{
    x->failed = 1;
    long verify = O.SSL_get_verify_result(x->ssl);
    if (verify != 0) {
        fprintf(stderr, "[SCHANNEL] %s: certificate rejected: %s\n", x->host, O.X509_verify_cert_error_string(verify));
        O.ERR_clear_error();
        return verify == X509_V_ERR_HOSTNAME_MISMATCH ? SEC_E_WRONG_PRINCIPAL : SEC_E_UNTRUSTED_ROOT;
    }
    char what[300];
    snprintf(what, sizeof(what), "%s: handshake failed", x->host);
    log_openssl(what);
    return SEC_E_ILLEGAL_MESSAGE;
}

static u32 tls_record_length(const u8 *data, u32 available)
{
    if (available < TLS_HEADER) return 0;
    return TLS_HEADER + ((u32)data[3] << 8 | data[4]);
}

static uint64_t initialize(uint64_t *credential, uint64_t *context, const char *target, uint32_t requirements,
                           uint8_t *input, uint64_t *new_context, uint8_t *output, uint32_t *attributes, int64_t *expiry)
{
    if (attributes) *attributes = ISC_RET_REPLAY_DETECT | ISC_RET_SEQUENCE_DETECT | ISC_RET_CONFIDENTIALITY |
                                  ISC_RET_STREAM | (requirements & ISC_REQ_ALLOCATE_MEMORY ? ISC_RET_ALLOCATED_MEMORY : 0);
    never_expires(expiry);
    Context *x = context_of(context);
    if (!x) {                                                /* first call: a new connection */
        Credential *c = credential_of(credential);
        if (!c || !new_context) return SEC_E_INVALID_HANDLE;
        x = calloc(1, sizeof(*x));
        if (!x) return SEC_E_INSUFFICIENT_MEMORY;
        pthread_mutex_init(&x->lock, NULL);
        x->credential = c;
        x->ssl = O.SSL_new(c->ctx);
        x->in = O.BIO_new(O.BIO_s_mem());
        x->out = O.BIO_new(O.BIO_s_mem());
        if (!x->ssl || !x->in || !x->out) { free(x); return SEC_E_INTERNAL_ERROR; }
        O.SSL_set_bio(x->ssl, x->in, x->out);
        O.SSL_set_connect_state(x->ssl);
        snprintf(x->host, sizeof(x->host), "%s", target ? target : "");
        if (x->host[0]) {
            O.SSL_ctrl(x->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, 0 /* host_name */, x->host);
            if (c->check_name) O.SSL_set1_host(x->ssl, x->host);
        }
        SecBuffer *protocols = desc_find(input, SECBUFFER_APPLICATION_PROTOCOLS);
        if (protocols && protocols->data && protocols->size >= 10) {
            /* SEC_APPLICATION_PROTOCOLS: ULONG size; SEC_APPLICATION_PROTOCOL_LIST { ext (2 = ALPN); USHORT size; list } */
            if (*(const u32 *)(protocols->data + 4) == 2) {
                u16 size = *(const u16 *)(protocols->data + 8);
                if (10u + size <= protocols->size) O.SSL_set_alpn_protos(x->ssl, protocols->data + 10, size);
            }
        }
        new_context[0] = (u64)(uintptr_t)x;
        new_context[1] = CTX_MAGIC;
        pthread_mutex_lock(&x->lock);
        int result = O.SSL_do_handshake(x->ssl);
        int error = O.SSL_get_error(x->ssl, result);
        u32 status = result <= 0 && error != SSL_ERROR_WANT_READ ? handshake_failure(x) : emit_output(x, output, requirements);
        pthread_mutex_unlock(&x->lock);
        return status == SEC_E_OK ? SEC_I_CONTINUE_NEEDED : status;
    }
    if (new_context && new_context != context) { new_context[0] = context[0]; new_context[1] = context[1]; }
    pthread_mutex_lock(&x->lock);
    if (x->shutting_down) {                                 /* after ApplyControlToken(SCHANNEL_SHUTDOWN) */
        O.SSL_shutdown(x->ssl);
        u32 status = emit_output(x, output, requirements);
        pthread_mutex_unlock(&x->lock);
        return status;
    }
    if (x->failed) { pthread_mutex_unlock(&x->lock); return SEC_E_ILLEGAL_MESSAGE; }
    SecBuffer *in = desc_buffers(input);
    if (!in || !desc_count(input) || !in[0].data) { pthread_mutex_unlock(&x->lock); return SEC_E_INVALID_TOKEN; }
    u32 offset = 0, fed = 0;
    while (!x->done) {                                      /* one complete record at a time */
        u32 length = tls_record_length(in[0].data + offset, in[0].size - offset);
        if (!length || offset + length > in[0].size) break;
        O.BIO_write(x->in, in[0].data + offset, (int)length);
        offset += length;
        ++fed;
        int result = O.SSL_do_handshake(x->ssl);
        if (result == 1) {
            x->done = 1;
            const unsigned char *alpn = NULL;
            unsigned int alpn_length = 0;
            O.SSL_get0_alpn_selected(x->ssl, &alpn, &alpn_length);
            fprintf(stderr, "[SCHANNEL] %s: TLS %s established (%s%s%.*s)\n", x->host,
                    O.SSL_version(x->ssl) == 0x0304 ? "1.3" : O.SSL_version(x->ssl) == 0x0303 ? "1.2" : "?",
                    O.SSL_CIPHER_get_name(O.SSL_get_current_cipher(x->ssl)), alpn_length ? ", ALPN " : "", (int)alpn_length, alpn ? (const char *)alpn : "");
            break;
        }
        int error = O.SSL_get_error(x->ssl, result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            u32 status = handshake_failure(x);
            emit_output(x, output, requirements);            /* the alert, if any */
            pthread_mutex_unlock(&x->lock);
            return status;
        }
    }
    if (!fed) {
        if (desc_count(input) > 1) { in[1].type = SECBUFFER_MISSING; in[1].size = 0; }
        pthread_mutex_unlock(&x->lock);
        return SEC_E_INCOMPLETE_MESSAGE;
    }
    if (desc_count(input) > 1) {
        u32 remaining = in[0].size - offset;
        in[1].type = remaining ? SECBUFFER_EXTRA : SECBUFFER_EMPTY;
        in[1].size = remaining;
        in[1].data = remaining ? in[0].data + offset : NULL;
    }
    u32 status = emit_output(x, output, requirements);
    pthread_mutex_unlock(&x->lock);
    if (status != SEC_E_OK) return status;
    return x->done ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
}

uint64_t BEER_SSPI_MS beer_InitializeSecurityContextA(uint64_t *credential, uint64_t *context, const char *target,
                                                      uint32_t requirements, uint32_t reserved1, uint32_t representation,
                                                      uint8_t *input, uint32_t reserved2, uint64_t *new_context,
                                                      uint8_t *output, uint32_t *attributes, int64_t *expiry)
{
    (void)reserved1; (void)representation; (void)reserved2;
    if (!openssl_ready()) return SEC_E_INTERNAL_ERROR;
    return initialize(credential, context, target, requirements, input, new_context, output, attributes, expiry);
}

uint64_t BEER_SSPI_MS beer_InitializeSecurityContextW(uint64_t *credential, uint64_t *context, const uint16_t *target,
                                                      uint32_t requirements, uint32_t reserved1, uint32_t representation,
                                                      uint8_t *input, uint32_t reserved2, uint64_t *new_context,
                                                      uint8_t *output, uint32_t *attributes, int64_t *expiry)
{
    (void)reserved1; (void)representation; (void)reserved2;
    if (!openssl_ready()) return SEC_E_INTERNAL_ERROR;
    char host[256];
    narrow(target, host, sizeof(host));
    return initialize(credential, context, target ? host : NULL, requirements, input, new_context, output, attributes, expiry);
}

uint64_t BEER_SSPI_MS beer_DeleteSecurityContext(uint64_t *context)
{
    Context *x = context_of(context);
    if (!x) return SEC_E_INVALID_HANDLE;
    O.SSL_free(x->ssl);                                      /* also frees both BIOs */
    pthread_mutex_destroy(&x->lock);
    free(x);
    context[0] = context[1] = 0;
    return SEC_E_OK;
}

uint64_t BEER_SSPI_MS beer_ApplyControlToken(uint64_t *context, uint8_t *input)
{
    Context *x = context_of(context);
    if (!x) return SEC_E_INVALID_HANDLE;
    SecBuffer *token = desc_find(input, SECBUFFER_TOKEN);
    if (!token || !token->data || token->size < 4) return SEC_E_INVALID_TOKEN;
    if (*(const u32 *)token->data != 1) return SEC_E_UNSUPPORTED_FUNCTION;   /* only SCHANNEL_SHUTDOWN */
    pthread_mutex_lock(&x->lock);
    x->shutting_down = 1;
    pthread_mutex_unlock(&x->lock);
    return SEC_E_OK;
}

static uint64_t query(uint64_t *context, uint32_t attribute, void *buffer)
{
    Context *x = context_of(context);
    if (!x) return SEC_E_INVALID_HANDLE;
    if (!buffer) return SEC_E_INTERNAL_ERROR;
    switch (attribute) {
    case SECPKG_ATTR_STREAM_SIZES: {                        /* cbHeader, cbTrailer, cbMaximumMessage, cBuffers, cbBlockSize */
        u32 *sizes = buffer;
        sizes[0] = TLS_HEADER; sizes[1] = TLS_TRAILER; sizes[2] = TLS_MAX_PLAINTEXT; sizes[3] = 4; sizes[4] = 16;
        return SEC_E_OK;
    }
    case SECPKG_ATTR_APPLICATION_PROTOCOL: {                /* status, ext, size, id[255] */
        u8 *out = buffer;
        memset(out, 0, 4 + 4 + 1 + 255);
        const unsigned char *alpn = NULL;
        unsigned int length = 0;
        pthread_mutex_lock(&x->lock);
        O.SSL_get0_alpn_selected(x->ssl, &alpn, &length);
        pthread_mutex_unlock(&x->lock);
        if (alpn && length && length <= 255) {
            *(u32 *)out = 1;                                 /* SecApplicationProtocolNegotiationStatus_Success */
            *(u32 *)(out + 4) = 2;                           /* ALPN */
            out[8] = (u8)length;
            memcpy(out + 9, alpn, length);
        }
        return SEC_E_OK;
    }
    case SECPKG_ATTR_CONNECTION_INFO: {                     /* protocol, cipher, strength, hash, strength, exchange, strength */
        u32 *info = buffer;
        memset(info, 0, 7 * sizeof(u32));
        pthread_mutex_lock(&x->lock);
        int version = O.SSL_version(x->ssl);
        pthread_mutex_unlock(&x->lock);
        info[0] = version == 0x0304 ? 0x00002000u : 0x00000800u;   /* SP_PROT_TLS1_3_CLIENT / SP_PROT_TLS1_2_CLIENT */
        info[1] = 0x6610; info[2] = 256;                     /* CALG_AES_256 */
        info[3] = 0x800c; info[4] = 256;                     /* CALG_SHA_256 */
        info[5] = 0xaa02; info[6] = 256;                     /* CALG_ECDH */
        return SEC_E_OK;
    }
    default:
        return SEC_E_UNSUPPORTED_FUNCTION;
    }
}

uint64_t BEER_SSPI_MS beer_QueryContextAttributesA(uint64_t *context, uint32_t attribute, void *buffer)
    { return query(context, attribute, buffer); }
uint64_t BEER_SSPI_MS beer_QueryContextAttributesW(uint64_t *context, uint32_t attribute, void *buffer)
    { return query(context, attribute, buffer); }

uint64_t BEER_SSPI_MS beer_FreeContextBuffer(void *buffer) { free(buffer); return SEC_E_OK; }

uint64_t BEER_SSPI_MS beer_EncryptMessage(uint64_t *context, uint32_t quality, uint8_t *message, uint32_t sequence)
{
    (void)quality; (void)sequence;
    Context *x = context_of(context);
    if (!x) return SEC_E_INVALID_HANDLE;
    SecBuffer *header = desc_find(message, SECBUFFER_STREAM_HEADER);
    SecBuffer *data = desc_find(message, SECBUFFER_DATA);
    SecBuffer *trailer = desc_find(message, SECBUFFER_STREAM_TRAILER);
    if (!header || !data || !trailer || (!data->data && data->size)) return SEC_E_INVALID_TOKEN;
    if (data->size > TLS_MAX_PLAINTEXT) return SEC_E_BUFFER_TOO_SMALL;
    pthread_mutex_lock(&x->lock);
    if (!x->done) { pthread_mutex_unlock(&x->lock); return SEC_E_INVALID_HANDLE; }
    int written = data->size ? O.SSL_write(x->ssl, data->data, (int)data->size) : 0;
    if (data->size && written != (int)data->size) {
        log_openssl("EncryptMessage failed");
        pthread_mutex_unlock(&x->lock);
        return SEC_E_INTERNAL_ERROR;
    }
    long pending = O.BIO_ctrl(x->out, BIO_CTRL_PENDING, 0, NULL);
    u8 record[TLS_HEADER + TLS_MAX_PLAINTEXT + 2 * TLS_TRAILER];
    if (pending < 0 || pending > (long)sizeof(record) ||
        (u64)pending > (u64)header->size + data->size + trailer->size) {
        pthread_mutex_unlock(&x->lock);
        return SEC_E_BUFFER_TOO_SMALL;
    }
    u32 total = pending > 0 ? (u32)O.BIO_read(x->out, record, (int)pending) : 0;
    pthread_mutex_unlock(&x->lock);
    /* Lay the record over header, data and trailer in order (callers send them back to back). */
    u32 at = 0;
    u32 header_part = total < header->size ? total : header->size;
    memcpy(header->data, record, header_part);
    at += header_part;
    u32 data_part = total - at < data->size ? total - at : data->size;
    memcpy(data->data, record + at, data_part);
    at += data_part;
    u32 trailer_part = total - at;
    memcpy(trailer->data, record + at, trailer_part);
    header->size = header_part;
    data->size = data_part;
    trailer->size = trailer_part;
    return SEC_E_OK;
}

uint64_t BEER_SSPI_MS beer_DecryptMessage(uint64_t *context, uint8_t *message, uint32_t sequence, uint32_t *quality)
{
    (void)sequence;
    if (quality) *quality = 0;
    Context *x = context_of(context);
    if (!x) return SEC_E_INVALID_HANDLE;
    SecBuffer *buffers = desc_buffers(message);
    u32 count = desc_count(message);
    SecBuffer *source = desc_find(message, SECBUFFER_DATA);
    if (!source || !source->data || count < 4) return SEC_E_INVALID_TOKEN;
    u8 *bytes = source->data;
    u32 available = source->size;
    u32 length = tls_record_length(bytes, available);
    if (!length || length > available) {
        for (u32 i = 0; i < count; ++i)
            if (&buffers[i] != source && buffers[i].type == SECBUFFER_EMPTY) {
                buffers[i].type = SECBUFFER_MISSING;
                buffers[i].size = length ? length - available : TLS_HEADER - available;
                break;
            }
        return SEC_E_INCOMPLETE_MESSAGE;
    }
    pthread_mutex_lock(&x->lock);
    if (!x->done) { pthread_mutex_unlock(&x->lock); return SEC_E_INVALID_HANDLE; }
    O.BIO_write(x->in, bytes, (int)length);
    u8 plain[TLS_MAX_PLAINTEXT + 256];
    u32 plain_length = 0;
    u32 status = SEC_E_OK;
    for (;;) {
        int got = O.SSL_read(x->ssl, plain + plain_length, (int)(sizeof(plain) - plain_length));
        if (got > 0) { plain_length += (u32)got; if (plain_length < sizeof(plain)) continue; break; }
        int error = O.SSL_get_error(x->ssl, got);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) break;   /* record consumed */
        if (error == SSL_ERROR_ZERO_RETURN) { status = SEC_I_CONTEXT_EXPIRED; break; }  /* close_notify */
        log_openssl("DecryptMessage failed");
        status = SEC_E_ILLEGAL_MESSAGE;
        break;
    }
    pthread_mutex_unlock(&x->lock);
    if (status == SEC_E_ILLEGAL_MESSAGE) return status;
    if (plain_length > length - TLS_HEADER) plain_length = length - TLS_HEADER;   /* cannot exceed the record */
    memcpy(bytes + TLS_HEADER, plain, plain_length);
    buffers[0] = (SecBuffer){ TLS_HEADER, SECBUFFER_STREAM_HEADER, bytes };
    buffers[1] = (SecBuffer){ plain_length, SECBUFFER_DATA, bytes + TLS_HEADER };
    buffers[2] = (SecBuffer){ length - TLS_HEADER - plain_length, SECBUFFER_STREAM_TRAILER, bytes + TLS_HEADER + plain_length };
    u32 extra = available - length;
    buffers[3] = extra ? (SecBuffer){ extra, SECBUFFER_EXTRA, bytes + length } : (SecBuffer){ 0, SECBUFFER_EMPTY, NULL };
    return status;
}

/* SecPkgInfoA/W { ULONG fCapabilities; USHORT wVersion; USHORT wRPCID; ULONG cbMaxToken; Name; Comment } */
static void *package_info(int wide)
{
    static const char name[] = "Microsoft Unified Security Protocol Provider";
    static const char comment[] = "Schannel Security Package";
    size_t name_length = sizeof(name), comment_length = sizeof(comment);
    size_t unit = wide ? 2 : 1;
    u8 *info = calloc(1, 32 + (name_length + comment_length) * unit);
    if (!info) return NULL;
    *(u32 *)info = 0x107b3;                                  /* integrity, privacy, connection, multi-required, extended error, impersonation, accept win32 name, stream */
    *(u16 *)(info + 4) = 1;
    *(u16 *)(info + 6) = 14;                                 /* UNISP_RPC_ID */
    *(u32 *)(info + 8) = 0x6000;                             /* cbMaxToken */
    u8 *strings = info + 32;
    *(u8 **)(info + 16) = strings;
    *(u8 **)(info + 24) = strings + name_length * unit;
    for (size_t i = 0; i < name_length; ++i) {
        if (wide) ((u16 *)strings)[i] = (u8)name[i]; else strings[i] = (u8)name[i];
    }
    u8 *second = strings + name_length * unit;
    for (size_t i = 0; i < comment_length; ++i) {
        if (wide) ((u16 *)second)[i] = (u8)comment[i]; else second[i] = (u8)comment[i];
    }
    return info;
}

uint64_t BEER_SSPI_MS beer_QuerySecurityPackageInfoA(const char *package, void **info)
{
    if (!info) return SEC_E_INTERNAL_ERROR;
    *info = NULL;
    if (!package_is_schannel(package)) return SEC_E_SECPKG_NOT_FOUND;
    *info = package_info(0);
    return *info ? SEC_E_OK : SEC_E_INSUFFICIENT_MEMORY;
}

uint64_t BEER_SSPI_MS beer_QuerySecurityPackageInfoW(const uint16_t *package, void **info)
{
    char name[128];
    narrow(package, name, sizeof(name));
    if (!info) return SEC_E_INTERNAL_ERROR;
    *info = NULL;
    if (!package_is_schannel(name)) return SEC_E_SECPKG_NOT_FOUND;
    *info = package_info(1);
    return *info ? SEC_E_OK : SEC_E_INSUFFICIENT_MEMORY;
}

uint64_t BEER_SSPI_MS beer_EnumerateSecurityPackagesA(uint32_t *count, void **info)
{
    if (!count || !info) return SEC_E_INTERNAL_ERROR;
    *info = package_info(0);
    *count = *info ? 1 : 0;
    return *info ? SEC_E_OK : SEC_E_INSUFFICIENT_MEMORY;
}

uint64_t BEER_SSPI_MS beer_EnumerateSecurityPackagesW(uint32_t *count, void **info)
{
    if (!count || !info) return SEC_E_INTERNAL_ERROR;
    *info = package_info(1);
    *count = *info ? 1 : 0;
    return *info ? SEC_E_OK : SEC_E_INSUFFICIENT_MEMORY;
}

uint64_t BEER_SSPI_MS beer_CompleteAuthToken(uint64_t *context, uint8_t *token)
{
    (void)token;
    return context_of(context) ? SEC_E_OK : SEC_E_INVALID_HANDLE;
}
