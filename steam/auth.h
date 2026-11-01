/* Steam Authentication service flows: QR code, account name + password, Steam Guard, polling. */
#ifndef BEER_STEAM_AUTH_H
#define BEER_STEAM_AUTH_H

#include <stddef.h>
#include <stdint.h>

#include "cm.h"

enum {
    GUARD_NONE = 1,
    GUARD_EMAIL_CODE = 2,
    GUARD_DEVICE_CODE = 3,
    GUARD_DEVICE_CONFIRMATION = 4,
    GUARD_EMAIL_CONFIRMATION = 5,
    GUARD_MACHINE_TOKEN = 6,
};

typedef struct {
    uint64_t client_id;
    uint8_t request_id[128];
    size_t request_id_len;
    uint64_t steamid;            /* known for password sessions */
    int interval_ms;             /* how often Steam wants to be polled */
    char *challenge_url;         /* QR sessions: the text to encode */
    int guards[8];               /* allowed confirmations, in Steam's preferred order */
    int guard_count;
    char *guard_hint;            /* e.g. the email domain for an emailed code */
} AuthSession;

typedef struct {
    char *refresh_token;
    char *account_name;
    char *guard_data;            /* remembers this device for future emailed-code checks */
} AuthTokens;

/* Return an EResult (ERESULT_OK on success). */
int auth_begin_qr(Cm *cm, AuthSession *session);
int auth_begin_password(Cm *cm, const char *account, const char *password, const char *guard_data,
                        AuthSession *session);
int auth_submit_code(Cm *cm, const AuthSession *session, const char *code, int guard_type);

/* Polls once. Returns 1 with tokens when approved, 0 while pending (session->challenge_url may
 * have been replaced; *url_changed is then set), or -1 with *eresult when the session failed. */
int auth_poll(Cm *cm, AuthSession *session, AuthTokens *tokens, int *url_changed, int *eresult);

void auth_session_free(AuthSession *session);
void auth_tokens_free(AuthTokens *tokens);

#endif
