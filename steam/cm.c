#include "cm.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>

#include "net.h"

#define PROTO_FLAG 0x80000000u
#define PROTOCOL_VERSION 65580u
#define EOS_LINUX_UNKNOWN (-203)
#define JOB_NONE UINT64_MAX
/* Individual account, desktop instance, public universe, account ID still unknown. */
#define STEAMID_LOGON_PLACEHOLDER UINT64_C(0x0110000100000000)

typedef struct Job {
    uint64_t id;
    int done;
    int eresult;
    PbBuf body;
    /* Multi-part replies: each part is kept while it carries pending_field = true. */
    int pending_field;
    CmPart *parts, **parts_tail;
    struct Job *next;
} Job;

typedef struct Outgoing {
    PbBuf packet;
    struct Outgoing *next;
} Outgoing;

struct Cm {
    CmHandler handler;
    void *context;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    int thread_running;
    int wake_fd;
    Ws ws;
    int connected;
    int stop;
    Outgoing *queue, **queue_tail;
    Job *jobs;
    uint64_t next_job;
    /* Session state set by the logon response. */
    int logon_pending, logon_result;
    int logged_on;
    uint64_t steamid;
    int32_t session_id;
    int heartbeat_seconds;
    uint32_t cell_id;              /* content-server region the logon response assigned */
    uint32_t public_ip;
    time_t logon_time;
};

const char *cm_eresult_name(int eresult)
{
    switch (eresult) {
    case ERESULT_OK: return "OK";
    case ERESULT_FAIL: return "failure";
    case ERESULT_NO_CONNECTION: return "no connection to Steam";
    case ERESULT_INVALID_PASSWORD: return "wrong account name or password";
    case ERESULT_ACCESS_DENIED: return "access denied";
    case ERESULT_TIMEOUT: return "Steam did not answer in time";
    case ERESULT_SERVICE_UNAVAILABLE: return "Steam service unavailable";
    case ERESULT_REVOKED: return "session revoked";
    case ERESULT_EXPIRED: return "session expired";
    case ERESULT_TRY_ANOTHER_CM: return "server busy, try again";
    case ERESULT_ACCOUNT_LOGON_DENIED: return "Steam Guard confirmation required";
    case ERESULT_INVALID_LOGIN_AUTH_CODE: return "wrong Steam Guard code";
    case ERESULT_RATE_LIMIT_EXCEEDED: return "too many attempts, wait and retry";
    case ERESULT_ACCOUNT_LOGIN_DENIED_THROTTLE: return "too many sign-in attempts, wait and retry";
    case ERESULT_TWO_FACTOR_CODE_MISMATCH: return "wrong authenticator code";
    default: return "Steam error";
    }
}

Cm *cm_new(CmHandler handler, void *context)
{
    Cm *cm = calloc(1, sizeof(*cm));
    if (!cm) return NULL;
    cm->handler = handler;
    cm->context = context;
    pthread_mutex_init(&cm->lock, NULL);
    pthread_cond_init(&cm->changed, NULL);
    cm->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    cm->queue_tail = &cm->queue;
    cm->next_job = 1;
    cm->ws.tls.fd = -1;
    return cm;
}

void cm_free(Cm *cm)
{
    if (!cm) return;
    cm_disconnect(cm);
    close(cm->wake_fd);
    pthread_mutex_destroy(&cm->lock);
    pthread_cond_destroy(&cm->changed);
    free(cm);
}

static void wake(Cm *cm)
{
    uint64_t one = 1;
    if (write(cm->wake_fd, &one, sizeof(one)) < 0) { /* already signalled */ }
}

