/* Steam voice chat: microphone capture, Opus encoding into Steam's voice packet format, and decoding.
 *
 * Packet layout (the Steam client's, as documented by public reverse engineering of its voice data; received
 * packets from real Steam clients decode here and ours decode there):
 *   u64  sender SteamID
 *   chunks:
 *     0x0B u16 sample rate                       (this module sends 24000)
 *     0x06 u16 length, then frames               (Opus with packet-loss concealment)
 *          frame: u16 frame length (0xFFFF = decoder reset), u16 sequence number, Opus data
 *     0x00 u16 silent sample count               (accepted on receive)
 *   u32  CRC32 (zlib polynomial) of everything before it
 * All integers little-endian. Frames are 20 ms (480 samples at 24 kHz), mono.
 *
 * Capture runs while the game records (StartVoiceRecording .. StopVoiceRecording) on a thread reading
 * libpulse-simple; a simple energy gate drops silence, as Steam sends nothing while nobody speaks. Each sender's
 * stream keeps its own Opus decoder so lost frames are concealed. Portable C: no loader dependency. */

#include "beer_steam_voice.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_SAMPLES 480u                       /* 20 ms at 24 kHz */
#define RING_SAMPLES (BEER_VOICE_SAMPLE_RATE * 2) /* 2 s of unread capture */
#define MAX_FRAMES_PER_PACKET 25u                /* 500 ms */
#define MAX_OPUS_FRAME 1275
#define PACKET_CAPACITY (8 + 3 + 3 + MAX_FRAMES_PER_PACKET * (4 + MAX_OPUS_FRAME) + 4)

/* ---- libopus / libpulse-simple, loaded at run time (no development headers needed) ---- */
typedef struct OpusEncoder OpusEncoder;
typedef struct OpusDecoder OpusDecoder;
static OpusEncoder *(*p_opus_encoder_create)(int32_t, int, int, int *);
static int (*p_opus_encode)(OpusEncoder *, const int16_t *, int, unsigned char *, int32_t);
static int (*p_opus_encoder_ctl)(OpusEncoder *, int, ...);
static OpusDecoder *(*p_opus_decoder_create)(int32_t, int, int *);
static int (*p_opus_decode)(OpusDecoder *, const unsigned char *, int32_t, int16_t *, int, int);
static int (*p_opus_decoder_ctl)(OpusDecoder *, int, ...);
static void (*p_opus_decoder_destroy)(OpusDecoder *);
#define OPUS_APPLICATION_VOIP 2048
#define OPUS_SET_BITRATE_REQUEST 4002
#define OPUS_RESET_STATE 4028

typedef struct pa_simple pa_simple;
typedef struct { int format; uint32_t rate; uint8_t channels; } PaSampleSpec;
typedef struct { uint32_t maxlength, tlength, prebuf, minreq, fragsize; } PaBufferAttr;
static pa_simple *(*p_pa_simple_new)(const char *, const char *, int, const char *, const char *, const PaSampleSpec *,
                                     const void *, const PaBufferAttr *, int *);
static int (*p_pa_simple_read)(pa_simple *, void *, size_t, int *);
static void (*p_pa_simple_free)(pa_simple *);
#define PA_STREAM_RECORD 2
#define PA_SAMPLE_S16LE 3

static pthread_once_t g_libs_once = PTHREAD_ONCE_INIT;
static int g_opus_ok, g_pulse_ok;

