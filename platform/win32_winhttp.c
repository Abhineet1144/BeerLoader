#include "win32_winhttp.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERROR_INVALID_HANDLE 6u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_WINHTTP_INVALID_OPTION 12009u
#define ERROR_WINHTTP_AUTODETECTION_FAILED 12180u
#define API_SEND_REQUEST 5u
#define INVALID_STATUS_CALLBACK ((BeerWinHttpStatusCallback)(intptr_t)-1)

enum { TYPE_SESSION = 1, TYPE_CONNECT = 2, TYPE_REQUEST = 3 };
enum { STATE_OPEN, STATE_SENDING, STATE_FAILED };

typedef struct Handle {
    uint32_t type;
    struct Handle *next;             /* registry */
    struct Handle *parent;
    int refs;                        /* the application's handle plus pending work */
    int closed;
    int async;
    int secure;
    int state;
    BeerWinHttpStatusCallback callback;
    uint32_t notify;
    uint64_t context;
    uint32_t port;
    char host[256];
    char verb[16];
    char object[512];
} Handle;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Handle *g_handles;
static BeerWinHttpSetErrorFn g_set_error;
static BeerWinHttpWorkerStartFn g_start_worker;
static uint64_t (*g_caller)(void);

void beer_winhttp_set_caller(uint64_t (*caller)(void)) { g_caller = caller; }
static unsigned long long caller(void) { return g_caller ? (unsigned long long)g_caller() : 0; }

void beer_winhttp_configure(BeerWinHttpSetErrorFn set_error, BeerWinHttpWorkerStartFn start_worker)
{
    g_set_error = set_error;
    g_start_worker = start_worker;
}

/* WinHTTP functions leave ERROR_SUCCESS behind when they succeed; callers that read GetLastError after a later
 * failure that set no error of its own (a NULL from a hooked or replaced function) see 0. */
static void clear_error(void)
{
    if (g_set_error) g_set_error(0);
}

static int fail(uint32_t error)
{
    if (g_set_error) g_set_error(error);
    return 0;
}

static void narrow(const uint16_t *text, char *out, size_t size)
{
    size_t n = 0;
    for (; text && text[n] && n + 1 < size; ++n) out[n] = text[n] >= 32 && text[n] < 127 ? (char)text[n] : '?';
    out[n] = 0;
}

/* Looks up a live handle of one of the given types and takes a reference. */
static Handle *acquire(void *opaque, uint32_t types)
{
    pthread_mutex_lock(&g_lock);
    Handle *found = NULL;
    for (Handle *h = g_handles; h; h = h->next)
        if (h == opaque && !h->closed) { found = h; break; }
    if (found && types && !((1u << found->type) & types)) {
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "[WINHTTP] handle %p has the wrong type for this call  from 0x%llx\n", opaque, caller());
        fail(BEER_ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
        return NULL;
    }
    if (found) ++found->refs;
    pthread_mutex_unlock(&g_lock);
    if (!found) {
        fprintf(stderr, "[WINHTTP] invalid handle %p  from 0x%llx\n", opaque, caller());
        fail(ERROR_INVALID_HANDLE);
    }
    return found;
}

static void notify(Handle *h, uint32_t status, void *info, uint32_t length)
{
    BeerWinHttpStatusCallback callback = h->callback;
    if (callback && callback != INVALID_STATUS_CALLBACK && (h->notify & status))
        callback(h, h->context, status, info, length);
}

/* Drops a reference; the last one removes the handle, after WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING (the final
 * notification a handle receives, so applications free their context there). */
static void release(Handle *h)
{
    pthread_mutex_lock(&g_lock);
    const int last = --h->refs == 0;
    if (last)
        for (Handle **link = &g_handles; *link; link = &(*link)->next)
            if (*link == h) { *link = h->next; break; }
    pthread_mutex_unlock(&g_lock);
    if (!last) return;
    void *value = h;
    notify(h, BEER_WINHTTP_STATUS_HANDLE_CLOSING, &value, sizeof(value));
    Handle *parent = h->parent;
    free(h);
    if (parent) release(parent);
}