/* Builds and queues a packet. Caller holds no lock. */
static int enqueue(Cm *cm, uint32_t emsg, uint64_t job_source, const char *job_name, const PbBuf *body)
{
    PbBuf header = {0};
    pthread_mutex_lock(&cm->lock);
    uint64_t steamid = cm->logged_on ? cm->steamid : (emsg == EMSG_CLIENT_LOGON ? STEAMID_LOGON_PLACEHOLDER : 0);
    int32_t session = cm->logged_on ? cm->session_id : 0;
    pthread_mutex_unlock(&cm->lock);
    pb_fixed64(&header, 1, steamid);
    pb_i32(&header, 2, session);
    if (job_source != JOB_NONE) pb_fixed64(&header, 10, job_source);
    pb_string(&header, 12, job_name);

    Outgoing *out = calloc(1, sizeof(*out));
    if (!out) { pb_free(&header); return 0; }
    uint8_t prefix[8];
    uint32_t tagged = emsg | PROTO_FLAG, header_len = (uint32_t)header.len;
    memcpy(prefix, &tagged, 4);           /* little-endian on every supported host */
    memcpy(prefix + 4, &header_len, 4);
    pb_raw(&out->packet, prefix, 8);
    pb_raw(&out->packet, header.data, header.len);
    if (body) pb_raw(&out->packet, body->data, body->len);
    int failed = header.failed || out->packet.failed || (body && body->failed);
    pb_free(&header);
    if (failed) { pb_free(&out->packet); free(out); return 0; }

    pthread_mutex_lock(&cm->lock);
    if (!cm->connected) {
        pthread_mutex_unlock(&cm->lock);
        pb_free(&out->packet);
        free(out);
        return 0;
    }
    *cm->queue_tail = out;
    cm->queue_tail = &out->next;
    pthread_mutex_unlock(&cm->lock);
    wake(cm);
    return 1;
}

typedef struct {
    uint64_t steamid;
    int32_t session_id;
    uint64_t job_target;
    int eresult;
} Header;

static void parse_header(const uint8_t *data, size_t len, Header *h)
{
    memset(h, 0, sizeof(*h));
    h->job_target = JOB_NONE;
    h->eresult = ERESULT_FAIL;
    PbReader r = { data, data + len };
    PbField f;
    while (pb_next(&r, &f) == 1) {
        if (f.field == 1) h->steamid = f.value;
        else if (f.field == 2) h->session_id = (int32_t)f.value;
        else if (f.field == 11) h->job_target = f.value;
        else if (f.field == 13) h->eresult = (int)(int32_t)f.value;
    }
}

static void dispatch(Cm *cm, const uint8_t *packet, size_t len);

/* CMsgMulti: optionally gzip'd run of [u32 length][packet]. */
static void dispatch_multi(Cm *cm, const uint8_t *body, size_t len)
{
    PbField size_field, payload;
    if (!pb_find(body, len, 2, &payload) || payload.type != PB_BYTES) return;
    const uint8_t *data = payload.data;
    size_t data_len = payload.len;
    uint8_t *unzipped = NULL;
    if (pb_find(body, len, 1, &size_field) && size_field.value) {
        if (size_field.value > (64u << 20) || !(unzipped = malloc(size_field.value))) return;
        z_stream z = {0};
        if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) { free(unzipped); return; }
        z.next_in = (Bytef *)payload.data;
        z.avail_in = (uInt)payload.len;
        z.next_out = unzipped;
        z.avail_out = (uInt)size_field.value;
        int status = inflate(&z, Z_FINISH);
        inflateEnd(&z);
        if (status != Z_STREAM_END || z.total_out != size_field.value) { free(unzipped); return; }
        data = unzipped;
        data_len = size_field.value;
    }
    while (data_len >= 4) {
        uint32_t sub;
        memcpy(&sub, data, 4);
        if (sub > data_len - 4) break;
        dispatch(cm, data + 4, sub);
        data += 4 + sub;
        data_len -= 4 + sub;
    }
    free(unzipped);
}

