/* ---- crypt32: Authenticode signature inspection (first-party, on the host's OpenSSL) ----
 *
 * The subset of crypt32 that applications use to read a PE file's Authenticode
 * signature: CryptQueryObject on a signed PE, signer information
 * (CryptMsgGetParam), nested signatures (CryptMsgOpenToDecode/CryptMsgUpdate),
 * the embedded certificate store (CertOpenStore with CERT_STORE_PROV_PKCS7,
 * CertFindCertificateInStore by issuer and serial number), and RSA public-key
 * decoding (CryptDecodeObjectEx with CNG_RSA_PUBLIC_KEY_BLOB). Structures use the
 * Windows x64 layouts. Returned signer information is self-relative, as on
 * Windows. Everything is parsed from the file's own signature data; nothing is
 * synthesized. Unsupported parameters fail with the native error codes. */

#include <openssl/pkcs7.h>
#include <openssl/x509.h>

#define BEER_CRYPT_ENCODING        0x00010001u   /* X509_ASN_ENCODING | PKCS_7_ASN_ENCODING */
#define BEER_CRYPT_E_INVALID_MSG_TYPE 0x80091004u
#define BEER_CRYPT_E_NOT_FOUND     0x80092004u
#define BEER_CRYPT_E_NO_MATCH      0x80092009u
#define BEER_TRUST_E_NOSIGNATURE   0x800B0100u
#define BEER_NESTED_SIGNATURE_OID  "1.3.6.1.4.1.311.2.4.1"

typedef struct { u32 magic; PKCS7 *p7; u8 *pending; size_t pending_length; } BeerCryptMsg;
typedef struct { u32 magic; STACK_OF(X509) *certs; } BeerCertStore;
#define BEER_CRYPT_MSG_MAGIC   0x4753434du   /* "MCSG" */
#define BEER_CERT_STORE_MAGIC  0x53545243u   /* "CRTS" */

static BeerCryptMsg *crypt_msg(u64 handle)
{
    BeerCryptMsg *msg = (BeerCryptMsg *)(uintptr_t)handle;
    return msg && msg->magic == BEER_CRYPT_MSG_MAGIC ? msg : NULL;
}
static BeerCertStore *cert_store(u64 handle)
{
    BeerCertStore *store = (BeerCertStore *)(uintptr_t)handle;
    return store && store->magic == BEER_CERT_STORE_MAGIC ? store : NULL;
}

/* The PKCS#7 SignedData in a PE file's IMAGE_DIRECTORY_ENTRY_SECURITY (a file offset). */
static PKCS7 *crypt_pe_signature(const char *host_path)
{
    FILE *file = fopen(host_path, "rb");
    if (!file) return NULL;
    PKCS7 *p7 = NULL;
    u8 header[0x400];
    size_t got = fread(header, 1, sizeof(header), file);
    u32 lfanew = got >= 0x40 ? *(const u32 *)(header + 0x3c) : 0;
    if (lfanew + 24 + 0x98 <= got && !memcmp(header + lfanew, "PE\0\0", 4)) {
        const u8 *opt = header + lfanew + 24;
        u32 directory_offset = *(const u16 *)opt == 0x20b ? 112 + 8 * 4 : 96 + 8 * 4;
        u32 offset = *(const u32 *)(opt + directory_offset);
        u32 size = *(const u32 *)(opt + directory_offset + 4);
        u8 *table = offset && size >= 8 && size < (64u << 20) ? malloc(size) : NULL;
        if (table && fseek(file, offset, SEEK_SET) == 0 && fread(table, 1, size, file) == size) {
            u32 length = *(const u32 *)table;
            u16 type = *(const u16 *)(table + 6);
            if (type == 2 /* WIN_CERT_TYPE_PKCS_SIGNED_DATA */ && length > 8 && length <= size) {
                const unsigned char *der = table + 8;
                p7 = d2i_PKCS7(NULL, &der, (long)(length - 8));
            }
        }
        free(table);
    }
    fclose(file);
    if (p7 && !PKCS7_type_is_signed(p7)) { PKCS7_free(p7); p7 = NULL; }
    return p7;
}

