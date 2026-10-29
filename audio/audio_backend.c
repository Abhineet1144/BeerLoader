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
    pthread_mutex_t lock;
    int tried;
    void *library;
    PaSimpleNewFn simple_new;
    PaSimpleWriteFn simple_write;
    PaSimpleDrainFn simple_drain;
    PaSimpleFlushFn simple_flush;
    PaSimpleFreeFn simple_free;
    PaSimpleLatencyFn simple_latency;   /* optional: underrun diagnostics */
} g_pulse = { .lock = PTHREAD_MUTEX_INITIALIZER };

struct BeerAudioStream {
    pa_simple *stream;
    char name[32];
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    AudioPacket *head;
    AudioPacket *tail;
    size_t queued_bytes;
    size_t max_queued_bytes;
    size_t piece_bytes;
    uint32_t bytes_per_second;
    atomic_uint_fast64_t bytes_played;
    atomic_uint_fast64_t write_started_ms;   /* 0 while no pa_simple_write is in flight */
    pthread_t watchdog;
    int running;
    int failed;
};

static int pulse_load(void)
{
    pthread_mutex_lock(&g_pulse.lock);
    if (!g_pulse.tried) {
        g_pulse.tried = 1;
        g_pulse.library = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!g_pulse.library) {
            fprintf(stderr, "[AUDIO] libpulse-simple.so.0 unavailable: %s\n", dlerror());
        } else {
            *(void **)(&g_pulse.simple_new) = dlsym(g_pulse.library, "pa_simple_new");
            *(void **)(&g_pulse.simple_write) = dlsym(g_pulse.library, "pa_simple_write");
            *(void **)(&g_pulse.simple_drain) = dlsym(g_pulse.library, "pa_simple_drain");
            *(void **)(&g_pulse.simple_flush) = dlsym(g_pulse.library, "pa_simple_flush");
            *(void **)(&g_pulse.simple_free) = dlsym(g_pulse.library, "pa_simple_free");
            *(void **)(&g_pulse.simple_latency) = dlsym(g_pulse.library, "pa_simple_get_latency");
            if (!g_pulse.simple_new || !g_pulse.simple_write || !g_pulse.simple_drain || !g_pulse.simple_flush ||
                !g_pulse.simple_free) {
                fprintf(stderr, "[AUDIO] libpulse-simple.so.0 lacks the pa_simple API\n");
                dlclose(g_pulse.library);
                g_pulse.library = NULL;
            }
        }
    }
    pthread_mutex_unlock(&g_pulse.lock);
    return g_pulse.library != NULL;
}

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
static void *audio_watchdog_main(void *opaque)
{
    BeerAudioStream *s = opaque;
    while (s->running) {
        usleep(250000);
        const uint64_t started = atomic_load(&s->write_started_ms);
        if (getenv("BEER_AUDIO_DIAGNOSTICS")) {
            static unsigned tick;
            if (++tick % 8 == 0)
                fprintf(stderr, "[AUDIO] %s watch: write age=%llu ms played=%llu queued=%zu\n", s->name,
                        (unsigned long long)(started ? audio_now_ms() - started : 0),
                        (unsigned long long)atomic_load(&s->bytes_played), s->queued_bytes);
        }
        if (started && audio_now_ms() - started > 1000) {
            int error = 0;
            fprintf(stderr, "[AUDIO] %s: sound server stopped taking data for %llu ms; flushing the stream\n",
                    s->name, (unsigned long long)(audio_now_ms() - started));
            g_pulse.simple_flush(s->stream, &error);
            atomic_store(&s->write_started_ms, audio_now_ms());
        }
    }
    return NULL;
}