static void load_libraries(void)
{
    void *opus = dlopen("libopus.so.0", RTLD_NOW | RTLD_LOCAL);
#define SYM(lib, var, name) (*(void **)(&(var)) = (lib) ? dlsym((lib), (name)) : NULL)
    g_opus_ok = opus && SYM(opus, p_opus_encoder_create, "opus_encoder_create") && SYM(opus, p_opus_encode, "opus_encode") &&
                SYM(opus, p_opus_encoder_ctl, "opus_encoder_ctl") &&
                SYM(opus, p_opus_decoder_create, "opus_decoder_create") && SYM(opus, p_opus_decode, "opus_decode") &&
                SYM(opus, p_opus_decoder_ctl, "opus_decoder_ctl") &&
                SYM(opus, p_opus_decoder_destroy, "opus_decoder_destroy");
    void *pulse = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
    g_pulse_ok = pulse && SYM(pulse, p_pa_simple_new, "pa_simple_new") && SYM(pulse, p_pa_simple_read, "pa_simple_read") &&
                 SYM(pulse, p_pa_simple_free, "pa_simple_free");
#undef SYM
    fprintf(stderr, "[VOICE] Opus %s, microphone capture %s\n", g_opus_ok ? "available" : "missing (libopus.so.0)",
            g_pulse_ok ? "available" : "missing (libpulse-simple.so.0)");
}

/* ---- CRC32 (zlib) ---- */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffffu); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

/* ---- capture ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t g_ring[RING_SAMPLES];
static uint32_t g_ring_start, g_ring_count;
static int g_recording;        /* the game wants voice */
static int g_thread_running;
static pthread_t g_thread;
static OpusEncoder *g_encoder;
static uint16_t g_sequence;
static int g_send_reset = 1;   /* first frame of a stream: tell receivers to reset their decoder */
static uint32_t g_hangover;    /* frames still sent after speech stops */
static uint8_t g_pending[PACKET_CAPACITY];
static uint32_t g_pending_size;

static void ring_push(const int16_t *pcm, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        if (g_ring_count == RING_SAMPLES) { g_ring_start = (g_ring_start + 1) % RING_SAMPLES; --g_ring_count; }
        g_ring[(g_ring_start + g_ring_count) % RING_SAMPLES] = pcm[i];
        ++g_ring_count;
    }
}