static int crypt_host_path(const u16 *wide, char *host, size_t size)
{
    char narrow[4096];
    if (!wide) return 0;
    beer_utf16_to_utf8(wide, narrow, sizeof(narrow));
    return resolve_windows_host_path(narrow, host, size);
}

/* Number of nested (secondary) signatures attached to the primary signer. */
static u32 crypt_nested_signature_count(PKCS7 *p7)
{
    STACK_OF(PKCS7_SIGNER_INFO) *signers = p7 ? PKCS7_get_signer_info(p7) : NULL;
    PKCS7_SIGNER_INFO *signer = signers && sk_PKCS7_SIGNER_INFO_num(signers) > 0
                              ? sk_PKCS7_SIGNER_INFO_value(signers, 0) : NULL;
    u32 count = 0;
    for (int i = 0; signer && i < sk_X509_ATTRIBUTE_num(signer->unauth_attr); ++i) {
        X509_ATTRIBUTE *attribute = sk_X509_ATTRIBUTE_value(signer->unauth_attr, i);
        char oid[128];
        OBJ_obj2txt(oid, sizeof(oid), X509_ATTRIBUTE_get0_object(attribute), 1);
        if (!strcmp(oid, BEER_NESTED_SIGNATURE_OID)) count += (u32)X509_ATTRIBUTE_count(attribute);
    }
    return count;
}

/* ---- Self-relative output builder: pass 1 sizes (base NULL), pass 2 writes. ---- */
typedef struct { u8 *base; size_t used; } CryptOut;

static u64 crypt_out_bytes(CryptOut *out, const void *data, size_t length, size_t align)
{
    out->used = (out->used + align - 1) & ~(align - 1);
    u64 address = out->base ? (u64)(uintptr_t)(out->base + out->used) : 0;
    if (out->base && length) {
        if (data) memcpy(out->base + out->used, data, length);
        else memset(out->base + out->used, 0, length);   /* reserved space, filled by the caller */
    }
    out->used += length;
    return address;
}

static void crypt_put_blob(CryptOut *out, u8 *blob, const void *data, size_t length)
{
    u64 address = length ? crypt_out_bytes(out, data, length, 1) : 0;
    if (out->base) { *(u32 *)blob = (u32)length; *(u64 *)(blob + 8) = address; }
}

static void crypt_put_der(CryptOut *out, u8 *blob, unsigned char *der, int length)
{
    crypt_put_blob(out, blob, der, length > 0 ? (size_t)length : 0);
    OPENSSL_free(der);
}

static void crypt_put_string(CryptOut *out, u8 *slot, const char *text)
{
    u64 address = crypt_out_bytes(out, text, strlen(text) + 1, 1);
    if (out->base) *(u64 *)slot = address;
}

/* CRYPT_INTEGER_BLOB holds integers little-endian. */
static void crypt_put_integer(CryptOut *out, u8 *blob, const ASN1_INTEGER *value)
{
    int length = value ? ASN1_STRING_length(value) : 0;
    u8 reversed[256];
    if (length > (int)sizeof(reversed)) length = 0;
    for (int i = 0; i < length; ++i) reversed[i] = ASN1_STRING_get0_data(value)[length - 1 - i];
    crypt_put_blob(out, blob, reversed, (size_t)length);
}

/* CRYPT_ALGORITHM_IDENTIFIER: pszObjId, then the DER parameters blob. */
static void crypt_put_algorithm(CryptOut *out, u8 *algorithm, const X509_ALGOR *source)
{
    const ASN1_OBJECT *object = NULL;
    int type = V_ASN1_UNDEF;
    const void *value = NULL;
    X509_ALGOR_get0(&object, &type, &value, source);
    char oid[128] = "";
    if (object) OBJ_obj2txt(oid, sizeof(oid), object, 1);
    crypt_put_string(out, algorithm, oid);
    unsigned char *der = NULL;
    int length = 0;
    if (type != V_ASN1_UNDEF) {
        ASN1_TYPE *parameters = ASN1_TYPE_new();
        if (parameters && ASN1_TYPE_set1(parameters, type, value)) length = i2d_ASN1_TYPE(parameters, &der);
        ASN1_TYPE_free(parameters);
    }
    crypt_put_der(out, algorithm + 8, der, length);
}

