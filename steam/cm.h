/* Steam connection-manager session over WebSocket: packet framing, service calls, logon, heartbeat. */
#ifndef BEER_STEAM_CM_H
#define BEER_STEAM_CM_H

#include <stddef.h>
#include <stdint.h>

#include "pb.h"

enum {
    EMSG_MULTI = 1,
    EMSG_SERVICE_METHOD_RESPONSE = 147,
    EMSG_SERVICE_METHOD_CALL = 151,
    EMSG_CLIENT_HEARTBEAT = 703,
    EMSG_CLIENT_LOGOFF = 706,
    EMSG_CLIENT_LOGON_RESPONSE = 751,
    EMSG_CLIENT_LOGGED_OFF = 757,
    EMSG_CLIENT_ACCOUNT_INFO = 768,
    EMSG_CLIENT_GAME_CONNECT_TOKENS = 779,
    EMSG_CLIENT_LICENSE_LIST = 780,
    EMSG_CLIENT_GET_APP_OWNERSHIP_TICKET = 857,
    EMSG_CLIENT_AUTH_LIST = 5432,
    EMSG_CLIENT_GET_DEPOT_DECRYPTION_KEY = 5438,
    EMSG_CLIENT_CHECK_APP_BETA_PASSWORD = 5450,
    EMSG_CLIENT_LOGON = 5514,
    EMSG_CLIENT_REQUEST_ENCRYPTED_APP_TICKET = 5526,
    EMSG_CLIENT_PICS_PRODUCT_INFO_REQUEST = 8903,
    EMSG_CLIENT_PICS_PRODUCT_INFO_RESPONSE = 8904,
    EMSG_CLIENT_PICS_ACCESS_TOKEN_REQUEST = 8905,
    EMSG_CLIENT_PICS_ACCESS_TOKEN_RESPONSE = 8906,
    EMSG_SERVICE_METHOD_CALL_NONAUTHED = 9804,
    EMSG_CLIENT_HELLO = 9805,
};

enum {
    ERESULT_OK = 1,
    ERESULT_FAIL = 2,
    ERESULT_NO_CONNECTION = 3,
    ERESULT_INVALID_PASSWORD = 5,
    ERESULT_ACCESS_DENIED = 15,
    ERESULT_TIMEOUT = 16,
    ERESULT_SERVICE_UNAVAILABLE = 20,
    ERESULT_REVOKED = 26,
    ERESULT_EXPIRED = 27,
    ERESULT_TRY_ANOTHER_CM = 48,
    ERESULT_ACCOUNT_LOGON_DENIED = 63,
    ERESULT_INVALID_LOGIN_AUTH_CODE = 65,
    ERESULT_RATE_LIMIT_EXCEEDED = 84,
    ERESULT_ACCOUNT_LOGIN_DENIED_THROTTLE = 87,
    ERESULT_TWO_FACTOR_CODE_MISMATCH = 88,
};

typedef struct Cm Cm;

/* Called on the I/O thread for messages nobody is waiting for. emsg 0 means the connection ended. */
typedef void (*CmHandler)(void *context, uint32_t emsg, const uint8_t *body, size_t len);

Cm *cm_new(CmHandler handler, void *context);
void cm_free(Cm *cm);
/* Fetches the server list, connects to the first reachable server and says hello. */
int cm_connect(Cm *cm, char *err, size_t errlen);
void cm_disconnect(Cm *cm);
int cm_connected(Cm *cm);

/* Unified service call ("Authentication.BeginAuthSessionViaQR#1"). Fills *response (caller frees)
 * and *eresult. Returns 0 only if no reply arrived (disconnect or timeout). */
int cm_call(Cm *cm, const char *method, const PbBuf *body, int authed, int timeout_ms,
            PbBuf *response, int *eresult);

typedef struct CmPart {
    PbBuf body;
    struct CmPart *next;
} CmPart;

/* Job-based request (for example PICS). Collects every reply part; while a part has the varint
 * field pending_field set, more parts follow (pass 0 when the reply is a single message).
 * Returns 0 if the reply did not complete. */
int cm_request(Cm *cm, uint32_t emsg, const PbBuf *body, int pending_field, int timeout_ms, CmPart **parts);
void cm_parts_free(CmPart *parts);

/* Logs on with a refresh token. Returns the logon EResult (ERESULT_NO_CONNECTION/TIMEOUT locally). */
int cm_logon(Cm *cm, const char *account, const char *refresh_token, int timeout_ms);
int cm_logged_on(Cm *cm);
uint64_t cm_steamid(Cm *cm);
uint32_t cm_cell_id(Cm *cm);
/* Public IPv4 address Steam saw at logon (host byte order), 0 if unknown. */
uint32_t cm_public_ip(Cm *cm);
/* Seconds since the current logon. */
uint32_t cm_logon_age(Cm *cm);
/* Sends a message without waiting for a reply. */
int cm_send(Cm *cm, uint32_t emsg, const PbBuf *body);
/* As cm_request / cm_send, with CMsgProtoBufHeader.routing_appid set (app-scoped services such as
 * matchmaking route on it). */
int cm_request_routed(Cm *cm, uint32_t emsg, const PbBuf *body, uint32_t routing_appid, int pending_field,
                      int timeout_ms, CmPart **parts);
int cm_send_routed(Cm *cm, uint32_t emsg, const PbBuf *body, uint32_t routing_appid);
/* Sends ClientLogOff and waits briefly for the server to end the session. */
void cm_logoff(Cm *cm);

/* Name for an EResult value, for messages shown to the user. */
const char *cm_eresult_name(int eresult);

#endif
