#ifndef BEER_WIN32_WINHTTP_H
#define BEER_WIN32_WINHTTP_H
#include <stdint.h>

/* WinHTTP sessions, connections and requests. Handles, options, status callbacks and the documented
 * call sequence follow Windows. Beer has no HTTP transport yet, so sending a request fails the way it
 * does on a Windows computer without a network connection (ERROR_WINHTTP_NAME_NOT_RESOLVED): synchronously
 * from WinHttpSendRequest, or for asynchronous sessions through WINHTTP_CALLBACK_STATUS_REQUEST_ERROR on a
 * worker thread. */

#define BEER_MSABI __attribute__((ms_abi))

/* Sets GetLastError for the calling thread. */
typedef void (*BeerWinHttpSetErrorFn)(uint32_t error);
/* Starts fn(arg) on a host thread that may call guest code; nonzero on success. */
typedef int (*BeerWinHttpWorkerStartFn)(void *(*fn)(void *), void *arg);
void beer_winhttp_configure(BeerWinHttpSetErrorFn set_error, BeerWinHttpWorkerStartFn start_worker);
/* Optional: returns the application's return address for the current call, shown in the [WINHTTP] log. */
void beer_winhttp_set_caller(uint64_t (*caller)(void));

typedef void (BEER_MSABI *BeerWinHttpStatusCallback)(void *handle, uint64_t context, uint32_t status,
                                                     void *info, uint32_t info_length);

void *BEER_MSABI beer_WinHttpOpen(const uint16_t *agent, uint32_t access_type, const uint16_t *proxy,
                                  const uint16_t *bypass, uint32_t flags);
void *BEER_MSABI beer_WinHttpConnect(void *session, const uint16_t *server, uint32_t port, uint32_t reserved);
void *BEER_MSABI beer_WinHttpOpenRequest(void *connect, const uint16_t *verb, const uint16_t *object,
                                         const uint16_t *version, const uint16_t *referrer,
                                         const uint16_t **accept_types, uint32_t flags);
BeerWinHttpStatusCallback BEER_MSABI beer_WinHttpSetStatusCallback(void *handle, BeerWinHttpStatusCallback callback,
                                                                   uint32_t notification_flags, uint64_t reserved);
int BEER_MSABI beer_WinHttpSetOption(void *handle, uint32_t option, const void *buffer, uint32_t length);
int BEER_MSABI beer_WinHttpQueryOption(void *handle, uint32_t option, void *buffer, uint32_t *length);
int BEER_MSABI beer_WinHttpSetTimeouts(void *handle, int resolve, int connect, int send, int receive);
int BEER_MSABI beer_WinHttpAddRequestHeaders(void *request, const uint16_t *headers, uint32_t length, uint32_t modifiers);
int BEER_MSABI beer_WinHttpSetCredentials(void *request, uint32_t targets, uint32_t scheme, const uint16_t *user,
                                          const uint16_t *password, void *params);
int BEER_MSABI beer_WinHttpSendRequest(void *request, const uint16_t *headers, uint32_t headers_length,
                                       void *optional, uint32_t optional_length, uint32_t total_length,
                                       uint64_t context);
int BEER_MSABI beer_WinHttpWriteData(void *request, const void *buffer, uint32_t length, uint32_t *written);
int BEER_MSABI beer_WinHttpReceiveResponse(void *request, void *reserved);
int BEER_MSABI beer_WinHttpQueryHeaders(void *request, uint32_t info_level, const uint16_t *name, void *buffer,
                                        uint32_t *length, uint32_t *index);
int BEER_MSABI beer_WinHttpQueryDataAvailable(void *request, uint32_t *available);
int BEER_MSABI beer_WinHttpReadData(void *request, void *buffer, uint32_t length, uint32_t *read);
int BEER_MSABI beer_WinHttpCloseHandle(void *handle);
int BEER_MSABI beer_WinHttpGetIEProxyConfigForCurrentUser(void *config);
int BEER_MSABI beer_WinHttpGetDefaultProxyConfiguration(void *info);
int BEER_MSABI beer_WinHttpGetProxyForUrl(void *session, const uint16_t *url, void *options, void *info);
int BEER_MSABI beer_WinHttpDetectAutoProxyConfigUrl(uint32_t flags, uint16_t **url);

#define BEER_WINHTTP_FLAG_ASYNC 0x10000000u
#define BEER_WINHTTP_FLAG_SECURE 0x00800000u
#define BEER_WINHTTP_STATUS_HANDLE_CREATED 0x00000400u
#define BEER_WINHTTP_STATUS_HANDLE_CLOSING 0x00000800u
#define BEER_WINHTTP_STATUS_REQUEST_ERROR  0x00200000u
#define BEER_WINHTTP_OPTION_CONTEXT_VALUE 45u
#define BEER_ERROR_WINHTTP_NAME_NOT_RESOLVED 12007u
#define BEER_ERROR_WINHTTP_INCORRECT_HANDLE_TYPE 12018u
#define BEER_ERROR_WINHTTP_INCORRECT_HANDLE_STATE 12019u

#endif