static void *audio_thread_main(void *opaque)
{
    BeerAudioStream *s = opaque;
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        while (s->running && !s->head)
            pthread_cond_wait(&s->condition, &s->mutex);
        if (!s->running && !s->head) {
            pthread_mutex_unlock(&s->mutex);
            break;
        }
        AudioPacket *packet = s->head;
        s->head = packet->next;
        if (!s->head) s->tail = NULL;
        if (s->queued_bytes >= packet->size) s->queued_bytes -= packet->size;
        else s->queued_bytes = 0;
        pthread_cond_broadcast(&s->condition);
        pthread_mutex_unlock(&s->mutex);

        int error = 0;
        if (g_pulse.simple_latency && getenv("BEER_AUDIO_DIAGNOSTICS")) {
            /* Latency 0 right before a write means the sound server had run out of data. */
            static unsigned writes, underruns;
            int lerr = 0;
            const uint64_t latency_us = g_pulse.simple_latency(s->stream, &lerr);
            ++writes;
            if (writes % 50 == 0) fprintf(stderr, "[AUDIO] %s write #%u latency=%llu us queued=%zu played=%llu\n", s->name, writes, (unsigned long long)latency_us, s->queued_bytes, (unsigned long long)atomic_load(&s->bytes_played));
            if (latency_us < 2000) {
                ++underruns;
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                fprintf(stderr, "[AUDIO] %s underrun #%u at t=%lld.%03lds after %u writes (server buffer %llu us, queued %zu bytes)\n", s->name, underruns,
                        (long long)now.tv_sec, now.tv_nsec / 1000000, writes, (unsigned long long)latency_us, s->queued_bytes);
            }
        }
        /* Write in ~20 ms pieces: a single pa_simple_write larger than the stream's maxlength (Bink hands over
         * ~600 ms at once) can stall against the server's buffer target and never return. */
        int write_ok = !s->failed;
        atomic_store(&s->write_started_ms, audio_now_ms());
        const size_t piece = s->piece_bytes ? s->piece_bytes : 4096;
        for (size_t done = 0; write_ok && done < packet->size; ) {
            size_t n = packet->size - done < piece ? packet->size - done : piece;
            const uint64_t t_before = audio_now_ms();
            if (g_pulse.simple_write(s->stream, (const uint8_t *)packet->data + done, n, &error) != 0) write_ok = 0;
            if (getenv("BEER_AUDIO_DIAGNOSTICS") && audio_now_ms() - t_before > 100)
                fprintf(stderr, "[AUDIO] %s slow write: %zu bytes took %llu ms (queued %zu)\n", s->name, n, (unsigned long long)(audio_now_ms() - t_before), s->queued_bytes);
            done += n;
            atomic_store(&s->write_started_ms, audio_now_ms());
        }
        atomic_store(&s->write_started_ms, 0);
        if (!s->failed && write_ok) {
            atomic_fetch_add_explicit(&s->bytes_played, packet->size, memory_order_relaxed);
        } else if (!s->failed) {
            fprintf(stderr, "[AUDIO] %s: Pulse write failed (error=%d); disabling output\n", s->name, error);
            s->failed = 1;
        }
        audio_finish_packet(packet);
    }
    return NULL;
}

BeerAudioStream *beer_audio_stream_open(unsigned rate, unsigned channels, const char *name)
{
    if (!rate || !channels || !pulse_load()) return NULL;
    BeerAudioStream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    snprintf(s->name, sizeof(s->name), "%s", name && *name ? name : "audio");
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
    s->stream = g_pulse.simple_new(NULL, "Beer", PA_STREAM_PLAYBACK, NULL, s->name, &spec, NULL, &attr, &error);
    if (!s->stream) {
        fprintf(stderr, "[AUDIO] cannot open PipeWire/Pulse stream '%s' (error=%d)\n", s->name, error);
        free(s);
        return NULL;
    }
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->condition, NULL);
    s->running = 1;
    s->bytes_per_second = bytes_per_second;
    /* Bound copied PCM to roughly 400 ms. This keeps the mixer thread
     * decoupled from PipeWire without allowing unbounded latency/memory growth. */
    s->max_queued_bytes = (size_t)rate * channels * sizeof(int16_t) * 2u / 5u;
    s->piece_bytes = (size_t)rate * channels * sizeof(int16_t) / 50u;   /* 20 ms */
    s->piece_bytes -= s->piece_bytes % (channels * sizeof(int16_t));
    if (pthread_create(&s->thread, NULL, audio_thread_main, s) != 0) {
        g_pulse.simple_free(s->stream);
        free(s);
        return NULL;
    }
    pthread_create(&s->watchdog, NULL, audio_watchdog_main, s);
    fprintf(stderr, "[AUDIO] PipeWire/Pulse stream '%s' opened: %u Hz, %u channels, S16LE\n", s->name, rate, channels);
    return s;
}