static Handle *create(uint32_t type, Handle *parent)
{
    Handle *h = calloc(1, sizeof(*h));
    if (!h) { fail(8 /* ERROR_NOT_ENOUGH_MEMORY */); return NULL; }
    h->type = type;
    h->refs = 1;
    if (parent) {
        /* Children inherit the parent's callback, notifications and mode; they keep the parent alive. */
        h->parent = parent;
        h->async = parent->async;
        h->callback = parent->callback;
        h->notify = parent->notify;
        h->context = parent->context;
        h->secure = parent->secure;
        h->port = parent->port;
        memcpy(h->host, parent->host, sizeof(h->host));
        pthread_mutex_lock(&g_lock);
        ++parent->refs;
        pthread_mutex_unlock(&g_lock);
    }
    pthread_mutex_lock(&g_lock);
    h->next = g_handles;
    g_handles = h;
    pthread_mutex_unlock(&g_lock);
    return h;
}

void *BEER_MSABI beer_WinHttpOpen(const uint16_t *agent, uint32_t access_type, const uint16_t *proxy,
                                  const uint16_t *bypass, uint32_t flags)
{
    clear_error();
    (void)proxy; (void)bypass;
    char text[128];
    narrow(agent, text, sizeof(text));
    Handle *h = create(TYPE_SESSION, NULL);
    if (h) h->async = (flags & BEER_WINHTTP_FLAG_ASYNC) != 0;
    fprintf(stderr, "[WINHTTP] Open(agent=\"%s\", access=%u, flags=0x%x) -> %p  from 0x%llx\n", text, access_type, flags,
            (void *)h, caller());
    return h;
}

void *BEER_MSABI beer_WinHttpConnect(void *session, const uint16_t *server, uint32_t port, uint32_t reserved)
{
    clear_error();
    (void)reserved;
    Handle *parent = acquire(session, 1u << TYPE_SESSION);
    if (!parent) return NULL;
    Handle *h = NULL;
    if (!server || !server[0]) fail(ERROR_INVALID_PARAMETER);
    else if ((h = create(TYPE_CONNECT, parent))) {
        narrow(server, h->host, sizeof(h->host));
        h->port = port;
    }
    release(parent);
    if (h) {
        void *value = h;
        notify(h, BEER_WINHTTP_STATUS_HANDLE_CREATED, &value, sizeof(value));
    }
    fprintf(stderr, "[WINHTTP] Connect(%s:%u) -> %p\n", h ? h->host : "?", port, (void *)h);
    return h;
}

void *BEER_MSABI beer_WinHttpOpenRequest(void *connect, const uint16_t *verb, const uint16_t *object,
                                         const uint16_t *version, const uint16_t *referrer,
                                         const uint16_t **accept_types, uint32_t flags)
{
    clear_error();
    (void)version; (void)referrer; (void)accept_types;
    Handle *parent = acquire(connect, 1u << TYPE_CONNECT);
    if (!parent) return NULL;
    Handle *h = create(TYPE_REQUEST, parent);
    release(parent);
    if (!h) return NULL;
    if (verb && verb[0]) narrow(verb, h->verb, sizeof(h->verb));
    else strcpy(h->verb, "GET");
    if (object && object[0]) narrow(object, h->object, sizeof(h->object));
    else strcpy(h->object, "/");
    h->secure = (flags & BEER_WINHTTP_FLAG_SECURE) != 0;
    if (!h->port) h->port = h->secure ? 443 : 80;
    void *value = h;
    notify(h, BEER_WINHTTP_STATUS_HANDLE_CREATED, &value, sizeof(value));
    fprintf(stderr, "[WINHTTP] OpenRequest(%s %s://%s:%u%s) -> %p\n", h->verb, h->secure ? "https" : "http",
            h->host, h->port, h->object, (void *)h);
    return h;
}

BeerWinHttpStatusCallback BEER_MSABI beer_WinHttpSetStatusCallback(void *handle, BeerWinHttpStatusCallback callback,
                                                                   uint32_t notification_flags, uint64_t reserved)
{
    clear_error();
    (void)reserved;
    Handle *h = acquire(handle, 0);
    if (!h) return INVALID_STATUS_CALLBACK;
    pthread_mutex_lock(&g_lock);
    BeerWinHttpStatusCallback previous = h->callback;
    h->callback = callback;
    h->notify = notification_flags;
    pthread_mutex_unlock(&g_lock);
    release(h);
    fprintf(stderr, "[WINHTTP] SetStatusCallback(%p, flags=0x%x)\n", handle, notification_flags);
    return previous;
}