static void dispatch(Cm *cm, const uint8_t *packet, size_t len)
{
    if (len < 8) return;
    uint32_t tagged, header_len;
    memcpy(&tagged, packet, 4);
    memcpy(&header_len, packet + 4, 4);
    if (!(tagged & PROTO_FLAG) || header_len > len - 8) return;   /* only protobuf messages are used */
    uint32_t emsg = tagged & ~PROTO_FLAG;
    Header h;
    parse_header(packet + 8, header_len, &h);
    const uint8_t *body = packet + 8 + header_len;
    size_t body_len = len - 8 - header_len;

    if (emsg == EMSG_MULTI) { dispatch_multi(cm, body, body_len); return; }

    pthread_mutex_lock(&cm->lock);
    if (h.job_target != JOB_NONE) {
        for (Job *job = cm->jobs; job; job = job->next) {
            if (job->id != h.job_target || job->done) continue;
            job->eresult = h.eresult;
            if (job->pending_field) {
                CmPart *part = calloc(1, sizeof(*part));
                if (part) {
                    pb_raw(&part->body, body, body_len);
                    *job->parts_tail = part;
                    job->parts_tail = &part->next;
                }
                PbField pending;
                job->done = !(pb_find(body, body_len, (uint32_t)job->pending_field, &pending) && pending.value);
            } else {
                job->done = 1;
                pb_raw(&job->body, body, body_len);
            }
            pthread_cond_broadcast(&cm->changed);
            pthread_mutex_unlock(&cm->lock);
            return;
        }
    }
    if (emsg == EMSG_CLIENT_LOGON_RESPONSE) {
        PbField f;
        int eresult = pb_find(body, body_len, 1, &f) ? (int)(int32_t)f.value : ERESULT_FAIL;
        cm->logon_result = eresult;
        cm->logon_pending = 0;
        if (eresult == ERESULT_OK) {
            cm->logged_on = 1;
            cm->steamid = h.steamid;
            cm->session_id = h.session_id;
            cm->heartbeat_seconds = pb_find(body, body_len, 3, &f) && (int32_t)f.value > 0 ? (int)f.value : 30;
            if (pb_find(body, body_len, 7, &f)) cm->cell_id = (uint32_t)f.value;
            PbField ip;
            if (pb_find(body, body_len, 15, &f) && f.type == PB_BYTES && pb_find(f.data, f.len, 1, &ip))
                cm->public_ip = (uint32_t)ip.value;
            cm->logon_time = time(NULL);
        }
        pthread_cond_broadcast(&cm->changed);
    } else if (emsg == EMSG_CLIENT_LOGGED_OFF) {
        cm->logged_on = 0;
        pthread_cond_broadcast(&cm->changed);
    }
    pthread_mutex_unlock(&cm->lock);
    if (cm->handler) cm->handler(cm->context, emsg, body, body_len);
}

static void *io_thread(void *arg)
{
    Cm *cm = arg;
    time_t last_heartbeat = time(NULL);
    for (;;) {
        pthread_mutex_lock(&cm->lock);
        int stop = cm->stop;
        Outgoing *batch = cm->queue;
        cm->queue = NULL;
        cm->queue_tail = &cm->queue;
        int heartbeat = cm->logged_on ? cm->heartbeat_seconds : 0;
        pthread_mutex_unlock(&cm->lock);

        int failed = 0;
        while (batch) {
            Outgoing *next = batch->next;
            if (!failed && !ws_send(&cm->ws, WS_BINARY, batch->packet.data, batch->packet.len)) failed = 1;
            pb_free(&batch->packet);
            free(batch);
            batch = next;
        }
        if (stop || failed) break;

        if (heartbeat && time(NULL) - last_heartbeat >= heartbeat) {
            last_heartbeat = time(NULL);
            enqueue(cm, EMSG_CLIENT_HEARTBEAT, JOB_NONE, NULL, NULL);
            continue;
        }

        const uint8_t *message;
        size_t len;
        int status;
        while ((status = ws_poll_message(&cm->ws, &message, &len)) == 1)
            if (cm->ws.msg_opcode == WS_BINARY) dispatch(cm, message, len);
        if (status < 0) break;

        struct pollfd fds[2] = { { cm->ws.tls.fd, POLLIN, 0 }, { cm->wake_fd, POLLIN, 0 } };
        if (poll(fds, 2, 1000) < 0 && errno != EINTR) break;
        if (fds[1].revents & POLLIN) {
            uint64_t value;
            if (read(cm->wake_fd, &value, sizeof(value)) < 0) { /* drained */ }
        }
    }

    pthread_mutex_lock(&cm->lock);
    cm->connected = 0;
    cm->logged_on = 0;
    cm->logon_pending = 0;
    for (Outgoing *o = cm->queue; o;) { Outgoing *n = o->next; pb_free(&o->packet); free(o); o = n; }
    cm->queue = NULL;
    cm->queue_tail = &cm->queue;
    pthread_cond_broadcast(&cm->changed);
    pthread_mutex_unlock(&cm->lock);
    if (cm->handler) cm->handler(cm->context, 0, NULL, 0);
    return NULL;
}