int beer_audio_stream_submit(BeerAudioStream *s, const void *pcm, size_t bytes, int block,
                             BeerAudioDoneFn done, void *opaque)
{
    if (!s || !pcm || !bytes || s->failed || !s->running)
        return 0;
    AudioPacket *packet = calloc(1, sizeof(*packet));
    if (!packet) return 0;
    packet->data = malloc(bytes);
    if (!packet->data) { free(packet); return 0; }
    memcpy(packet->data, pcm, bytes);
    packet->size = bytes;
    packet->done = done;
    packet->opaque = opaque;
    pthread_mutex_lock(&s->mutex);
    /* Always admit one packet into an empty queue, even when a legacy API
     * submits a block larger than the latency target. Otherwise that producer
     * waits forever for queued_bytes to shrink below zero. Subsequent packets
     * still observe the configured back-pressure limit. */
    while (block && s->running && !s->failed && s->max_queued_bytes &&
           s->queued_bytes != 0 &&
           s->queued_bytes + bytes > s->max_queued_bytes)
        pthread_cond_wait(&s->condition, &s->mutex);
    if (!s->running || s->failed) {
        pthread_mutex_unlock(&s->mutex);
        audio_finish_packet(packet);
        return 0;
    }
    if (s->tail) s->tail->next = packet;
    else s->head = packet;
    s->tail = packet;
    s->queued_bytes += bytes;
    pthread_cond_signal(&s->condition);
    pthread_mutex_unlock(&s->mutex);
    return 1;
}

void beer_audio_stream_reset(BeerAudioStream *s)
{
    if (!s || !s->stream) return;
    pthread_mutex_lock(&s->mutex);
    AudioPacket *packet = s->head;
    s->head = s->tail = NULL;
    s->queued_bytes = 0;
    pthread_cond_broadcast(&s->condition);
    pthread_mutex_unlock(&s->mutex);
    while (packet) {
        AudioPacket *next = packet->next;
        audio_finish_packet(packet);
        packet = next;
    }
    int error = 0;
    g_pulse.simple_flush(s->stream, &error);
}

uint64_t beer_audio_stream_bytes_played(BeerAudioStream *s)
{
    return s ? atomic_load_explicit(&s->bytes_played, memory_order_relaxed) : 0;
}

uint64_t beer_audio_stream_bytes_heard(BeerAudioStream *s, int *exact)
{
    if (exact) *exact = 0;
    if (!s) return 0;
    const uint64_t handed = atomic_load_explicit(&s->bytes_played, memory_order_relaxed);
    if (!g_pulse.simple_latency) return handed;
    int error = 0;
    const uint64_t latency_us = g_pulse.simple_latency(s->stream, &error);
    if (latency_us == (uint64_t)-1) return handed;
    uint64_t pending = latency_us * s->bytes_per_second / 1000000u;
    const uint32_t frame = 4;   /* keep the result frame-aligned for 16-bit stereo and mono pairs */
    pending -= pending % frame;
    if (exact) *exact = 1;
    return handed > pending ? handed - pending : 0;
}

void beer_audio_stream_close(BeerAudioStream *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->mutex);
    s->running = 0;
    pthread_cond_broadcast(&s->condition);
    pthread_mutex_unlock(&s->mutex);
    pthread_join(s->thread, NULL);
    pthread_join(s->watchdog, NULL);
    if (s->stream) {
        int error = 0;
        g_pulse.simple_drain(s->stream, &error);
        g_pulse.simple_free(s->stream);
    }
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->condition);
    free(s);
}

/* ---- the default stream (waveOut) ---- */
static BeerAudioStream *g_default;
static int g_default_failed;

int beer_audio_init(unsigned rate, unsigned channels)
{
    if (g_default) return 1;
    if (g_default_failed) return 0;
    g_default = beer_audio_stream_open(rate, channels, "waveOut");
    if (!g_default) g_default_failed = 1;
    return g_default != NULL;
}

int beer_audio_submit(const void *pcm, size_t bytes, BeerAudioDoneFn done, void *opaque)
{
    return beer_audio_stream_submit(g_default, pcm, bytes, 1, done, opaque);
}

int beer_audio_submit_nowait(const void *pcm, size_t bytes)
{
    return beer_audio_stream_submit(g_default, pcm, bytes, 0, NULL, NULL);
}

void beer_audio_reset(void)
{
    beer_audio_stream_reset(g_default);
}

void beer_audio_shutdown(void)
{
    BeerAudioStream *s = g_default;
    g_default = NULL;
    g_default_failed = 0;
    beer_audio_stream_close(s);
}

int beer_audio_available(void)
{
    return g_default != NULL;
}

uint64_t beer_audio_bytes_played(void)
{
    return beer_audio_stream_bytes_played(g_default);
}
