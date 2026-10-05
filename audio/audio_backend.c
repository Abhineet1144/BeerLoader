#define _GNU_SOURCE
#include "audio_backend.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pulse simple is loaded dynamically so Beer needs no development package or
 * link-time Pulse dependency. PipeWire exposes the compatible Pulse server on
 * the user's normal desktop session. */
typedef struct pa_simple pa_simple;
typedef struct {
    int format;
    uint32_t rate;
    uint8_t channels;
} pa_sample_spec;

typedef struct {
    uint32_t maxlength;
    uint32_t tlength;
    uint32_t prebuf;
    uint32_t minreq;
    uint32_t fragsize;
} pa_buffer_attr;

typedef pa_simple *(*PaSimpleNewFn)(const char *, const char *, int, const char *,
                                    const char *, const pa_sample_spec *,
                                    const void *, const void *, int *);
typedef int (*PaSimpleWriteFn)(pa_simple *, const void *, size_t, int *);
typedef int (*PaSimpleDrainFn)(pa_simple *, int *);
typedef int (*PaSimpleFlushFn)(pa_simple *, int *);
typedef void (*PaSimpleFreeFn)(pa_simple *);

enum { PA_STREAM_PLAYBACK = 1, PA_SAMPLE_S16LE = 3 };

typedef struct AudioPacket {
    void *data;
    size_t size;
    BeerAudioDoneFn done;
    void *opaque;
    struct AudioPacket *next;
} AudioPacket;

static struct {
    void *library;
    PaSimpleNewFn simple_new;
    PaSimpleWriteFn simple_write;
    PaSimpleDrainFn simple_drain;
    PaSimpleFlushFn simple_flush;
    PaSimpleFreeFn simple_free;
    pa_simple *stream;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    AudioPacket *head;
    AudioPacket *tail;
    size_t queued_bytes;
    size_t max_queued_bytes;
    atomic_uint_fast64_t bytes_played;
    int initialized;
    int running;
    int failed;
} g_audio;

static void audio_finish_packet(AudioPacket *packet)
{
    if (!packet) return;
    if (packet->done) packet->done(packet->opaque);
    free(packet->data);
    free(packet);
}

static void *audio_thread_main(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_audio.mutex);
        while (g_audio.running && !g_audio.head)
            pthread_cond_wait(&g_audio.condition, &g_audio.mutex);
        if (!g_audio.running && !g_audio.head) {
            pthread_mutex_unlock(&g_audio.mutex);
            break;
        }
        AudioPacket *packet = g_audio.head;
        g_audio.head = packet->next;
        if (!g_audio.head) g_audio.tail = NULL;
        if (g_audio.queued_bytes >= packet->size) g_audio.queued_bytes -= packet->size;
        else g_audio.queued_bytes = 0;
        pthread_cond_broadcast(&g_audio.condition);
        pthread_mutex_unlock(&g_audio.mutex);

        int error = 0;
        if (!g_audio.failed && g_audio.simple_write(g_audio.stream, packet->data,
                                                    packet->size, &error) == 0) {
            atomic_fetch_add_explicit(&g_audio.bytes_played, packet->size,
                                      memory_order_relaxed);
        } else if (!g_audio.failed) {
            fprintf(stderr, "[AUDIO] Pulse write failed (error=%d); disabling output\n", error);
            g_audio.failed = 1;
        }
        audio_finish_packet(packet);
    }
    return NULL;
}

int beer_audio_init(unsigned rate, unsigned channels)
{
    if (g_audio.initialized)
        return !g_audio.failed;
    memset(&g_audio, 0, sizeof(g_audio));
    g_audio.library = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!g_audio.library) {
        fprintf(stderr, "[AUDIO] libpulse-simple.so.0 unavailable: %s\n", dlerror());
        g_audio.failed = 1;
        g_audio.initialized = 1;
        return 0;
    }
#define LOAD(field, symbol) do { *(void **)(&g_audio.field) = dlsym(g_audio.library, symbol); \
    if (!g_audio.field) goto load_failed; } while (0)
    LOAD(simple_new, "pa_simple_new");
    LOAD(simple_write, "pa_simple_write");
    LOAD(simple_drain, "pa_simple_drain");
    LOAD(simple_flush, "pa_simple_flush");
    LOAD(simple_free, "pa_simple_free");
