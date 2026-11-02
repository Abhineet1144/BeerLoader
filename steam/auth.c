#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>

#define CALL_TIMEOUT_MS 20000
#define PLATFORM_STEAM_CLIENT 1
#define EOS_LINUX_UNKNOWN (-203)

static void device_details(PbBuf *out, char *name, size_t name_len)
{
    char host[200] = "Linux";
    gethostname(host, sizeof(host) - 1);
    snprintf(name, name_len, "%s (Beer)", host);
    pb_string(out, 1, name);
    pb_u64(out, 2, PLATFORM_STEAM_CLIENT);
    pb_i32(out, 3, EOS_LINUX_UNKNOWN);
}

static int float_ms(uint64_t bits)
{
    float seconds;
    uint32_t raw = (uint32_t)bits;
    memcpy(&seconds, &raw, 4);
    return seconds > 0.f && seconds < 60.f ? (int)(seconds * 1000.f) : 5000;
}

static void read_confirmation(AuthSession *s, const uint8_t *data, size_t len)
{
    PbField f;
    if (s->guard_count >= 8) return;
    int type = pb_find(data, len, 1, &f) ? (int)f.value : 0;
    s->guards[s->guard_count++] = type;
    if (!s->guard_hint && pb_find(data, len, 2, &f) && f.len) s->guard_hint = pb_strdup(&f);
}

int auth_begin_qr(Cm *cm, AuthSession *s)
{
    memset(s, 0, sizeof(*s));
    char name[256];
    PbBuf details = {0}, request = {0}, response;
    device_details(&details, name, sizeof(name));
    pb_string(&request, 1, name);
    pb_u64(&request, 2, PLATFORM_STEAM_CLIENT);
    pb_msg(&request, 3, &details);
    pb_string(&request, 4, "Client");
    int eresult;
    cm_call(cm, "Authentication.BeginAuthSessionViaQR#1", &request, 0, CALL_TIMEOUT_MS, &response, &eresult);
    pb_free(&details);
    pb_free(&request);
    if (eresult == ERESULT_OK) {
        PbReader r = { response.data, response.data + response.len };
        PbField f;
        while (pb_next(&r, &f) == 1) {
            if (f.field == 1) s->client_id = f.value;
            else if (f.field == 2) s->challenge_url = pb_strdup(&f);
            else if (f.field == 3 && f.len <= sizeof(s->request_id)) {
                memcpy(s->request_id, f.data, f.len);
                s->request_id_len = f.len;
            } else if (f.field == 4) s->interval_ms = float_ms(f.value);
            else if (f.field == 5) read_confirmation(s, f.data, f.len);
        }
        if (!s->client_id || !s->challenge_url || !s->request_id_len) eresult = ERESULT_FAIL;
    }
    pb_free(&response);
    return eresult;
}

/* RSA PKCS#1 v1.5 with Steam's per-account key, then base64, as the service requires. */
static char *encrypt_password(const char *password, const char *mod_hex, const char *exp_hex)
{
    BIGNUM *n = NULL, *e = NULL;
    OSSL_PARAM_BLD *build = NULL;
    OSSL_PARAM *params = NULL;
    EVP_PKEY_CTX *ctx = NULL, *enc = NULL;
    EVP_PKEY *key = NULL;
    unsigned char *cipher = NULL;
    char *out = NULL;
    size_t cipher_len = 0;
    if (!BN_hex2bn(&n, mod_hex) || !BN_hex2bn(&e, exp_hex)) goto done;
    if (!(build = OSSL_PARAM_BLD_new()) || !OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_N, n) ||
        !OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_E, e) || !(params = OSSL_PARAM_BLD_to_param(build)))
        goto done;
    if (!(ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL)) || EVP_PKEY_fromdata_init(ctx) <= 0 ||
        EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) <= 0)
        goto done;
    if (!(enc = EVP_PKEY_CTX_new(key, NULL)) || EVP_PKEY_encrypt_init(enc) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(enc, RSA_PKCS1_PADDING) <= 0 ||
        EVP_PKEY_encrypt(enc, NULL, &cipher_len, (const unsigned char *)password, strlen(password)) <= 0 ||
        !(cipher = malloc(cipher_len)) ||
        EVP_PKEY_encrypt(enc, cipher, &cipher_len, (const unsigned char *)password, strlen(password)) <= 0)
        goto done;
    if ((out = malloc(4 * ((cipher_len + 2) / 3) + 1)))
        EVP_EncodeBlock((unsigned char *)out, cipher, (int)cipher_len);
done:
    free(cipher);
    EVP_PKEY_CTX_free(enc);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(build);
    BN_free(n);
    BN_free(e);
    return out;
}