/* CRYPT_ATTRIBUTES { cAttr; rgAttr } with CRYPT_ATTRIBUTE { pszObjId; cValue; rgValue }. */
static void crypt_put_attributes(CryptOut *out, u8 *attributes, STACK_OF(X509_ATTRIBUTE) *source)
{
    int count = source ? sk_X509_ATTRIBUTE_num(source) : 0;
    u64 array = crypt_out_bytes(out, NULL, (size_t)count * 24, 8);
    if (out->base) { *(u32 *)attributes = (u32)count; *(u64 *)(attributes + 8) = count ? array : 0; }
    for (int i = 0; i < count; ++i) {
        X509_ATTRIBUTE *attribute = sk_X509_ATTRIBUTE_value(source, i);
        u8 *entry = out->base ? (u8 *)(uintptr_t)array + (size_t)i * 24 : NULL;
        u8 scratch[24];
        if (!entry) entry = scratch;
        char oid[128];
        OBJ_obj2txt(oid, sizeof(oid), X509_ATTRIBUTE_get0_object(attribute), 1);
        crypt_put_string(out, entry, oid);
        int values = X509_ATTRIBUTE_count(attribute);
        u64 blobs = crypt_out_bytes(out, NULL, (size_t)values * 16, 8);
        if (out->base) { *(u32 *)(entry + 8) = (u32)values; *(u64 *)(entry + 16) = values ? blobs : 0; }
        for (int v = 0; v < values; ++v) {
            unsigned char *der = NULL;
            int length = i2d_ASN1_TYPE(X509_ATTRIBUTE_get0_type(attribute, v), &der);
            u8 blob_scratch[16];
            u8 *blob = out->base ? (u8 *)(uintptr_t)blobs + (size_t)v * 16 : blob_scratch;
            crypt_put_der(out, blob, der, length);
        }
    }
}

/* CMSG_SIGNER_INFO (Windows x64): dwVersion@0 Issuer@8 SerialNumber@0x18 HashAlgorithm@0x28
 * HashEncryptionAlgorithm@0x40 EncryptedHash@0x58 AuthAttrs@0x68 UnauthAttrs@0x78, size 0x88. */
static size_t crypt_signer_info(u8 *base, PKCS7_SIGNER_INFO *signer)
{
    CryptOut out = { base, 0 };
    u8 scratch[0x88];
    u8 *info = base ? base : scratch;
    crypt_out_bytes(&out, NULL, 0x88, 8);
    if (base) memset(base, 0, 0x88);
    if (base) *(u32 *)info = (u32)ASN1_INTEGER_get(signer->version);
    unsigned char *der = NULL;
    int length = i2d_X509_NAME(signer->issuer_and_serial->issuer, &der);
    crypt_put_der(&out, info + 0x08, der, length);
    crypt_put_integer(&out, info + 0x18, signer->issuer_and_serial->serial);
    crypt_put_algorithm(&out, info + 0x28, signer->digest_alg);
    crypt_put_algorithm(&out, info + 0x40, signer->digest_enc_alg);
    crypt_put_blob(&out, info + 0x58, ASN1_STRING_get0_data(signer->enc_digest),
                   (size_t)ASN1_STRING_length(signer->enc_digest));
    crypt_put_attributes(&out, info + 0x68, signer->auth_attr);
    crypt_put_attributes(&out, info + 0x78, signer->unauth_attr);
    return out.used;
}