int BEER_MSABI beer_WinHttpSetOption(void *handle, uint32_t option, const void *buffer, uint32_t length)
{
    clear_error();
    /* Global options may be set on a NULL handle. */
    Handle *h = handle ? acquire(handle, 0) : NULL;
    if (handle && !h) return 0;
    if (option == BEER_WINHTTP_OPTION_CONTEXT_VALUE) {
        if (!h || !buffer || length < sizeof(uint64_t)) { if (h) release(h); return fail(ERROR_INVALID_PARAMETER); }
        memcpy(&h->context, buffer, sizeof(uint64_t));
    }
    fprintf(stderr, "[WINHTTP] SetOption(%p, %u, length=%u)\n", handle, option, length);
    if (h) release(h);
    return 1;
}

int BEER_MSABI beer_WinHttpQueryOption(void *handle, uint32_t option, void *buffer, uint32_t *length)
{
    clear_error();
    Handle *h = acquire(handle, 0);
    if (!h) return 0;
    int ok = 0;
    if (!length) fail(ERROR_INVALID_PARAMETER);
    else if (option == BEER_WINHTTP_OPTION_CONTEXT_VALUE) {
        if (!buffer || *length < sizeof(uint64_t)) { *length = sizeof(uint64_t); fail(ERROR_INSUFFICIENT_BUFFER); }
        else { memcpy(buffer, &h->context, sizeof(uint64_t)); *length = sizeof(uint64_t); ok = 1; }
    } else fail(ERROR_WINHTTP_INVALID_OPTION);
    fprintf(stderr, "[WINHTTP] QueryOption(%p, %u) -> %d\n", handle, option, ok);
    release(h);
    return ok;
}

int BEER_MSABI beer_WinHttpSetTimeouts(void *handle, int resolve, int connect, int send, int receive)
{
    clear_error();
    Handle *h = acquire(handle, 0);
    fprintf(stderr, "[WINHTTP] SetTimeouts(%p, %d, %d, %d, %d)  from 0x%llx\n", handle, resolve, connect, send, receive, caller());
    if (!h) return 0;
    release(h);
    if (resolve < -1 || connect < -1 || send < -1 || receive < -1) return fail(ERROR_INVALID_PARAMETER);
    return 1;
}

int BEER_MSABI beer_WinHttpAddRequestHeaders(void *request, const uint16_t *headers, uint32_t length, uint32_t modifiers)
{
    clear_error();
    (void)length; (void)modifiers;
    Handle *h = acquire(request, 1u << TYPE_REQUEST);
    if (!h) return 0;
    release(h);
    if (!headers) return fail(ERROR_INVALID_PARAMETER);
    return 1;
}

int BEER_MSABI beer_WinHttpSetCredentials(void *request, uint32_t targets, uint32_t scheme, const uint16_t *user,
                                          const uint16_t *password, void *params)
{
    clear_error();
    (void)targets; (void)scheme; (void)user; (void)password; (void)params;
    Handle *h = acquire(request, 1u << TYPE_REQUEST);
    if (!h) return 0;
    release(h);
    return 1;
}

static void *deliver_send_error(void *opaque)
{
    Handle *h = opaque;
    struct { uint64_t result; uint32_t error; uint32_t pad; } async_result = { API_SEND_REQUEST, BEER_ERROR_WINHTTP_NAME_NOT_RESOLVED, 0 };
    h->state = STATE_FAILED;
    if (!h->closed) notify(h, BEER_WINHTTP_STATUS_REQUEST_ERROR, &async_result, sizeof(async_result));
    release(h);
    return NULL;
}

int BEER_MSABI beer_WinHttpSendRequest(void *request, const uint16_t *headers, uint32_t headers_length,
                                       void *optional, uint32_t optional_length, uint32_t total_length,
                                       uint64_t context)
{
    clear_error();
    (void)headers; (void)headers_length; (void)optional; (void)optional_length; (void)total_length;
    Handle *h = acquire(request, 1u << TYPE_REQUEST);
    if (!h) return 0;
    h->context = context;
    fprintf(stderr, "[WINHTTP] SendRequest(%s %s://%s:%u%s, %s): no network transport; failing as offline\n",
            h->verb, h->secure ? "https" : "http", h->host, h->port, h->object, h->async ? "async" : "sync");
    if (!h->async) {
        h->state = STATE_FAILED;
        release(h);
        return fail(BEER_ERROR_WINHTTP_NAME_NOT_RESOLVED);
    }
    /* Asynchronous: the call is accepted and the outcome arrives through the callback on another thread,
     * as Windows reports name resolution failures. The reference taken above belongs to that work. */
    h->state = STATE_SENDING;
    if (!g_start_worker || !g_start_worker(deliver_send_error, h)) {
        h->state = STATE_FAILED;
        release(h);
        return fail(BEER_ERROR_WINHTTP_NAME_NOT_RESOLVED);
    }
    return 1;
}

