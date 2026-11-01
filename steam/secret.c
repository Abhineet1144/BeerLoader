#include "secret.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The subset of the libsecret / GLib ABI used here (libsecret-1.so.0). */
typedef struct { const char *name; int type; } SecretSchemaAttribute;
typedef struct {
    const char *name;
    int flags;
    SecretSchemaAttribute attributes[32];
    int reserved;
    void *reserved1, *reserved2, *reserved3, *reserved4, *reserved5, *reserved6, *reserved7;
} SecretSchema;
typedef struct { unsigned domain; int code; char *message; } GError;

typedef int (*StoreFn)(const SecretSchema *, const char *, const char *, const char *, void *, GError **, ...);
typedef char *(*LookupFn)(const SecretSchema *, void *, GError **, ...);
typedef int (*ClearFn)(const SecretSchema *, void *, GError **, ...);
typedef void (*FreeFn)(char *);
typedef void (*ErrorFreeFn)(GError *);

static const SecretSchema SCHEMA = {
    "dev.beer.Steam", 0,
    { { "kind", 0 }, { "account", 0 }, { NULL, 0 } },
    0, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

static struct {
    StoreFn store;
    LookupFn lookup;
    ClearFn clear;
    FreeFn free_password;
    ErrorFreeFn free_error;
} lib;

static void load_once(void)
{
    void *handle = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!handle) return;
    lib.store = (StoreFn)dlsym(handle, "secret_password_store_sync");
    lib.lookup = (LookupFn)dlsym(handle, "secret_password_lookup_sync");
    lib.clear = (ClearFn)dlsym(handle, "secret_password_clear_sync");
    lib.free_password = (FreeFn)dlsym(handle, "secret_password_free");
    lib.free_error = (ErrorFreeFn)dlsym(handle, "g_error_free");
    if (!lib.store || !lib.lookup || !lib.clear || !lib.free_password || !lib.free_error)
        memset(&lib, 0, sizeof(lib));
}

int secret_available(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_once);
    return lib.store != NULL;
}

static int report(GError *error, const char *what)
{
    if (!error) return 1;
    fprintf(stderr, "[STEAM] keyring %s failed: %s\n", what, error->message ? error->message : "unknown error");
    lib.free_error(error);
    return 0;
}

static int store(const char *kind, const char *account, const char *label, const char *value)
{
    if (!secret_available()) return 0;
    GError *error = NULL;
    int ok = lib.store(&SCHEMA, NULL, label, value, NULL, &error, "kind", kind, "account", account, NULL);
    return report(error, "write") && ok;
}

static char *lookup(const char *kind, const char *account)
{
    if (!secret_available()) return NULL;
    GError *error = NULL;
    char *value = account ? lib.lookup(&SCHEMA, NULL, &error, "kind", kind, "account", account, NULL)
                          : lib.lookup(&SCHEMA, NULL, &error, "kind", kind, NULL);
    report(error, "read");
    if (!value) return NULL;
    char *copy = strdup(value);
    lib.free_password(value);   /* libsecret wipes its copy */
    return copy;
}

void secret_wipe(char *s)
{
    if (!s) return;
    volatile char *p = s;
    while (*p) *p++ = 0;
    free(s);
}

int secret_save_session(const char *account, const char *refresh_token)
{
    size_t n = strlen(account) + strlen(refresh_token) + 2;
    char *value = malloc(n);
    if (!value) return 0;
    snprintf(value, n, "%s\n%s", account, refresh_token);
    /* One session at a time: replace whatever account was stored before. */
    secret_clear_session();
    int ok = store("session", account, "Beer Steam session", value);
    secret_wipe(value);
    return ok;
}

int secret_load_session(char **account, char **refresh_token)
{
    *account = *refresh_token = NULL;
    if (!secret_available()) return -1;
    char *value = lookup("session", NULL);
    if (!value) return 0;
    char *newline = strchr(value, '\n');
    if (!newline || newline == value || !newline[1]) { secret_wipe(value); return 0; }
    *newline = 0;
    *account = strdup(value);
    *refresh_token = strdup(newline + 1);
    *newline = '\n';
    secret_wipe(value);
    if (!*account || !*refresh_token) {
        free(*account);
        secret_wipe(*refresh_token);
        *account = *refresh_token = NULL;
        return 0;
    }
    return 1;
}

int secret_clear_session(void)
{
    if (!secret_available()) return 0;
    GError *error = NULL;
    lib.clear(&SCHEMA, NULL, &error, "kind", "session", NULL);
    return report(error, "clear");
}

int secret_save_guard(const char *account, const char *guard_data)
{
    return store("guard", account, "Beer Steam Guard device data", guard_data);
}

char *secret_load_guard(const char *account)
{
    return lookup("guard", account);
}

int secret_save_beta(const char *app_branch, const char *key_hex)
{
    return store("beta", app_branch, "Beer Steam beta branch key", key_hex);
}

char *secret_load_beta(const char *app_branch)
{
    return lookup("beta", app_branch);
}
