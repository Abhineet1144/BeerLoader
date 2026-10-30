#define _GNU_SOURCE
#include <unistd.h>
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
typedef uint64_t (*PaSimpleLatencyFn)(pa_simple *, int *);

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
    PaSimpleLatencyFn simple_latency;   /* optional: underrun diagnostics */
    pa_simple *stream;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    AudioPacket *head;
    AudioPacket *tail;
    size_t queued_bytes;
    size_t max_queued_bytes;
    size_t piece_bytes;
    atomic_uint_fast64_t bytes_played;
    atomic_uint_fast64_t write_started_ms;   /* 0 while no pa_simple_write is in flight */
    pthread_t watchdog;
    int have_watchdog;
    int initialized;
    int running;
    int failed;
    int null_sink;                 /* no sound server: accept PCM and discard it at real-time pace */
    uint64_t bytes_per_second;
} g_audio;

static void audio_finish_packet(AudioPacket *packet)
{
    if (!packet) return;
    if (packet->done) packet->done(packet->opaque);
    free(packet->data);
    free(packet);
}

static uint64_t audio_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* A pa_simple_write that makes no progress for over a second means the server stopped asking for data; a flush
 * restarts the request flow, and the game's audio thread (which can hold game locks while it waits) goes on. */
static void *audio_watchdog_main(void *unused)
{
    (void)unused;
    while (g_audio.running) {
        usleep(250000);
        const uint64_t started = atomic_load(&g_audio.write_started_ms);
        if (getenv("BEER_AUDIO_DIAGNOSTICS")) {
            static unsigned tick;
            if (++tick % 8 == 0)
                fprintf(stderr, "[AUDIO] watch: write age=%llu ms played=%llu queued=%zu\n",
                        (unsigned long long)(started ? audio_now_ms() - started : 0),
                        (unsigned long long)atomic_load(&g_audio.bytes_played), g_audio.queued_bytes);
        }
        if (started && audio_now_ms() - started > 1000) {
            int error = 0;
            fprintf(stderr, "[AUDIO] sound server stopped taking data for %llu ms; flushing the stream\n",
                    (unsigned long long)(audio_now_ms() - started));
            g_audio.simple_flush(g_audio.stream, &error);
            atomic_store(&g_audio.write_started_ms, audio_now_ms());
        }
    }
    return NULL;
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
        if (g_audio.simple_latency && getenv("BEER_AUDIO_DIAGNOSTICS")) {
            /* Latency 0 right before a write means the sound server had run out of data. */
            static unsigned writes, underruns;
            int lerr = 0;
            const uint64_t latency_us = g_audio.simple_latency(g_audio.stream, &lerr);
            ++writes;
            if (writes % 50 == 0) fprintf(stderr, "[AUDIO] write #%u latency=%llu us queued=%zu played=%llu\n", writes, (unsigned long long)latency_us, g_audio.queued_bytes, (unsigned long long)atomic_load(&g_audio.bytes_played));
            if (latency_us < 2000) {
                ++underruns;
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                fprintf(stderr, "[AUDIO] underrun #%u at t=%lld.%03lds after %u writes (server buffer %llu us, queued %zu bytes)\n", underruns,
                        (long long)now.tv_sec, now.tv_nsec / 1000000, writes, (unsigned long long)latency_us, g_audio.queued_bytes);
            }
        }
        /* Write in ~20 ms pieces: a single pa_simple_write larger than the stream's maxlength (Bink hands over
         * ~600 ms at once) can stall against the server's buffer target and never return. */
        int write_ok = !g_audio.failed;
        atomic_store(&g_audio.write_started_ms, audio_now_ms());
        const size_t piece = (size_t)g_audio.piece_bytes ? (size_t)g_audio.piece_bytes : 4096;
        for (size_t done = 0; write_ok && done < packet->size; ) {
            size_t n = packet->size - done < piece ? packet->size - done : piece;
            const uint64_t t_before = audio_now_ms();
            if (g_audio.null_sink) {
                /* A real device consumes PCM at its sample rate; pace the producer the same way so the game's audio
                 * threads see normal timing (and callbacks complete) instead of an instant, endless drain. */
                usleep((useconds_t)((uint64_t)n * 1000000u / (g_audio.bytes_per_second ? g_audio.bytes_per_second : 1)));
            } else if (g_audio.simple_write(g_audio.stream, (const uint8_t *)packet->data + done, n, &error) != 0) {
                write_ok = 0;
            }
            if (getenv("BEER_AUDIO_DIAGNOSTICS") && audio_now_ms() - t_before > 100)
                fprintf(stderr, "[AUDIO] slow write: %zu bytes took %llu ms (queued %zu)\n", n, (unsigned long long)(audio_now_ms() - t_before), g_audio.queued_bytes);
            done += n;
            atomic_store(&g_audio.write_started_ms, audio_now_ms());
        }
        atomic_store(&g_audio.write_started_ms, 0);
        if (!g_audio.failed && write_ok) {
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
    const char *force_null = getenv("BEER_AUDIO_NULL");
    if (!(force_null && *force_null && strcmp(force_null, "0")))
        g_audio.library = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!g_audio.library) {
        /* No sound server (Android has no PulseAudio): keep the game's audio path working with a silent sink. */
        fprintf(stderr, "[AUDIO] no Pulse/PipeWire library (%s): using a silent timed sink\n",
                force_null && *force_null ? "BEER_AUDIO_NULL" : dlerror());
        g_audio.null_sink = 1;
        g_audio.bytes_per_second = (uint64_t)rate * channels * sizeof(int16_t);
        pthread_mutex_init(&g_audio.mutex, NULL);
        pthread_cond_init(&g_audio.condition, NULL);
        g_audio.running = 1;
        g_audio.max_queued_bytes = (size_t)(g_audio.bytes_per_second * 2u / 5u);
        g_audio.piece_bytes = (size_t)(g_audio.bytes_per_second / 50u);
        g_audio.piece_bytes -= g_audio.piece_bytes % (channels * sizeof(int16_t));
        g_audio.initialized = 1;
        if (pthread_create(&g_audio.thread, NULL, audio_thread_main, NULL) != 0) {
            g_audio.running = 0;
            g_audio.failed = 1;
            return 0;
        }
        return 1;
    }
#define LOAD(field, symbol) do { *(void **)(&g_audio.field) = dlsym(g_audio.library, symbol); \
    if (!g_audio.field) goto load_failed; } while (0)
    LOAD(simple_new, "pa_simple_new");
    LOAD(simple_write, "pa_simple_write");
    LOAD(simple_drain, "pa_simple_drain");
    LOAD(simple_flush, "pa_simple_flush");
    LOAD(simple_free, "pa_simple_free");