static void *capture_thread(void *arg)
{
    (void)arg;
    const PaSampleSpec spec = { PA_SAMPLE_S16LE, BEER_VOICE_SAMPLE_RATE, 1 };
    const PaBufferAttr attr = { (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, FRAME_SAMPLES * 2 };
    int error = 0;
    pa_simple *stream = p_pa_simple_new(NULL, "Beer", PA_STREAM_RECORD, NULL, "Steam voice chat", &spec, NULL, &attr,
                                        &error);
    if (!stream) fprintf(stderr, "[VOICE] microphone could not be opened (pulse error %d)\n", error);
    else fprintf(stderr, "[VOICE] microphone recording\n");
    int16_t frame[FRAME_SAMPLES];
    for (;;) {
        pthread_mutex_lock(&g_lock);
        const int keep = g_recording;
        pthread_mutex_unlock(&g_lock);
        if (!keep || !stream) break;
        if (p_pa_simple_read(stream, frame, sizeof(frame), &error) < 0) {
            fprintf(stderr, "[VOICE] microphone read failed (pulse error %d)\n", error);
            break;
        }
        pthread_mutex_lock(&g_lock);
        ring_push(frame, FRAME_SAMPLES);
        pthread_mutex_unlock(&g_lock);
    }
    if (stream) p_pa_simple_free(stream);
    pthread_mutex_lock(&g_lock);
    g_thread_running = 0;
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[VOICE] microphone stopped\n");
    return NULL;
}

void beer_steam_voice_start(void)
{
    pthread_once(&g_libs_once, load_libraries);
    pthread_mutex_lock(&g_lock);
    g_recording = 1;
    if (g_pulse_ok && g_opus_ok && !g_thread_running) {
        if (g_thread_running == 0 && !pthread_create(&g_thread, NULL, capture_thread, NULL)) {
            pthread_detach(g_thread);
            g_thread_running = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void beer_steam_voice_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_recording = 0;           /* the capture thread ends after its current 20 ms read; buffered voice stays readable */
    pthread_mutex_unlock(&g_lock);
}

/* Energy gate: speech above about -50 dBFS opens it; it stays open 400 ms after the last loud frame. */
static int frame_is_voiced(const int16_t *pcm)
{
    /* Integer only (platform code is built without SSE): mean square against 100^2. */
    uint64_t sum = 0;
    for (uint32_t i = 0; i < FRAME_SAMPLES; ++i) sum += (uint64_t)((int32_t)pcm[i] * pcm[i]);
    if (sum > (uint64_t)100 * 100 * FRAME_SAMPLES) { g_hangover = 20; return 1; }
    if (g_hangover) { --g_hangover; return 1; }
    return 0;
}

/* Encodes up to MAX_FRAMES_PER_PACKET frames of `pcm` (or the capture ring when pcm is NULL) into `out`. Returns the
 * packet size, 0 when there is nothing to send. Caller holds g_lock. */
static uint32_t build_packet(uint64_t steamid, const int16_t *pcm, uint32_t samples, uint8_t *out)
{
    if (!g_encoder) {
        int error = 0;
        g_encoder = p_opus_encoder_create(BEER_VOICE_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
        if (!g_encoder) return 0;
        p_opus_encoder_ctl(g_encoder, OPUS_SET_BITRATE_REQUEST, 32000);
    }
    uint8_t *p = out;
    for (int i = 0; i < 8; ++i) *p++ = (uint8_t)(steamid >> (8 * i));
    *p++ = 0x0B;
    put16(p, BEER_VOICE_SAMPLE_RATE);
    p += 2;
    *p++ = 0x06;
    uint8_t *length_field = p;
    p += 2;
    uint8_t *const frames_start = p;
    uint32_t frames = 0;
    int16_t frame[FRAME_SAMPLES];
    while (frames < MAX_FRAMES_PER_PACKET) {
        if (pcm) {
            if (samples < FRAME_SAMPLES) break;
            memcpy(frame, pcm, sizeof(frame));
            pcm += FRAME_SAMPLES;
            samples -= FRAME_SAMPLES;
        } else {
            if (g_ring_count < FRAME_SAMPLES) break;
            for (uint32_t i = 0; i < FRAME_SAMPLES; ++i) frame[i] = g_ring[(g_ring_start + i) % RING_SAMPLES];
            g_ring_start = (g_ring_start + FRAME_SAMPLES) % RING_SAMPLES;
            g_ring_count -= FRAME_SAMPLES;
            if (!frame_is_voiced(frame)) { g_send_reset = 1; continue; }   /* silence is not sent */
        }
        if (g_send_reset) {    /* a new talk spurt: receivers start a fresh decoder */
            put16(p, 0xFFFFu);
            put16(p + 2, g_sequence);
            p += 4;
            g_send_reset = 0;
        }
        const int n = p_opus_encode(g_encoder, frame, FRAME_SAMPLES, p + 4, MAX_OPUS_FRAME);
        if (n <= 0) continue;
        put16(p, (uint32_t)n);
        put16(p + 2, g_sequence++);
        p += 4 + n;
        ++frames;
    }
    if (!frames) return 0;
    put16(length_field, (uint32_t)(p - frames_start));
    put32(p, crc32_update(0, out, (size_t)(p - out)));
    p += 4;
    return (uint32_t)(p - out);
}

int beer_steam_voice_available(uint64_t steamid, uint32_t *compressed_bytes)
{
    pthread_once(&g_libs_once, load_libraries);
    if (compressed_bytes) *compressed_bytes = 0;
    pthread_mutex_lock(&g_lock);
    if (!g_pending_size && g_opus_ok) g_pending_size = build_packet(steamid, NULL, 0, g_pending);
    const uint32_t size = g_pending_size;
    const int recording = g_recording;
    pthread_mutex_unlock(&g_lock);
    if (compressed_bytes) *compressed_bytes = size;
    if (size) return BEER_VOICE_OK;
    return recording ? BEER_VOICE_NO_DATA : BEER_VOICE_NOT_RECORDING;
}

int beer_steam_voice_get(uint64_t steamid, void *dest, uint32_t dest_size, uint32_t *written)
{
    uint32_t size = 0;
    const int result = beer_steam_voice_available(steamid, &size);
    if (written) *written = 0;
    if (result != BEER_VOICE_OK) return result;
    if (!dest || dest_size < size) return BEER_VOICE_BUFFER_TOO_SMALL;
    pthread_mutex_lock(&g_lock);
    memcpy(dest, g_pending, g_pending_size);
    if (written) *written = g_pending_size;
    g_pending_size = 0;
    pthread_mutex_unlock(&g_lock);
    return BEER_VOICE_OK;
}

int beer_steam_voice_encode_for_test(uint64_t steamid, const int16_t *pcm, uint32_t samples, void *dest,
                                     uint32_t dest_size, uint32_t *written)
{
    pthread_once(&g_libs_once, load_libraries);
    if (!g_opus_ok) return BEER_VOICE_NOT_INITIALIZED;
    static uint8_t packet[PACKET_CAPACITY];
    pthread_mutex_lock(&g_lock);
    const uint32_t size = build_packet(steamid, pcm, samples, packet);
    pthread_mutex_unlock(&g_lock);
    if (!size) return BEER_VOICE_NO_DATA;
    if (dest_size < size) return BEER_VOICE_BUFFER_TOO_SMALL;
    memcpy(dest, packet, size);
    *written = size;
    return BEER_VOICE_OK;
}

/* ---- decoding ---- */
typedef struct { uint64_t steamid; OpusDecoder *decoder; uint32_t rate; uint16_t next_sequence; int have_sequence; } Sender;
static Sender g_senders[16];
static uint32_t g_sender_turn;

static Sender *sender_for(uint64_t steamid, uint32_t rate)
{
    for (size_t i = 0; i < sizeof(g_senders) / sizeof(*g_senders); ++i)
        if (g_senders[i].decoder && g_senders[i].steamid == steamid && g_senders[i].rate == rate) return &g_senders[i];
    Sender *s = &g_senders[g_sender_turn++ % (sizeof(g_senders) / sizeof(*g_senders))];   /* oldest slot */
    if (s->decoder) p_opus_decoder_destroy(s->decoder);
    int error = 0;
    memset(s, 0, sizeof(*s));
    s->decoder = p_opus_decoder_create((int32_t)rate, 1, &error);
    if (!s->decoder) return NULL;
    s->steamid = steamid;
    s->rate = rate;
    return s;
}

/* Linear resampling of 16-bit mono from `from` Hz to `to` Hz. */
static uint32_t resample(const int16_t *in, uint32_t n, uint32_t from, uint32_t to, int16_t *out, uint32_t cap)
{
    if (from == to) {
        if (n > cap) n = cap;
        memcpy(out, in, n * sizeof(int16_t));
        return n;
    }
    const uint64_t produced = (uint64_t)n * to / from;
    uint32_t m = produced > cap ? cap : (uint32_t)produced;
    for (uint32_t i = 0; i < m; ++i) {   /* 16.16 fixed point: platform code is built without SSE */
        const uint64_t position = ((uint64_t)i * from << 16) / to;
        const uint32_t j = (uint32_t)(position >> 16), t = (uint32_t)(position & 0xffffu);
        const int32_t a = in[j < n ? j : n - 1], b = in[j + 1 < n ? j + 1 : n - 1];
        out[i] = (int16_t)(a + (((b - a) * (int32_t)t) >> 16));
    }
    return m;
}

int beer_steam_voice_decompress(const void *packet, uint32_t size, void *dest, uint32_t dest_size, uint32_t *written,
                                uint32_t rate)
{
    pthread_once(&g_libs_once, load_libraries);
    if (written) *written = 0;
    if (!g_opus_ok) return BEER_VOICE_NOT_INITIALIZED;
    const uint8_t *p = packet;
    if (!p || size < 8 + 4) return BEER_VOICE_DATA_CORRUPTED;
    if (rate < 11025 || rate > 48000) rate = BEER_VOICE_SAMPLE_RATE;
    if (crc32_update(0, p, size - 4) != get32(p + size - 4)) return BEER_VOICE_DATA_CORRUPTED;
    uint64_t steamid = 0;
    for (int i = 0; i < 8; ++i) steamid |= (uint64_t)p[i] << (8 * i);
    const uint8_t *cursor = p + 8, *const end = p + size - 4;
    uint32_t stream_rate = BEER_VOICE_SAMPLE_RATE;
    /* Decoded PCM at the stream rate, then resampled into dest. */
    static int16_t pcm[48000];
    uint32_t total = 0;
    pthread_mutex_lock(&g_lock);
    int result = BEER_VOICE_OK;
    while (cursor < end && result == BEER_VOICE_OK) {
        const uint8_t type = *cursor++;
        if (type == 0x0B) {                      /* sample rate */
            if (end - cursor < 2) { result = BEER_VOICE_DATA_CORRUPTED; break; }
            stream_rate = get16(cursor);
            cursor += 2;
            if (stream_rate != 8000 && stream_rate != 12000 && stream_rate != 16000 && stream_rate != 24000 &&
                stream_rate != 48000) { result = BEER_VOICE_DATA_CORRUPTED; break; }
        } else if (type == 0x00) {               /* silence: n samples */
            if (end - cursor < 2) { result = BEER_VOICE_DATA_CORRUPTED; break; }
            uint32_t n = get16(cursor);
            cursor += 2;
            if (n > sizeof(pcm) / sizeof(*pcm) - total) n = (uint32_t)(sizeof(pcm) / sizeof(*pcm)) - total;
            memset(pcm + total, 0, n * sizeof(int16_t));
            total += n;
        } else if (type == 0x06) {               /* Opus with packet-loss concealment */
            if (end - cursor < 2) { result = BEER_VOICE_DATA_CORRUPTED; break; }
            const uint32_t length = get16(cursor);
            cursor += 2;
            if ((uint32_t)(end - cursor) < length) { result = BEER_VOICE_DATA_CORRUPTED; break; }
            const uint8_t *frame = cursor, *const frames_end = cursor + length;
            cursor = frames_end;
            Sender *sender = sender_for(steamid, stream_rate);
            if (!sender) { result = BEER_VOICE_NOT_INITIALIZED; break; }
            const uint32_t frame_samples = stream_rate / 50;
            while (frame + 4 <= frames_end) {
                const uint32_t frame_length = get16(frame), sequence = get16(frame + 2);
                frame += 4;
                if (frame_length == 0xFFFFu) {   /* reset */
                    p_opus_decoder_ctl(sender->decoder, OPUS_RESET_STATE);
                    sender->next_sequence = (uint16_t)sequence;
                    sender->have_sequence = 1;
                    continue;
                }
                if (frame + frame_length > frames_end) { result = BEER_VOICE_DATA_CORRUPTED; break; }
                if (sender->have_sequence) {     /* conceal up to 10 lost frames */
                    const uint16_t lost = (uint16_t)(sequence - sender->next_sequence);
                    for (uint32_t k = 0; lost < 10 && k < lost; ++k) {
                        if (total + frame_samples > sizeof(pcm) / sizeof(*pcm)) break;
                        const int n = p_opus_decode(sender->decoder, NULL, 0, pcm + total, (int)frame_samples, 0);
                        if (n > 0) total += (uint32_t)n;
                    }
                }
                sender->next_sequence = (uint16_t)(sequence + 1);
                sender->have_sequence = 1;
                if (total + 2880 > sizeof(pcm) / sizeof(*pcm)) break;   /* room for a 60 ms frame at 48 kHz */
                const int n = p_opus_decode(sender->decoder, frame, (int32_t)frame_length, pcm + total, 2880, 0);
                if (n > 0) total += (uint32_t)n;
                frame += frame_length;
            }
        } else {                                 /* SILK and other legacy codecs: not supported */
            result = BEER_VOICE_DATA_CORRUPTED;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (result != BEER_VOICE_OK) return result;
    const uint64_t needed = (uint64_t)total * rate / stream_rate * sizeof(int16_t);
    if (!dest || dest_size < needed) {
        if (written) *written = (uint32_t)needed;
        return BEER_VOICE_BUFFER_TOO_SMALL;
    }
    const uint32_t produced = resample(pcm, total, stream_rate, rate, dest, dest_size / sizeof(int16_t));
    if (written) *written = produced * (uint32_t)sizeof(int16_t);
    return BEER_VOICE_OK;
}