static u64 crypt_return_sized(void *data, u32 *data_length, size_t needed, int (*fill)(void *, void *), void *opaque)
{
    if (!data_length) { g_last_error = 87; return 0; }
    if (!data) { *data_length = (u32)needed; return 1; }
    if (*data_length < needed) { *data_length = (u32)needed; g_last_error = 234; return 0; }  /* ERROR_MORE_DATA */
    *data_length = (u32)needed;
    return (u64)fill(data, opaque);
}

static int crypt_fill_signer(void *data, void *signer) { crypt_signer_info(data, signer); return 1; }
static int crypt_fill_attributes(void *data, void *source)
{
    CryptOut out = { data, 0 };
    crypt_out_bytes(&out, NULL, 16, 8);
    crypt_put_attributes(&out, data, source);
    return 1;
}

static u64 __attribute__((ms_abi))
impl_CryptMsgGetParam(u64 handle, u32 parameter, u32 index, void *data, u32 *data_length)
{
    BeerCryptMsg *msg = crypt_msg(handle);
    if (!msg || !msg->p7) { g_last_error = 6; return 0; }
    STACK_OF(PKCS7_SIGNER_INFO) *signers = PKCS7_get_signer_info(msg->p7);
    int count = signers ? sk_PKCS7_SIGNER_INFO_num(signers) : 0;
    if (parameter == 5) {                       /* CMSG_SIGNER_COUNT_PARAM */
        u32 value = (u32)count;
        if (!data_length) { g_last_error = 87; return 0; }
        if (data && *data_length < 4) { *data_length = 4; g_last_error = 234; return 0; }
        if (data) memcpy(data, &value, 4);
        *data_length = 4;
        return 1;
    }
    if (parameter != 6 && parameter != 9 && parameter != 10) {
        fprintf(stderr, "[CRYPT32] CryptMsgGetParam parameter %u is not implemented\n", parameter);
        g_last_error = BEER_CRYPT_E_INVALID_MSG_TYPE;
        return 0;
    }
    if ((int)index >= count) { g_last_error = 0x80091008u; return 0; }   /* CRYPT_E_INVALID_INDEX */
    PKCS7_SIGNER_INFO *signer = sk_PKCS7_SIGNER_INFO_value(signers, (int)index);
    if (parameter == 6)                         /* CMSG_SIGNER_INFO_PARAM */
        return crypt_return_sized(data, data_length, crypt_signer_info(NULL, signer), crypt_fill_signer, signer);
    STACK_OF(X509_ATTRIBUTE) *attributes = parameter == 9 ? signer->auth_attr : signer->unauth_attr;
    if (!attributes || !sk_X509_ATTRIBUTE_num(attributes)) { g_last_error = 0x8009100Fu; return 0; }  /* CRYPT_E_ATTRIBUTES_MISSING */
    CryptOut size = { NULL, 0 };
    crypt_out_bytes(&size, NULL, 16, 8);
    crypt_put_attributes(&size, (u8[16]){0}, attributes);
    return crypt_return_sized(data, data_length, size.used, crypt_fill_attributes, attributes);
}

static u64 __attribute__((ms_abi))
impl_CryptMsgOpenToDecode(u32 encoding, u32 flags, u32 type, u64 crypt_provider, void *recipient, void *stream)
{
    (void)flags; (void)type; (void)crypt_provider; (void)recipient;
    if (!(encoding & 0x00010000u) || stream) { g_last_error = BEER_CRYPT_E_INVALID_MSG_TYPE; return 0; }
    BeerCryptMsg *msg = calloc(1, sizeof(*msg));
    if (!msg) { g_last_error = 8; return 0; }
    msg->magic = BEER_CRYPT_MSG_MAGIC;
    return (u64)(uintptr_t)msg;
}