#undef LOAD
    *(void **)(&g_audio.simple_latency) = dlsym(g_audio.library, "pa_simple_get_latency");
    pa_sample_spec spec = { PA_SAMPLE_S16LE, rate, (uint8_t)channels };
    uint32_t bytes_per_second = rate * channels * (uint32_t)sizeof(int16_t);
    pa_buffer_attr attr = {
        .maxlength = bytes_per_second * 2u / 5u, /* 400 ms hard ceiling */
        .tlength = bytes_per_second * 7u / 50u, /* target about 140 ms: headroom for a stalled mixer thread */
        .prebuf = 0,                        /* start immediately; an underrun must not re-enter prebuffering and stall */
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
    g_audio.max_queued_bytes = (size_t)rate * channels * sizeof(int16_t) * 2u / 5u;
    g_audio.piece_bytes = (size_t)rate * channels * sizeof(int16_t) / 50u;   /* 20 ms */
    g_audio.piece_bytes -= g_audio.piece_bytes % (channels * sizeof(int16_t));
    g_audio.initialized = 1;
    if (pthread_create(&g_audio.thread, NULL, audio_thread_main, NULL) != 0) {
        g_audio.running = 0;
        goto load_failed;
    }
    g_audio.have_watchdog = pthread_create(&g_audio.watchdog, NULL, audio_watchdog_main, NULL) == 0;
    g_audio.bytes_per_second = bytes_per_second;
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

static int audio_submit(const void *pcm, size_t bytes, BeerAudioDoneFn done, void *opaque, int block)
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
    while (block && g_audio.running && !g_audio.failed && g_audio.max_queued_bytes &&
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

int beer_audio_submit(const void *pcm, size_t bytes, BeerAudioDoneFn done, void *opaque)
{
    return audio_submit(pcm, bytes, done, opaque, 1);
}

/* For DirectSound ring buffers: Unlock never blocks on real hardware, and the game's audio thread often holds
 * game-wide locks while it calls it. The ring size already bounds how far ahead the game can write. */
int beer_audio_submit_nowait(const void *pcm, size_t bytes)
{
    return audio_submit(pcm, bytes, NULL, NULL, 0);
}

void beer_audio_reset(void)
{
    if (!g_audio.initialized || (!g_audio.stream && !g_audio.null_sink)) return;
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
    if (g_audio.simple_flush) g_audio.simple_flush(g_audio.stream, &error);
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
        if (g_audio.have_watchdog) pthread_join(g_audio.watchdog, NULL);
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