/* Endpoints from GetCMListForConnect, in the server's preferred order. */
static int server_list(char endpoints[][128], int capacity, char *err, size_t errlen)
{
    char *body;
    size_t len;
    int status = https_get("api.steampowered.com",
                           "/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets&realm=steamglobal",
                           &body, &len, err, errlen);
    if (status != 200) {
        if (status) snprintf(err, errlen, "Steam server list returned HTTP %d", status);
        free(body);
        return 0;
    }
    int count = 0;
    for (char *p = body; count < capacity && (p = strstr(p, "\"endpoint\""));) {
        p += 10;
        char *open = strchr(p, '"');
        char *close = open ? strchr(open + 1, '"') : NULL;
        if (!close) break;
        size_t n = (size_t)(close - open - 1);
        if (n && n < 128) {
            memcpy(endpoints[count], open + 1, n);
            endpoints[count++][n] = 0;
        }
        p = close + 1;
    }
    free(body);
    if (!count) snprintf(err, errlen, "Steam server list was empty");
    return count;
}

int cm_connect(Cm *cm, char *err, size_t errlen)
{
    if (cm_connected(cm)) return 1;
    char endpoints[16][128];
    int count = server_list(endpoints, 16, err, errlen);
    for (int i = 0; i < count; i++) {
        if (!ws_connect(&cm->ws, endpoints[i], "/cmsocket/", err, errlen)) continue;
        pthread_mutex_lock(&cm->lock);
        cm->connected = 1;
        cm->stop = 0;
        pthread_mutex_unlock(&cm->lock);
        if (pthread_create(&cm->thread, NULL, io_thread, cm)) {
            ws_close(&cm->ws);
            cm->connected = 0;
            snprintf(err, errlen, "could not start the connection thread");
            return 0;
        }
        cm->thread_running = 1;
        PbBuf hello = {0};
        pb_u64(&hello, 1, PROTOCOL_VERSION);
        int ok = enqueue(cm, EMSG_CLIENT_HELLO, JOB_NONE, NULL, &hello);
        pb_free(&hello);
        if (ok) return 1;
        cm_disconnect(cm);
    }
    return 0;
}

void cm_disconnect(Cm *cm)
{
    if (!cm->thread_running) return;
    pthread_mutex_lock(&cm->lock);
    cm->stop = 1;
    pthread_mutex_unlock(&cm->lock);
    wake(cm);
    pthread_join(cm->thread, NULL);
    cm->thread_running = 0;
    ws_close(&cm->ws);
}

int cm_connected(Cm *cm)
{
    pthread_mutex_lock(&cm->lock);
    int connected = cm->connected;
    pthread_mutex_unlock(&cm->lock);
    return connected;
}