static u64 __attribute__((ms_abi))
impl_CryptMsgUpdate(u64 handle, const u8 *data, u32 length, u32 final)
{
    BeerCryptMsg *msg = crypt_msg(handle);
    if (!msg || msg->p7) { g_last_error = 6; return 0; }
    u8 *grown = realloc(msg->pending, msg->pending_length + length);
    if (!grown && length) { g_last_error = 8; return 0; }
    msg->pending = grown;
    if (length) memcpy(msg->pending + msg->pending_length, data, length);
    msg->pending_length += length;
    if (!final) return 1;
    const unsigned char *der = msg->pending;
    msg->p7 = d2i_PKCS7(NULL, &der, (long)msg->pending_length);
    if (!msg->p7 || !PKCS7_type_is_signed(msg->p7)) {
        PKCS7_free(msg->p7);
        msg->p7 = NULL;
        g_last_error = 0x80093102u;             /* CRYPT_E_ASN1_BADTAG */
        return 0;
    }
    return 1;
}

static u64 __attribute__((ms_abi)) impl_CryptMsgClose(u64 handle)
{
    BeerCryptMsg *msg = crypt_msg(handle);
    if (!msg) return 1;
    msg->magic = 0;
    PKCS7_free(msg->p7);
    free(msg->pending);
    free(msg);
    return 1;
}

static BeerCertStore *cert_store_from_p7(PKCS7 *p7)
{
    BeerCertStore *store = calloc(1, sizeof(*store));
    if (!store) return NULL;
    store->magic = BEER_CERT_STORE_MAGIC;
    store->certs = sk_X509_new_null();
    STACK_OF(X509) *certs = p7 && PKCS7_type_is_signed(p7) ? p7->d.sign->cert : NULL;
    for (int i = 0; certs && i < sk_X509_num(certs); ++i) {
        X509 *cert = sk_X509_value(certs, i);
        if (X509_up_ref(cert)) sk_X509_push(store->certs, cert);
    }
    return store;
}

/* CertOpenStore: CERT_STORE_PROV_PKCS7 (5) and CERT_STORE_PROV_MSG (1) over a decoded message. */
static u64 __attribute__((ms_abi))
impl_CertOpenStore(u64 provider, u32 encoding, u64 crypt_provider, u32 flags, const void *parameter)
{
    (void)encoding; (void)crypt_provider; (void)flags;
    PKCS7 *p7 = NULL;
    if (provider == 5 && parameter) {
        const u8 *blob = parameter;
        const unsigned char *der = (const unsigned char *)(uintptr_t)*(const u64 *)(blob + 8);
        p7 = der ? d2i_PKCS7(NULL, &der, (long)*(const u32 *)blob) : NULL;
        if (!p7) { g_last_error = 0x80093102u; return 0; }
    } else if (provider == 1 && crypt_msg((u64)(uintptr_t)parameter)) {
        PKCS7 *source = crypt_msg((u64)(uintptr_t)parameter)->p7;
        p7 = source ? PKCS7_dup(source) : NULL;
    } else {
        fprintf(stderr, "[CRYPT32] CertOpenStore provider %llu is not implemented\n", (unsigned long long)provider);
        g_last_error = 2;                       /* no provider function for this store type */
        return 0;
    }
    BeerCertStore *store = cert_store_from_p7(p7);
    PKCS7_free(p7);
    if (!store) { g_last_error = 8; return 0; }
    return (u64)(uintptr_t)store;
}

static u64 __attribute__((ms_abi)) impl_CertCloseStore(u64 handle, u32 flags)
{
    (void)flags;
    BeerCertStore *store = cert_store(handle);
    if (!store) return 1;
    store->magic = 0;
    sk_X509_pop_free(store->certs, X509_free);
    free(store);
    return 1;
}

static u64 crypt_filetime(const ASN1_TIME *time)
{
    struct tm tm;
    if (!time || !ASN1_TIME_to_tm(time, &tm)) return 0;
    return (u64)((s64)timegm(&tm) + 11644473600ll) * 10000000ull;
}

/* CERT_CONTEXT { dwCertEncodingType@0 pbCertEncoded@8 cbCertEncoded@0x10 pCertInfo@0x18 hCertStore@0x20 }
 * followed by CERT_INFO (0xd0 bytes) and its data, in one allocation. */