int auth_begin_password(Cm *cm, const char *account, const char *password, const char *guard_data,
                        AuthSession *s)
{
    memset(s, 0, sizeof(*s));
    PbBuf request = {0}, response;
    int eresult;
    pb_string(&request, 1, account);
    cm_call(cm, "Authentication.GetPasswordRSAPublicKey#1", &request, 0, CALL_TIMEOUT_MS, &response, &eresult);
    pb_free(&request);
    char *mod = NULL, *exp = NULL;
    uint64_t timestamp = 0;
    if (eresult == ERESULT_OK) {
        PbField f;
        if (pb_find(response.data, response.len, 1, &f)) mod = pb_strdup(&f);
        if (pb_find(response.data, response.len, 2, &f)) exp = pb_strdup(&f);
        if (pb_find(response.data, response.len, 3, &f)) timestamp = f.value;
    }
    pb_free(&response);
    char *encrypted = mod && exp ? encrypt_password(password, mod, exp) : NULL;
    free(mod);
    free(exp);
    if (!encrypted) return eresult == ERESULT_OK ? ERESULT_FAIL : eresult;

    char name[256];
    PbBuf details = {0};
    device_details(&details, name, sizeof(name));
    pb_string(&request, 1, name);
    pb_string(&request, 2, account);
    pb_string(&request, 3, encrypted);
    pb_u64(&request, 4, timestamp);
    pb_u64(&request, 5, 1);                       /* remember_login */
    pb_u64(&request, 6, PLATFORM_STEAM_CLIENT);
    pb_u64(&request, 7, 1);                       /* k_ESessionPersistence_Persistent */
    pb_string(&request, 8, "Client");
    pb_msg(&request, 9, &details);
    if (guard_data && *guard_data) pb_string(&request, 10, guard_data);
    cm_call(cm, "Authentication.BeginAuthSessionViaCredentials#1", &request, 0, CALL_TIMEOUT_MS, &response, &eresult);
    memset(encrypted, 0, strlen(encrypted));
    free(encrypted);
    if (request.data) memset(request.data, 0, request.len);
    pb_free(&request);
    pb_free(&details);
    if (eresult == ERESULT_OK) {
        PbReader r = { response.data, response.data + response.len };
        PbField f;
        while (pb_next(&r, &f) == 1) {
            if (f.field == 1) s->client_id = f.value;
            else if (f.field == 2 && f.len <= sizeof(s->request_id)) {
                memcpy(s->request_id, f.data, f.len);
                s->request_id_len = f.len;
            } else if (f.field == 3) s->interval_ms = float_ms(f.value);
            else if (f.field == 4) read_confirmation(s, f.data, f.len);
            else if (f.field == 5) s->steamid = f.value;
        }
        if (!s->client_id || !s->request_id_len) eresult = ERESULT_FAIL;
    }
    pb_free(&response);
    return eresult;
}

int auth_submit_code(Cm *cm, const AuthSession *s, const char *code, int guard_type)
{
    PbBuf request = {0}, response;
    pb_u64(&request, 1, s->client_id);
    pb_fixed64(&request, 2, s->steamid);
    pb_string(&request, 3, code);
    pb_u64(&request, 4, (uint64_t)guard_type);
    int eresult;
    cm_call(cm, "Authentication.UpdateAuthSessionWithSteamGuardCode#1", &request, 0, CALL_TIMEOUT_MS,
            &response, &eresult);
    pb_free(&request);
    pb_free(&response);
    return eresult;
}

int auth_poll(Cm *cm, AuthSession *s, AuthTokens *tokens, int *url_changed, int *eresult)
{
    memset(tokens, 0, sizeof(*tokens));
    *url_changed = 0;
    PbBuf request = {0}, response;
    pb_u64(&request, 1, s->client_id);
    pb_bytes(&request, 2, s->request_id, s->request_id_len);
    cm_call(cm, "Authentication.PollAuthSessionStatus#1", &request, 0, CALL_TIMEOUT_MS, &response, eresult);
    pb_free(&request);
    if (*eresult != ERESULT_OK) { pb_free(&response); return -1; }
    PbReader r = { response.data, response.data + response.len };
    PbField f;
    while (pb_next(&r, &f) == 1) {
        if (f.field == 1 && f.value) s->client_id = f.value;
        else if (f.field == 2 && f.len) {
            free(s->challenge_url);
            s->challenge_url = pb_strdup(&f);
            *url_changed = 1;
        } else if (f.field == 3 && f.len) tokens->refresh_token = pb_strdup(&f);
        else if (f.field == 6 && f.len) tokens->account_name = pb_strdup(&f);
        else if (f.field == 7 && f.len) tokens->guard_data = pb_strdup(&f);
    }
    pb_free(&response);
    if (tokens->refresh_token && tokens->account_name) return 1;
    auth_tokens_free(tokens);
    return 0;
}

void auth_session_free(AuthSession *s)
{
    free(s->challenge_url);
    free(s->guard_hint);
    memset(s, 0, sizeof(*s));
}

static void free_secret(char *p)
{
    if (p) { memset(p, 0, strlen(p)); free(p); }
}

void auth_tokens_free(AuthTokens *t)
{
    free_secret(t->refresh_token);
    free(t->account_name);
    free_secret(t->guard_data);
    memset(t, 0, sizeof(*t));
}
