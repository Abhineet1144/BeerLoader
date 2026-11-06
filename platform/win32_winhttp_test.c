#include "win32_winhttp.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static __thread uint32_t last_error;
static void set_error(uint32_t error) { last_error = error; }

static int start_thread(void *(*fn)(void *), void *arg)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, fn, arg)) return 0;
    pthread_detach(thread);
    return 1;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t statuses[32];
static uint64_t contexts[32];
static uint32_t async_error;
static uint64_t async_api;
static int status_count;

static void BEER_MSABI callback(void *handle, uint64_t context, uint32_t status, void *info, uint32_t length)
{
    (void)handle;
    pthread_mutex_lock(&lock);
    if (status_count < 32) { statuses[status_count] = status; contexts[status_count] = context; ++status_count; }
    if (status == BEER_WINHTTP_STATUS_REQUEST_ERROR) {
        assert(length == 16);
        memcpy(&async_api, info, 8);
        memcpy(&async_error, (char *)info + 8, 4);
    }
    pthread_mutex_unlock(&lock);
}

static int seen(uint32_t status)
{
    pthread_mutex_lock(&lock);
    int count = 0;
    for (int i = 0; i < status_count; ++i) count += statuses[i] == status;
    pthread_mutex_unlock(&lock);
    return count;
}

static void wide(const char *text, uint16_t *out)
{
    size_t i = 0;
    for (; text[i]; ++i) out[i] = (uint8_t)text[i];
    out[i] = 0;
}

int main(void)
{
    beer_winhttp_configure(set_error, start_thread);
    uint16_t agent[32], host[64], verb[8], path[64];
    wide("Test/1.0", agent);
    wide("hjm.example.com", host);
    wide("GET", verb);
    wide("/v1/content", path);

    /* Synchronous: handles are created; sending fails as on an offline computer. */
    void *session = beer_WinHttpOpen(agent, 0, NULL, NULL, 0);
    assert(session);
    void *connect = beer_WinHttpConnect(session, host, 0, 0);
    void *request = beer_WinHttpOpenRequest(connect, verb, path, NULL, NULL, NULL, BEER_WINHTTP_FLAG_SECURE);
    assert(connect && request);
    last_error = 1234;
    assert(beer_WinHttpSetTimeouts(request, 1000, 1000, 1000, 1000) && last_error == 0);   /* success leaves 0 */
    assert(!beer_WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, 7) && last_error == BEER_ERROR_WINHTTP_NAME_NOT_RESOLVED);
    assert(!beer_WinHttpReceiveResponse(request, NULL) && last_error == BEER_ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
    /* Handle types are checked. */
    assert(!beer_WinHttpOpenRequest(session, verb, path, NULL, NULL, NULL, 0) &&
           last_error == BEER_ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    assert(beer_WinHttpCloseHandle(request) && beer_WinHttpCloseHandle(connect) && beer_WinHttpCloseHandle(session));
    assert(!beer_WinHttpCloseHandle(session) && last_error == 6);
    assert(!beer_WinHttpConnect((void *)0x1234, host, 0, 0) && last_error == 6);

    /* Asynchronous: SendRequest is accepted, REQUEST_ERROR(API_SEND_REQUEST, NAME_NOT_RESOLVED) arrives with the
     * send context, and every handle reports HANDLE_CLOSING last. */
    session = beer_WinHttpOpen(agent, 0, NULL, NULL, BEER_WINHTTP_FLAG_ASYNC);
    assert(beer_WinHttpSetStatusCallback(session, callback, 0xffffffffu, 0) == NULL);
    connect = beer_WinHttpConnect(session, host, 443, 0);
    request = beer_WinHttpOpenRequest(connect, verb, path, NULL, NULL, NULL, BEER_WINHTTP_FLAG_SECURE);
    assert(seen(BEER_WINHTTP_STATUS_HANDLE_CREATED) == 2);
    uint64_t value = 0x55, read_back = 0;
    uint32_t length = sizeof(read_back);
    assert(beer_WinHttpSetOption(request, BEER_WINHTTP_OPTION_CONTEXT_VALUE, &value, sizeof(value)));
    assert(beer_WinHttpQueryOption(request, BEER_WINHTTP_OPTION_CONTEXT_VALUE, &read_back, &length) && read_back == 0x55);
    assert(beer_WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, 0x99));
    for (int i = 0; i < 200 && !seen(BEER_WINHTTP_STATUS_REQUEST_ERROR); ++i) usleep(5000);
    assert(seen(BEER_WINHTTP_STATUS_REQUEST_ERROR) == 1);
    assert(async_api == 5 && async_error == BEER_ERROR_WINHTTP_NAME_NOT_RESOLVED);
    pthread_mutex_lock(&lock);
    assert(contexts[status_count - 1] == 0x99);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < 200 && beer_WinHttpCloseHandle(request) == 0; ++i) usleep(1000);
    assert(beer_WinHttpCloseHandle(connect) && beer_WinHttpCloseHandle(session));
    for (int i = 0; i < 200 && seen(BEER_WINHTTP_STATUS_HANDLE_CLOSING) < 3; ++i) usleep(5000);
    assert(seen(BEER_WINHTTP_STATUS_HANDLE_CLOSING) == 3);
    pthread_mutex_lock(&lock);
    assert(statuses[status_count - 1] == BEER_WINHTTP_STATUS_HANDLE_CLOSING);
    pthread_mutex_unlock(&lock);

    /* Proxy queries: direct connection, no automatic configuration. */
    struct { uint32_t access; uint32_t pad; void *proxy, *bypass; } info;
    assert(beer_WinHttpGetDefaultProxyConfiguration(&info) && info.access == 1 && !info.proxy);
    uint16_t *script = (uint16_t *)1;
    assert(!beer_WinHttpDetectAutoProxyConfigUrl(3, &script) && !script && last_error == 12180);

    puts("winhttp tests passed");
    return 0;
}