static size_t crypt_cert_context(u8 *base, X509 *cert, u64 store)
{
    CryptOut out = { base, 0 };
    u64 context = crypt_out_bytes(&out, NULL, 0x28, 8);
    u64 info_address = crypt_out_bytes(&out, NULL, 0xd0, 8);
    u8 scratch[0xd0];
    u8 *info = base ? (u8 *)(uintptr_t)info_address : scratch;
    if (base) memset(base, 0, 0x28 + 0xd0);
    unsigned char *der = NULL;
    int length = i2d_X509(cert, &der);
    u64 encoded = crypt_out_bytes(&out, der, length > 0 ? (size_t)length : 0, 1);
    OPENSSL_free(der);
    if (base) {
        u8 *ctx = (u8 *)(uintptr_t)context;
        *(u32 *)ctx = BEER_CRYPT_ENCODING;
        *(u64 *)(ctx + 0x08) = encoded;
        *(u32 *)(ctx + 0x10) = (u32)(length > 0 ? length : 0);
        *(u64 *)(ctx + 0x18) = info_address;
        *(u64 *)(ctx + 0x20) = store;
        *(u32 *)info = (u32)X509_get_version(cert);
        *(u64 *)(info + 0x40) = crypt_filetime(X509_get0_notBefore(cert));
        *(u64 *)(info + 0x48) = crypt_filetime(X509_get0_notAfter(cert));
    }
    crypt_put_integer(&out, info + 0x08, X509_get0_serialNumber(cert));
    crypt_put_algorithm(&out, info + 0x18, X509_get0_tbs_sigalg(cert));
    der = NULL; length = i2d_X509_NAME(X509_get_issuer_name(cert), &der);
    crypt_put_der(&out, info + 0x30, der, length);
    der = NULL; length = i2d_X509_NAME(X509_get_subject_name(cert), &der);
    crypt_put_der(&out, info + 0x50, der, length);
    ASN1_OBJECT *key_object = NULL;
    const unsigned char *key = NULL;
    int key_length = 0;
    X509_ALGOR *key_algorithm = NULL;
    X509_PUBKEY_get0_param(&key_object, &key, &key_length, &key_algorithm, X509_get_X509_PUBKEY(cert));
    if (key_algorithm) crypt_put_algorithm(&out, info + 0x60, key_algorithm);
    crypt_put_blob(&out, info + 0x78, key, key_length > 0 ? (size_t)key_length : 0);   /* CRYPT_BIT_BLOB, cUnusedBits 0 */
    return out.used;
}

/* CertFindCertificateInStore: CERT_FIND_ANY (0) and CERT_FIND_SUBJECT_CERT (0xb0000, issuer + serial). */
static u64 __attribute__((ms_abi))
impl_CertFindCertificateInStore(u64 handle, u32 encoding, u32 flags, u32 type, const void *parameter, const u8 *previous)
{
    (void)encoding; (void)flags;
    BeerCertStore *store = cert_store(handle);
    if (!store) { g_last_error = 6; return 0; }
    if (type != 0 && type != 0xb0000u) {
        fprintf(stderr, "[CRYPT32] CertFindCertificateInStore type 0x%x is not implemented\n", type);
        g_last_error = BEER_CRYPT_E_NOT_FOUND;
        return 0;
    }
    /* Each returned context is preceded by a private 16-byte header holding its store index, so
     * enumeration can continue from pPrevCertContext (which the call frees, as on Windows). */
    int start = 0;
    if (previous) {
        start = (int)*(const u32 *)(previous - 16) + 1;
        free((void *)(previous - 16));
    }
    for (int i = start; i < sk_X509_num(store->certs); ++i) {
        X509 *cert = sk_X509_value(store->certs, i);
        if (type == 0xb0000u) {
            const u8 *wanted = parameter;
            u32 issuer_length = *(const u32 *)(wanted + 0x30);
            const u8 *issuer = (const u8 *)(uintptr_t)*(const u64 *)(wanted + 0x38);
            u32 serial_length = *(const u32 *)(wanted + 0x08);
            const u8 *serial = (const u8 *)(uintptr_t)*(const u64 *)(wanted + 0x10);
            unsigned char *der = NULL;
            int length = i2d_X509_NAME(X509_get_issuer_name(cert), &der);
            int issuer_match = length == (int)issuer_length && issuer && !memcmp(der, issuer, issuer_length);
            OPENSSL_free(der);
            const ASN1_INTEGER *own = X509_get0_serialNumber(cert);
            int own_length = ASN1_STRING_length(own);
            int serial_match = own_length == (int)serial_length && serial;
            for (int b = 0; serial_match && b < own_length; ++b)
                serial_match = ASN1_STRING_get0_data(own)[own_length - 1 - b] == serial[b];
            if (!issuer_match || !serial_match) continue;
        }
        size_t size = crypt_cert_context(NULL, cert, handle);
        u8 *allocation = calloc(1, 16 + size);
        if (!allocation) { g_last_error = 8; return 0; }
        *(u32 *)allocation = (u32)i;
        crypt_cert_context(allocation + 16, cert, handle);
        return (u64)(uintptr_t)(allocation + 16);
    }
    g_last_error = BEER_CRYPT_E_NOT_FOUND;
    return 0;
}