/* Calls that need a sent request and a response; with no transport no request ever reaches that state. */
static int needs_response(void *request, const char *name)
{
    Handle *h = acquire(request, 1u << TYPE_REQUEST);
    if (!h) return 0;
    fprintf(stderr, "[WINHTTP] %s(%p): no response\n", name, request);
    release(h);
    return fail(BEER_ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
}

int BEER_MSABI beer_WinHttpWriteData(void *request, const void *buffer, uint32_t length, uint32_t *written)
{
    clear_error();
    (void)buffer; (void)length;
    if (written) *written = 0;
    return needs_response(request, "WriteData");
}

int BEER_MSABI beer_WinHttpReceiveResponse(void *request, void *reserved)
{
    clear_error();
    (void)reserved;
    return needs_response(request, "ReceiveResponse");
}

int BEER_MSABI beer_WinHttpQueryHeaders(void *request, uint32_t info_level, const uint16_t *name, void *buffer,
                                        uint32_t *length, uint32_t *index)
{
    clear_error();
    (void)info_level; (void)name; (void)buffer; (void)length; (void)index;
    return needs_response(request, "QueryHeaders");
}

int BEER_MSABI beer_WinHttpQueryDataAvailable(void *request, uint32_t *available)
{
    clear_error();
    if (available) *available = 0;
    return needs_response(request, "QueryDataAvailable");
}

int BEER_MSABI beer_WinHttpReadData(void *request, void *buffer, uint32_t length, uint32_t *read)
{
    clear_error();
    (void)buffer; (void)length;
    if (read) *read = 0;
    return needs_response(request, "ReadData");
}

int BEER_MSABI beer_WinHttpCloseHandle(void *handle)
{
    clear_error();
    pthread_mutex_lock(&g_lock);
    Handle *h = NULL;
    for (Handle *it = g_handles; it; it = it->next)
        if (it == handle && !it->closed) { h = it; break; }
    if (h) h->closed = 1;
    pthread_mutex_unlock(&g_lock);
    if (!h) return fail(ERROR_INVALID_HANDLE);
    fprintf(stderr, "[WINHTTP] CloseHandle(%p)  from 0x%llx\n", handle, caller());
    release(h);
    return 1;
}

int BEER_MSABI beer_WinHttpGetIEProxyConfigForCurrentUser(void *config)
{
    clear_error();
    /* WINHTTP_CURRENT_USER_IE_PROXY_CONFIG: no automatic detection, no script, no proxy. */
    if (!config) return fail(ERROR_INVALID_PARAMETER);
    memset(config, 0, 32);
    return 1;
}

int BEER_MSABI beer_WinHttpGetDefaultProxyConfiguration(void *info)
{
    clear_error();
    /* WINHTTP_PROXY_INFO: WINHTTP_ACCESS_TYPE_NO_PROXY. */
    if (!info) return fail(ERROR_INVALID_PARAMETER);
    memset(info, 0, 24);
    *(uint32_t *)info = 1;
    return 1;
}

int BEER_MSABI beer_WinHttpGetProxyForUrl(void *session, const uint16_t *url, void *options, void *info)
{
    clear_error();
    (void)url; (void)options; (void)info;
    Handle *h = acquire(session, 1u << TYPE_SESSION);
    if (!h) return 0;
    release(h);
    return fail(ERROR_WINHTTP_AUTODETECTION_FAILED);
}

int BEER_MSABI beer_WinHttpDetectAutoProxyConfigUrl(uint32_t flags, uint16_t **url)
{
    clear_error();
    (void)flags;
    if (url) *url = NULL;
    return fail(ERROR_WINHTTP_AUTODETECTION_FAILED);
}
