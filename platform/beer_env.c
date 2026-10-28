#include "beer_env.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#undef getenv
extern char *getenv(const char *name);   /* <stdlib.h> above declared beer_getenv through the macro */

/* Open-addressing table of BEER_* lookups, keyed by name text (names are sometimes built in reused buffers).
 * Readers never lock: an entry's name is published last, after its value. */
#define ENV_SLOTS 1024

typedef struct {
    const char *_Atomic name;
    char *value;
} EnvEntry;

static EnvEntry g_env[ENV_SLOTS];
static pthread_mutex_t g_env_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t name_hash(const char *name)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) h = (h ^ *p) * 16777619u;
    return h;
}

char *beer_getenv(const char *name)
{
    if (!name || strncmp(name, "BEER_", 5) != 0) return getenv(name);
    const uint32_t start = name_hash(name) % ENV_SLOTS;
    for (uint32_t probe = 0; probe < ENV_SLOTS; ++probe) {
        EnvEntry *e = &g_env[(start + probe) % ENV_SLOTS];
        const char *key = __atomic_load_n(&e->name, __ATOMIC_ACQUIRE);
        if (!key) break;
        if (!strcmp(key, name)) return e->value;
    }

    pthread_mutex_lock(&g_env_lock);
    char *value = getenv(name);
    for (uint32_t probe = 0; probe < ENV_SLOTS; ++probe) {
        EnvEntry *e = &g_env[(start + probe) % ENV_SLOTS];
        const char *key = __atomic_load_n(&e->name, __ATOMIC_ACQUIRE);
        if (key && !strcmp(key, name)) break;   /* another thread inserted it meanwhile */
        if (!key) {
            char *copy = strdup(name);
            if (copy) {
                e->value = value;
                __atomic_store_n(&e->name, copy, __ATOMIC_RELEASE);
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_env_lock);
    return value;
}