static u64 __attribute__((ms_abi)) impl_CertFreeCertificateContext(void *context)
{
    if (context) free((u8 *)context - 16);
    return 1;
}

/* A DER length/tag reader for RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }. */
static const u8 *crypt_der_item(const u8 *at, const u8 *end, u8 tag, size_t *length)
{
    if (at + 2 > end || at[0] != tag) return NULL;
    size_t size = at[1];
    at += 2;
    if (size & 0x80) {
        int bytes = (int)(size & 0x7f);
        if (bytes < 1 || bytes > 4 || at + bytes > end) return NULL;
        size = 0;
        while (bytes--) size = (size << 8) | *at++;
    }
    if (at + size > end) return NULL;
    *length = size;
    return at;
}

/* CryptDecodeObjectEx: CNG_RSA_PUBLIC_KEY_BLOB (72) produces a BCRYPT_RSAKEY_BLOB
 * { Magic "RSA1", BitLength, cbPublicExp, cbModulus, cbPrime1, cbPrime2 } + exponent + modulus. */
static u64 __attribute__((ms_abi))
impl_CryptDecodeObjectEx(u32 encoding, u64 struct_type, const u8 *encoded, u32 encoded_length, u32 flags,
                         const void *decode_para, void *output, u32 *output_length)
{
    (void)encoding; (void)decode_para;
    if (struct_type != 72) {
        if (struct_type < 0x10000) fprintf(stderr, "[CRYPT32] CryptDecodeObjectEx type %llu is not implemented\n",
                                           (unsigned long long)struct_type);
        else fprintf(stderr, "[CRYPT32] CryptDecodeObjectEx type \"%s\" is not implemented\n",
                     (const char *)(uintptr_t)struct_type);
        g_last_error = 2;                       /* no decode function for this structure type */
        return 0;
    }
    size_t length;
    const u8 *end = encoded + encoded_length;
    const u8 *sequence = encoded ? crypt_der_item(encoded, end, 0x30, &length) : NULL;
    size_t modulus_length = 0, exponent_length = 0;
    const u8 *modulus = sequence ? crypt_der_item(sequence, sequence + length, 0x02, &modulus_length) : NULL;
    const u8 *exponent = modulus ? crypt_der_item(modulus + modulus_length, sequence + length, 0x02, &exponent_length) : NULL;
    if (!exponent) { g_last_error = 0x80093102u; return 0; }  /* CRYPT_E_ASN1_BADTAG */
    while (modulus_length > 1 && !modulus[0]) { ++modulus; --modulus_length; }
    while (exponent_length > 1 && !exponent[0]) { ++exponent; --exponent_length; }
    size_t needed = 24 + exponent_length + modulus_length;
    u8 *target = output;
    if (flags & 0x8000u) {                      /* CRYPT_DECODE_ALLOC_FLAG: LocalAlloc'd result */
        target = (u8 *)(uintptr_t)impl_LocalAlloc(0, needed);
        if (!target) { g_last_error = 8; return 0; }
        *(u8 **)output = target;
    } else {
        if (!output_length) { g_last_error = 87; return 0; }
        if (!output) { *output_length = (u32)needed; return 1; }
        if (*output_length < needed) { *output_length = (u32)needed; g_last_error = 234; return 0; }
    }
    if (output_length) *output_length = (u32)needed;
    u32 header[6] = { 0x31415352u, (u32)(modulus_length * 8), (u32)exponent_length, (u32)modulus_length, 0, 0 };
    memcpy(target, header, sizeof(header));
    memcpy(target + 24, exponent, exponent_length);
    memcpy(target + 24 + exponent_length, modulus, modulus_length);
    return 1;
}