static void deadline_after(struct timespec *ts, int timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ms / 1000;
    ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

int cm_call(Cm *cm, const char *method, const PbBuf *body, int authed, int timeout_ms,
            PbBuf *response, int *eresult)
{
    memset(response, 0, sizeof(*response));
    *eresult = ERESULT_NO_CONNECTION;
    Job *job = calloc(1, sizeof(*job));
    if (!job) return 0;
    pthread_mutex_lock(&cm->lock);
    job->id = cm->next_job++;
    job->next = cm->jobs;
    cm->jobs = job;
    pthread_mutex_unlock(&cm->lock);

    int sent = enqueue(cm, authed ? EMSG_SERVICE_METHOD_CALL : EMSG_SERVICE_METHOD_CALL_NONAUTHED,
                       job->id, method, body);
    struct timespec deadline;
    deadline_after(&deadline, timeout_ms);
    pthread_mutex_lock(&cm->lock);
    while (sent && !job->done && cm->connected)
        if (pthread_cond_timedwait(&cm->changed, &cm->lock, &deadline) == ETIMEDOUT) {
            *eresult = ERESULT_TIMEOUT;
            break;
        }
    for (Job **link = &cm->jobs; *link; link = &(*link)->next)
        if (*link == job) { *link = job->next; break; }
    pthread_mutex_unlock(&cm->lock);
    int done = job->done;
    if (done) {
        *eresult = job->eresult;
        *response = job->body;
    }
    free(job);
    return done;
}

void cm_parts_free(CmPart *parts)
{
    while (parts) {
        CmPart *next = parts->next;
        pb_free(&parts->body);
        free(parts);
        parts = next;
    }
}

int cm_request(Cm *cm, uint32_t emsg, const PbBuf *body, int pending_field, int timeout_ms, CmPart **parts)
{
    *parts = NULL;
    Job *job = calloc(1, sizeof(*job));
    if (!job) return 0;
    job->pending_field = pending_field ? pending_field : -1;
    job->parts_tail = &job->parts;
    pthread_mutex_lock(&cm->lock);
    job->id = cm->next_job++;
    job->next = cm->jobs;
    cm->jobs = job;
    pthread_mutex_unlock(&cm->lock);

    int sent = enqueue(cm, emsg, job->id, NULL, body);
    struct timespec deadline;
    deadline_after(&deadline, timeout_ms);
    pthread_mutex_lock(&cm->lock);
    while (sent && !job->done && cm->connected)
        if (pthread_cond_timedwait(&cm->changed, &cm->lock, &deadline) == ETIMEDOUT) break;
    for (Job **link = &cm->jobs; *link; link = &(*link)->next)
        if (*link == job) { *link = job->next; break; }
    pthread_mutex_unlock(&cm->lock);
    int done = job->done;
    if (done) *parts = job->parts;
    else cm_parts_free(job->parts);
    free(job);
    return done;
}

int cm_logon(Cm *cm, const char *account, const char *refresh_token, int timeout_ms)
{
    char machine[256] = "Beer";
    gethostname(machine, sizeof(machine) - 1);
    PbBuf logon = {0};
    pb_u64(&logon, 1, PROTOCOL_VERSION);
    pb_string(&logon, 6, "english");
    pb_i32(&logon, 7, EOS_LINUX_UNKNOWN);
    pb_u64(&logon, 8, 1);                  /* should_remember_password */
    pb_string(&logon, 50, account);
    pb_string(&logon, 96, machine);
    pb_u64(&logon, 102, 1);                /* supports_rate_limit_response */
    pb_string(&logon, 108, refresh_token);

    pthread_mutex_lock(&cm->lock);
    cm->logon_pending = 1;
    cm->logon_result = ERESULT_TIMEOUT;
    pthread_mutex_unlock(&cm->lock);
    int sent = enqueue(cm, EMSG_CLIENT_LOGON, JOB_NONE, NULL, &logon);
    pb_free(&logon);
    if (!sent) return ERESULT_NO_CONNECTION;

    struct timespec deadline;
    deadline_after(&deadline, timeout_ms);
    pthread_mutex_lock(&cm->lock);
    while (cm->logon_pending && cm->connected)
        if (pthread_cond_timedwait(&cm->changed, &cm->lock, &deadline) == ETIMEDOUT) break;
    int result = cm->logon_pending ? (cm->connected ? ERESULT_TIMEOUT : ERESULT_NO_CONNECTION) : cm->logon_result;
    cm->logon_pending = 0;
    pthread_mutex_unlock(&cm->lock);
    return result;
}

int cm_logged_on(Cm *cm)
{
    pthread_mutex_lock(&cm->lock);
    int logged_on = cm->logged_on;
    pthread_mutex_unlock(&cm->lock);
    return logged_on;
}

uint64_t cm_steamid(Cm *cm)
{
    pthread_mutex_lock(&cm->lock);
    uint64_t steamid = cm->logged_on ? cm->steamid : 0;
    pthread_mutex_unlock(&cm->lock);
    return steamid;
}

uint32_t cm_cell_id(Cm *cm)
{
    pthread_mutex_lock(&cm->lock);
    uint32_t cell = cm->cell_id;
    pthread_mutex_unlock(&cm->lock);
    return cell;
}

uint32_t cm_public_ip(Cm *cm)
{
    pthread_mutex_lock(&cm->lock);
    uint32_t ip = cm->public_ip;
    pthread_mutex_unlock(&cm->lock);
    return ip;
}

uint32_t cm_logon_age(Cm *cm)
{
    pthread_mutex_lock(&cm->lock);
    uint32_t age = cm->logged_on ? (uint32_t)(time(NULL) - cm->logon_time) : 0;
    pthread_mutex_unlock(&cm->lock);
    return age;
}

int cm_send(Cm *cm, uint32_t emsg, const PbBuf *body)
{
    return enqueue(cm, emsg, JOB_NONE, NULL, body);
}

void cm_logoff(Cm *cm)
{
    if (!cm_logged_on(cm)) return;
    enqueue(cm, EMSG_CLIENT_LOGOFF, JOB_NONE, NULL, NULL);
    struct timespec deadline;
    deadline_after(&deadline, 3000);
    pthread_mutex_lock(&cm->lock);
    while (cm->logged_on && cm->connected)
        if (pthread_cond_timedwait(&cm->changed, &cm->lock, &deadline) == ETIMEDOUT) break;
    cm->logged_on = 0;
    pthread_mutex_unlock(&cm->lock);
}