#undef LOAD
    pa_sample_spec spec = { PA_SAMPLE_S16LE, rate, (uint8_t)channels };
    uint32_t bytes_per_second = rate * channels * (uint32_t)sizeof(int16_t);
    pa_buffer_attr attr = {
        .maxlength = bytes_per_second / 4u, /* 250 ms hard ceiling */
        .tlength = bytes_per_second / 10u, /* target about 100 ms */
        .prebuf = UINT32_MAX,               /* server default */
        .minreq = bytes_per_second / 100u, /* 10 ms refill granularity */
        .fragsize = UINT32_MAX,
    };
    int error = 0;
    g_audio.stream = g_audio.simple_new(NULL, "Beer", PA_STREAM_PLAYBACK, NULL,
                                        "Sekiro", &spec, NULL, &attr, &error);
    if (!g_audio.stream) {
        fprintf(stderr, "[AUDIO] cannot open PipeWire/Pulse stream (error=%d)\n", error);
        goto load_failed;
    }
    pthread_mutex_init(&g_audio.mutex, NULL);
    pthread_cond_init(&g_audio.condition, NULL);
    g_audio.running = 1;
    /* Bound copied PCM to roughly 250 ms. This keeps the FMOD mixer thread
     * decoupled from PipeWire without allowing unbounded latency/memory growth. */
    g_audio.max_queued_bytes = (size_t)rate * channels * sizeof(int16_t) / 4u;
    g_audio.initialized = 1;
    if (pthread_create(&g_audio.thread, NULL, audio_thread_main, NULL) != 0) {
        g_audio.running = 0;
        goto load_failed;
    }
    fprintf(stderr, "[AUDIO] PipeWire/Pulse output initialized: %u Hz, %u channels, S16LE\n",
            rate, channels);
    return 1;

load_failed:
    if (g_audio.stream && g_audio.simple_free) g_audio.simple_free(g_audio.stream);
    if (g_audio.library) dlclose(g_audio.library);
    g_audio.stream = NULL;
    g_audio.library = NULL;
    g_audio.failed = 1;
    g_audio.initialized = 1;
    return 0;
}

int beer_audio_submit(const void *pcm, size_t bytes, BeerAudioDoneFn done, void *opaque)
{
    if (!pcm || !bytes || !g_audio.initialized || g_audio.failed || !g_audio.running)
        return 0;
    AudioPacket *packet = calloc(1, sizeof(*packet));
    if (!packet) return 0;
    packet->data = malloc(bytes);
    if (!packet->data) { free(packet); return 0; }
    memcpy(packet->data, pcm, bytes);
    packet->size = bytes;
    packet->done = done;
    packet->opaque = opaque;
    pthread_mutex_lock(&g_audio.mutex);
    /* Always admit one packet into an empty queue, even when a legacy API
     * submits a block larger than the latency target. Otherwise that producer
     * waits forever for queued_bytes to shrink below zero. Subsequent packets
     * still observe the configured back-pressure limit. */
    while (g_audio.running && !g_audio.failed && g_audio.max_queued_bytes &&
           g_audio.queued_bytes != 0 &&
           g_audio.queued_bytes + bytes > g_audio.max_queued_bytes)
        pthread_cond_wait(&g_audio.condition, &g_audio.mutex);
    if (!g_audio.running || g_audio.failed) {
        pthread_mutex_unlock(&g_audio.mutex);
        audio_finish_packet(packet);
        return 0;
    }
    if (g_audio.tail) g_audio.tail->next = packet;
    else g_audio.head = packet;
    g_audio.tail = packet;
    g_audio.queued_bytes += bytes;
    pthread_cond_signal(&g_audio.condition);
    pthread_mutex_unlock(&g_audio.mutex);
    return 1;
}

void beer_audio_reset(void)
{
    if (!g_audio.initialized || !g_audio.stream) return;
    pthread_mutex_lock(&g_audio.mutex);
    AudioPacket *packet = g_audio.head;
    g_audio.head = g_audio.tail = NULL;
    g_audio.queued_bytes = 0;
    pthread_cond_broadcast(&g_audio.condition);
    pthread_mutex_unlock(&g_audio.mutex);
    while (packet) {
        AudioPacket *next = packet->next;
        audio_finish_packet(packet);
        packet = next;
    }
    int error = 0;
    g_audio.simple_flush(g_audio.stream, &error);
}

void beer_audio_shutdown(void)
{
    if (!g_audio.initialized) return;
    if (g_audio.running) {
        pthread_mutex_lock(&g_audio.mutex);
        g_audio.running = 0;
        pthread_cond_broadcast(&g_audio.condition);
        pthread_mutex_unlock(&g_audio.mutex);
        pthread_join(g_audio.thread, NULL);
    }
    if (g_audio.stream) {
        int error = 0;
        g_audio.simple_drain(g_audio.stream, &error);
        g_audio.simple_free(g_audio.stream);
    }
    if (g_audio.library) dlclose(g_audio.library);
    memset(&g_audio, 0, sizeof(g_audio));
}

int beer_audio_available(void)
{
    return g_audio.initialized && !g_audio.failed;
}

uint64_t beer_audio_bytes_played(void)
{
    return atomic_load_explicit(&g_audio.bytes_played, memory_order_relaxed);
}