/* CryptQueryObject: CERT_QUERY_OBJECT_FILE for an Authenticode-signed PE (PKCS7_SIGNED_EMBED). */
static u64 __attribute__((ms_abi))
impl_CryptQueryObject(u32 object_type, const void *object, u32 expected_content, u32 expected_format, u32 flags,
                      u32 *encoding, u32 *content_type, u32 *format_type, u64 *store, u64 *msg, void **context)
{
    (void)expected_format; (void)flags;
    if (context) *context = NULL;
    char host[4096];
    if (object_type != 1 || !(expected_content & 0x400u) || !crypt_host_path(object, host, sizeof(host))) {
        g_last_error = BEER_CRYPT_E_NO_MATCH;
        return 0;
    }
    PKCS7 *p7 = crypt_pe_signature(host);
    if (!p7) { g_last_error = BEER_CRYPT_E_NO_MATCH; return 0; }
    if (encoding) *encoding = BEER_CRYPT_ENCODING;
    if (content_type) *content_type = 10;       /* CERT_QUERY_CONTENT_PKCS7_SIGNED_EMBED */
    if (format_type) *format_type = 1;          /* CERT_QUERY_FORMAT_BINARY */
    if (store) *store = (u64)(uintptr_t)cert_store_from_p7(p7);
    if (msg) {
        BeerCryptMsg *message = calloc(1, sizeof(*message));
        if (message) { message->magic = BEER_CRYPT_MSG_MAGIC; message->p7 = p7; p7 = NULL; }
        *msg = (u64)(uintptr_t)message;
    }
    PKCS7_free(p7);
    return 1;
}

/* WINTRUST_SIGNATURE_SETTINGS (cbStruct@0 dwIndex@4 dwFlags@8 cSecondarySigs@0xc dwVerifiedSigIndex@0x10)
 * for a WTD_CHOICE_FILE request: report the file's nested-signature count and the verified index. */
static void crypt_wintrust_signature_settings(const u8 *trust_data)
{
    if (*(const u32 *)trust_data < 0x58) return;
    u8 *settings = (u8 *)(uintptr_t)*(const u64 *)(trust_data + 0x50);
    if (!settings || *(const u32 *)settings < 0x14) return;
    u32 flags = *(const u32 *)(settings + 8);
    PKCS7 *p7 = NULL;
    if (*(const u32 *)(trust_data + 0x20) == 1) {                /* WTD_CHOICE_FILE */
        const u8 *file = (const u8 *)(uintptr_t)*(const u64 *)(trust_data + 0x28);
        char host[4096];
        if (file && crypt_host_path((const u16 *)(uintptr_t)*(const u64 *)(file + 8), host, sizeof(host)))
            p7 = crypt_pe_signature(host);
    }
    u32 secondary = crypt_nested_signature_count(p7);
    PKCS7_free(p7);
    if (flags & 0x2u) *(u32 *)(settings + 0x0c) = secondary;    /* WSS_GET_SECONDARY_SIG_COUNT */
    *(u32 *)(settings + 0x10) = (flags & 0x1u) ? *(const u32 *)(settings + 4) : 0;   /* WSS_VERIFY_SPECIFIC */
}
