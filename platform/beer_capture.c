#define _GNU_SOURCE
#include "beer_capture.h"
#include "beer_control.h"
#include "beer_time.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define AUDIO_RATE 48000u
#define AUDIO_RINGS 8
#define AUDIO_CAP 96000u                /* frames per ring: 2 s */
/* The mixer runs this far behind real time: jitter room for the taps, which arrive once per sound-server period (up
 * to ~43 ms). Kept under ffmpeg's 100 ms scheduling tolerance: an input further ahead of the other (here the video) is
 * held back, and the readback buffers would back up behind it. */
#define AUDIO_LAG_NS 80000000ull
#define AUDIO_RESYNC 4800u              /* a tap more than 100 ms off its expected place is moved there */
#define AUDIO_AHEAD 9600u
#define QUEUE_SIZE 8

enum { IDLE, RUNNING, FINISHING };

typedef struct {
    int slot;
    const uint8_t *rgba;
    uint64_t due;
    BeerCaptureRelease release;
} Frame;

typedef struct {
    const void *key;
    int used;
    uint64_t start, end;          /* timeline sample positions held: [start, end) */
    uint64_t last_ns;
    unsigned rate;
    uint32_t frac;                /* resampler phase (16.16, relative to prev) */
    int16_t prev[2];
    int16_t buf[AUDIO_CAP * 2];
} Ring;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int phase;
    int stopping;
    int audio_stop;               /* set by the video thread once the video input is closed */
    uint64_t stop_ns;
    char game[96];

    /* settings */
    int stream;                   /* 0: record to a file, 1: RTMP */
    int save_copy;                /* streaming: also write the file */
    int game_time;
    int audio;
    int fps;
    int height;                   /* 0: the game's own */
    int bitrate;                  /* kbit/s */
    int audio_bitrate;
    char encoder[32];
    char container[8];
    char ffmpeg[256];
    char url[1024];
    char path[1024];

    /* timeline */
    int started;
    uint32_t w, h;
    uint64_t start_ns;            /* real (or game) clock at timeline 0 */
    uint64_t audio_start_ns;      /* real clock at timeline 0 */
    uint64_t planned;             /* frames committed by the post pass */
    uint64_t written;             /* frames written to ffmpeg (repeats included) */
    uint64_t received, dropped, busy;

    Frame queue[QUEUE_SIZE];
    int q_head, q_count;

    pid_t pid;
    int video_fd, audio_fd;
    pthread_t video_thread, audio_thread;
    int audio_thread_started;
    char log_path[512];
    char error[400];
    uint64_t audio_written;
} C = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .video_fd = -1, .audio_fd = -1 };

static atomic_int g_active;       /* RUNNING and not stopping: the presenter routes frames through the post pass */
static atomic_int g_audio_on;     /* the tap feeds the rings */
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static Ring rings[AUDIO_RINGS];

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t timeline_ns(void)
{
    return C.game_time ? beer_time_monotonic_ns() : mono_ns();
}

int beer_capture_active(void)
{
    return atomic_load_explicit(&g_active, memory_order_relaxed);
}

void beer_capture_init(const char *exe_path)
{
    const char *base = exe_path ? strrchr(exe_path, '/') : NULL;
    const char *name = base ? base + 1 : exe_path ? exe_path : "game";
    size_t k = 0;
    for (const char *c = name; *c && *c != '.' && k + 1 < sizeof(C.game); ++c) {
        const char ch = *c;
        const int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        C.game[k++] = ok ? ch : '_';
    }
    C.game[k] = 0;
    if (!k) snprintf(C.game, sizeof(C.game), "game");
}

/* ------------------------------------------------------------------ post pass side */

int beer_capture_poll(uint32_t src_w, uint32_t src_h, uint32_t *out_w, uint32_t *out_h, uint64_t *due)
{
    if (!beer_capture_active() || src_w < 16 || src_h < 16) return 0;
    pthread_mutex_lock(&C.lock);
    int want = 0;
    if (C.phase == RUNNING && !C.stopping) {
        const uint64_t now = timeline_ns();
        if (!C.started) {
            uint32_t h = C.height > 0 && (uint32_t)C.height < src_h ? (uint32_t)C.height : src_h;
            uint32_t w = (uint32_t)((uint64_t)src_w * h / src_h);
            C.w = w & ~1u;
            C.h = h & ~1u;
            C.start_ns = now;
            C.audio_start_ns = mono_ns();
            C.started = 1;
        }
        const uint64_t index = (now > C.start_ns ? (now - C.start_ns) * (uint64_t)C.fps / 1000000000ull : 0) + 1;
        if (index > C.planned) {
            *out_w = C.w;
            *out_h = C.h;
            *due = index;
            want = 1;
        }
    }
    pthread_mutex_unlock(&C.lock);
    return want;
}

void beer_capture_commit(uint64_t due)
{
    pthread_mutex_lock(&C.lock);
    if (due > C.planned) C.planned = due;
    pthread_mutex_unlock(&C.lock);
}

void beer_capture_note_busy(void)
{
    pthread_mutex_lock(&C.lock);
    C.busy++;
    pthread_mutex_unlock(&C.lock);
}

int beer_capture_push(int slot, const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t due, BeerCaptureRelease release)
{
    pthread_mutex_lock(&C.lock);
    const int take = C.phase == RUNNING && !C.stopping && w == C.w && h == C.h && C.q_count < QUEUE_SIZE;
    if (take) {
        Frame *f = &C.queue[(C.q_head + C.q_count) % QUEUE_SIZE];
        f->slot = slot;
        f->rgba = rgba;
        f->due = due;
        f->release = release;
        C.q_count++;
        C.received++;
        pthread_cond_broadcast(&C.cond);
    } else if (C.phase == RUNNING) {
        C.dropped++;
    }
    pthread_mutex_unlock(&C.lock);
    if (!take) release(slot);
    return take;
}

/* ------------------------------------------------------------------ audio */

static int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* One input frame of any layout -> stereo. */
static void to_stereo(const int16_t *in, unsigned channels, int16_t out[2])
{
    if (channels == 1) { out[0] = out[1] = in[0]; return; }
    int32_t l = in[0], r = in[1];
    if (channels >= 6) {   /* 5.1 / 7.1: FL FR FC LFE SL SR [BL BR] */
        const int32_t c = in[2] * 181 / 256;
        l += c + in[4] * 181 / 256;
        r += c + in[5] * 181 / 256;
        if (channels >= 8) { l += in[6] * 181 / 256; r += in[7] * 181 / 256; }
    }
    out[0] = clamp16(l);
    out[1] = clamp16(r);
}

static Ring *ring_for(const void *key, unsigned rate, uint64_t now)
{
    Ring *free_ring = NULL;
    for (int i = 0; i < AUDIO_RINGS; ++i) {
        if (rings[i].used && rings[i].key == key) return &rings[i];
        if (!rings[i].used && !free_ring) free_ring = &rings[i];
    }
    if (!free_ring) {   /* reuse the stalest one */
        for (int i = 0; i < AUDIO_RINGS; ++i)
            if (!free_ring || rings[i].last_ns < free_ring->last_ns) free_ring = &rings[i];
    }
    Ring *r = free_ring;
    r->key = key;
    r->used = 1;
    r->start = r->end = 0;
    r->last_ns = now;
    r->rate = rate;
    r->frac = 0;
    r->prev[0] = r->prev[1] = 0;
    return r;
}

static void ring_put(Ring *r, const int16_t *stereo, size_t n)
{
    if (n > AUDIO_CAP) { stereo += (n - AUDIO_CAP) * 2; r->end += n - AUDIO_CAP; n = AUDIO_CAP; }
    for (size_t i = 0; i < n; ++i) {
        const size_t at = (size_t)((r->end + i) % AUDIO_CAP) * 2;
        r->buf[at] = stereo[i * 2];
        r->buf[at + 1] = stereo[i * 2 + 1];
    }
    r->end += n;
    if (r->end - r->start > AUDIO_CAP) r->start = r->end - AUDIO_CAP;
}

void beer_capture_audio(const void *stream, const int16_t *pcm, size_t frames, unsigned rate, unsigned channels)
{
    if (!atomic_load_explicit(&g_audio_on, memory_order_relaxed) || !pcm || !frames || !rate || !channels || rate > 384000u) return;
    const uint64_t now = mono_ns();
    pthread_mutex_lock(&C.lock);
    const int started = C.started;
    const uint64_t start = C.audio_start_ns;
    pthread_mutex_unlock(&C.lock);
    if (!started || now < start) return;
    const uint64_t expected = (now - start) * AUDIO_RATE / 1000000000ull;

    pthread_mutex_lock(&audio_lock);
    Ring *r = ring_for(stream, rate, now);
    if (r->rate != rate) { r->rate = rate; r->frac = 0; }
    if (r->end == 0 && r->start == 0) r->start = r->end = expected;
    if (r->end + AUDIO_RESYNC < expected) {          /* the source was quiet: leave a gap of silence */
        const uint64_t gap = expected - r->end;
        const uint64_t fill = gap > AUDIO_CAP ? AUDIO_CAP : gap;
        for (uint64_t i = 0; i < fill; ++i) {
            const size_t at = (size_t)((expected - fill + i) % AUDIO_CAP) * 2;
            r->buf[at] = r->buf[at + 1] = 0;
        }
        r->end = expected;
        if (r->end - r->start > AUDIO_CAP) r->start = r->end - AUDIO_CAP;
    } else if (r->end > expected + AUDIO_AHEAD) {     /* ran ahead of the clock: snap back */
        r->end = expected;
        if (r->start > r->end) r->start = r->end;
    }
    if (r->end < r->start) r->end = r->start;
    r->last_ns = now;

    int16_t out[2048 * 2];
    size_t used = 0;
    if (rate == AUDIO_RATE) {
        for (size_t i = 0; i < frames; ++i) {
            to_stereo(pcm + i * channels, channels, out + used * 2);
            if (++used == 2048) { ring_put(r, out, used); used = 0; }
        }
    } else {
        /* Linear interpolation over [prev, x0, x1, ...]; the phase is 16.16 relative to prev. */
        const uint32_t step = (uint32_t)(((uint64_t)rate << 16) / AUDIO_RATE);
        uint64_t p = r->frac;
        int16_t a[2], b[2];
        while ((p >> 16) < frames) {
            const size_t i = (size_t)(p >> 16);
            if (i == 0) { a[0] = r->prev[0]; a[1] = r->prev[1]; }
            else to_stereo(pcm + (i - 1) * channels, channels, a);
            to_stereo(pcm + i * channels, channels, b);
            const int32_t f = (int32_t)(p & 0xffff);
            out[used * 2] = (int16_t)(a[0] + (((int32_t)b[0] - a[0]) * f >> 16));
            out[used * 2 + 1] = (int16_t)(a[1] + (((int32_t)b[1] - a[1]) * f >> 16));
            if (++used == 2048) { ring_put(r, out, used); used = 0; }
            p += step;
        }
        to_stereo(pcm + (frames - 1) * channels, channels, r->prev);
        r->frac = (uint32_t)(p - ((uint64_t)frames << 16));
    }
    if (used) ring_put(r, out, used);
    pthread_mutex_unlock(&audio_lock);
}

/* Mixes timeline samples [pos, pos+n) of every ring into out (stereo S16). */
static void mix(uint64_t pos, size_t n, int16_t *out)
{
    static int32_t acc[1024 * 2];
    memset(acc, 0, n * 2 * sizeof(int32_t));
    const uint64_t now = mono_ns();
    pthread_mutex_lock(&audio_lock);
    for (int k = 0; k < AUDIO_RINGS; ++k) {
        Ring *r = &rings[k];
        if (!r->used) continue;
        const uint64_t from = pos > r->start ? pos : r->start;
        const uint64_t to = pos + n < r->end ? pos + n : r->end;
        for (uint64_t p = from; p < to; ++p) {
            const size_t at = (size_t)(p % AUDIO_CAP) * 2;
            acc[(p - pos) * 2] += r->buf[at];
            acc[(p - pos) * 2 + 1] += r->buf[at + 1];
        }
        if (r->start < pos + n) r->start = pos + n;
        if (r->end < r->start && now - r->last_ns > 3000000000ull) r->used = 0;   /* the stream went away */
    }
    pthread_mutex_unlock(&audio_lock);
    for (size_t i = 0; i < n * 2; ++i) out[i] = clamp16(acc[i]);
}

/* Sends never block for good: the sockets have a send timeout, and after a stop a send gives up once ffmpeg has had
 * STOP_GRACE_NS to take the rest (a stalled network, or ffmpeg waiting on the other input). */
#define STOP_GRACE_NS 8000000000ull
static int give_up(int audio)
{
    pthread_mutex_lock(&C.lock);
    const int r = (audio && C.audio_stop) || (C.stopping && mono_ns() > C.stop_ns + STOP_GRACE_NS);
    pthread_mutex_unlock(&C.lock);
    return r;
}

static int send_all(int fd, const void *data, size_t bytes, int audio)
{
    const uint8_t *p = data;
    while (bytes) {
        const ssize_t sent = send(fd, p, bytes, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && !give_up(audio)) continue;
            return 0;
        }
        p += sent;
        bytes -= (size_t)sent;
    }
    return 1;
}

static void set_error_locked(const char *text);

static void *audio_main(void *opaque)
{
    (void)opaque;
    static int16_t chunk[1024 * 2];
    for (;;) {
        usleep(10000);
        pthread_mutex_lock(&C.lock);
        const int stop = C.audio_stop;
        const int fd = C.audio_fd;
        const uint64_t start = C.audio_start_ns;
        const int started = C.started && C.pid > 0;
        pthread_mutex_unlock(&C.lock);
        if (stop) break;
        if (!started || fd < 0) continue;
        const uint64_t now = mono_ns();
        if (now < start + AUDIO_LAG_NS) continue;
        const uint64_t target = (now - start - AUDIO_LAG_NS) * AUDIO_RATE / 1000000000ull;
        int failed = 0;
        while (C.audio_written < target) {
            const uint64_t left = target - C.audio_written;
            const size_t n = left > 1024 ? 1024 : (size_t)left;
            mix(C.audio_written, n, chunk);
            if (!send_all(fd, chunk, n * 4, 1)) { failed = 1; break; }
            C.audio_written += n;
        }
        if (failed) {
            pthread_mutex_lock(&C.lock);
            if (!C.audio_stop) set_error_locked("ffmpeg stopped reading the audio");
            pthread_mutex_unlock(&C.lock);
            break;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ ffmpeg */

typedef struct {
    char *argv[96];
    int argc;
    char pool[8192];
    size_t used;
} Args;

static void arg(Args *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void arg(Args *a, const char *fmt, ...)
{
    if (a->argc >= (int)(sizeof(a->argv) / sizeof(a->argv[0])) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(a->pool + a->used, sizeof(a->pool) - a->used, fmt, ap);
    va_end(ap);
    if (n < 0 || a->used + (size_t)n + 1 > sizeof(a->pool)) return;
    a->argv[a->argc++] = a->pool + a->used;
    a->argv[a->argc] = NULL;
    a->used += (size_t)n + 1;
}

/* Tee muxer slave names escape \ | [ ]. */
static void tee_escape(const char *in, char *out, size_t size)
{
    size_t k = 0;
    for (; *in && k + 2 < size; ++in) {
        if (*in == '\\' || *in == '|' || *in == '[' || *in == ']') out[k++] = '\\';
        out[k++] = *in;
    }
    out[k] = 0;
}

static int family(const char *suffix) { const size_t a = strlen(C.encoder), b = strlen(suffix); return a > b && !strcmp(C.encoder + a - b, suffix); }

static void build_args(Args *a)
{
    const int audio = C.audio && !C.game_time;
    arg(a, "%s", C.ffmpeg);
    arg(a, "-hide_banner"); arg(a, "-nostdin"); arg(a, "-loglevel"); arg(a, "warning"); arg(a, "-y");
    if (family("_vaapi")) { arg(a, "-vaapi_device"); arg(a, "/dev/dri/renderD128"); }
    /* Both inputs are fully described raw streams: no probing. Probing the real-time audio input would read ~5 s of it
     * before ffmpeg starts taking video, and the video socket fills up meanwhile. */
    arg(a, "-probesize"); arg(a, "32"); arg(a, "-analyzeduration"); arg(a, "0");
    arg(a, "-thread_queue_size"); arg(a, "64");
    arg(a, "-f"); arg(a, "rawvideo"); arg(a, "-pix_fmt"); arg(a, "rgba");
    arg(a, "-video_size"); arg(a, "%ux%u", C.w, C.h);
    arg(a, "-framerate"); arg(a, "%d", C.fps);
    arg(a, "-i"); arg(a, "pipe:0");
    if (audio) {
        arg(a, "-probesize"); arg(a, "32"); arg(a, "-analyzeduration"); arg(a, "0");
        arg(a, "-thread_queue_size"); arg(a, "1024");
        arg(a, "-f"); arg(a, "s16le"); arg(a, "-ar"); arg(a, "%u", AUDIO_RATE); arg(a, "-ac"); arg(a, "2");
        arg(a, "-i"); arg(a, "pipe:3");
    }
    arg(a, "-map"); arg(a, "0:v:0");
    if (audio) { arg(a, "-map"); arg(a, "1:a:0"); }
    if (family("_vaapi")) { arg(a, "-vf"); arg(a, "format=nv12,hwupload"); }
    else { arg(a, "-pix_fmt"); arg(a, "yuv420p"); }
    arg(a, "-c:v"); arg(a, "%s", C.encoder);
    if (family("_nvenc")) {
        arg(a, "-preset"); arg(a, C.stream ? "p4" : "p5");
        arg(a, "-tune"); arg(a, C.stream ? "ll" : "hq");
        arg(a, "-rc"); arg(a, C.stream ? "cbr" : "vbr");
    } else if (family("_vaapi")) {
        arg(a, "-rc_mode"); arg(a, C.stream ? "CBR" : "VBR");
    } else if (!strcmp(C.encoder, "libx264") || !strcmp(C.encoder, "libx265")) {
        arg(a, "-preset"); arg(a, "veryfast");
    } else if (!strcmp(C.encoder, "libsvtav1")) {
        arg(a, "-preset"); arg(a, "10");
    }
    arg(a, "-b:v"); arg(a, "%dk", C.bitrate);
    if (strcmp(C.encoder, "libsvtav1")) {   /* SVT-AV1 takes a max rate only in CRF mode */
        arg(a, "-maxrate"); arg(a, "%dk", C.stream ? C.bitrate : C.bitrate * 3 / 2);
        arg(a, "-bufsize"); arg(a, "%dk", C.bitrate * 2);
    }
    arg(a, "-g"); arg(a, "%d", C.fps * 2);
    if (audio) { arg(a, "-c:a"); arg(a, "aac"); arg(a, "-b:a"); arg(a, "%dk", C.audio_bitrate); }

    const int mp4 = strcmp(C.container, "mkv") != 0;
    if (C.stream && C.save_copy) {
        char url[1100], path[1100];
        tee_escape(C.url, url, sizeof(url));
        tee_escape(C.path, path, sizeof(path));
        arg(a, "-flags"); arg(a, "+global_header");
        arg(a, "-f"); arg(a, "tee");
        arg(a, "[f=flv:onfail=abort]%s|[f=%s%s:onfail=ignore]%s", url, mp4 ? "mp4" : "matroska",
            mp4 ? ":movflags=+frag_keyframe+empty_moov+default_base_moof" : "", path);
    } else if (C.stream) {
        arg(a, "-f"); arg(a, "flv"); arg(a, "%s", C.url);
    } else {
        if (mp4) { arg(a, "-movflags"); arg(a, "+frag_keyframe+empty_moov+default_base_moof"); arg(a, "-f"); arg(a, "mp4"); }
        else { arg(a, "-f"); arg(a, "matroska"); }
        arg(a, "%s", C.path);
    }
}

/* Last non-empty line of the ffmpeg log (the reason it stopped, usually). */
static void log_tail(char *out, size_t size)
{
    out[0] = 0;
    FILE *f = fopen(C.log_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (n) snprintf(out, size, "%s", line);
    }
    fclose(f);
}

static void set_error_locked(const char *text)
{
    if (!C.error[0]) {
        char tail[300];
        log_tail(tail, sizeof(tail));
        if (tail[0]) snprintf(C.error, sizeof(C.error), "%s: %s", text, tail);
        else snprintf(C.error, sizeof(C.error), "%s", text);
        fprintf(stderr, "[CAPTURE] %s\n", C.error);
    }
    if (!C.stopping) C.stop_ns = mono_ns();
    C.stopping = 1;
    atomic_store(&g_active, 0);
    atomic_store(&g_audio_on, 0);
    pthread_cond_broadcast(&C.cond);
}

static int spawn_ffmpeg(void)
{
    int video[2], audio[2] = { -1, -1 };
    const int want_audio = C.audio && !C.game_time;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, video) != 0) return 0;
    if (want_audio && socketpair(AF_UNIX, SOCK_STREAM, 0, audio) != 0) { close(video[0]); close(video[1]); return 0; }
    int sndbuf = 8 << 20;
    setsockopt(video[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    const struct timeval timeout = { 0, 500000 };
    setsockopt(video[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (want_audio) setsockopt(audio[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    shutdown(video[1], SHUT_WR);
    if (want_audio) shutdown(audio[1], SHUT_WR);
    fcntl(video[0], F_SETFD, FD_CLOEXEC);
    if (want_audio) fcntl(audio[0], F_SETFD, FD_CLOEXEC);

    Args a = {0};
    build_args(&a);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, video[1], 0);
    if (want_audio) posix_spawn_file_actions_adddup2(&actions, audio[1], 3);
    posix_spawn_file_actions_addopen(&actions, 2, C.log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&actions, 2, 1);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&actions, want_audio ? 4 : 3);
#endif
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE); sigaddset(&defaults, SIGINT); sigaddset(&defaults, SIGTERM); sigaddset(&defaults, SIGCHLD);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);   /* a Ctrl+C meant for the game leaves ffmpeg to finish the file */
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);

    /* The game's preloads have no business in ffmpeg. */
    size_t count = 0;
    while (environ && environ[count]) ++count;
    char **env = calloc(count + 1, sizeof(char *));
    size_t k = 0;
    for (size_t i = 0; env && i < count; ++i)
        if (strncmp(environ[i], "LD_PRELOAD=", 11)) env[k++] = environ[i];

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, C.ffmpeg, &actions, &attr, a.argv, env ? env : environ);
    free(env);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(video[1]);
    if (want_audio) close(audio[1]);
    if (rc != 0) {
        close(video[0]);
        if (want_audio) close(audio[0]);
        snprintf(C.error, sizeof(C.error), "cannot start %s: %s", C.ffmpeg, strerror(rc));
        fprintf(stderr, "[CAPTURE] %s\n", C.error);
        return 0;
    }
    pthread_mutex_lock(&C.lock);
    C.pid = pid;
    C.video_fd = video[0];
    C.audio_fd = want_audio ? audio[0] : -1;
    pthread_mutex_unlock(&C.lock);
    fprintf(stderr, "[CAPTURE] ffmpeg %d started:", (int)pid);
    for (int i = 0; i < a.argc; ++i) {
        const int secret = C.stream && i > 0 && (strstr(a.argv[i], "rtmp://") || strstr(a.argv[i], "rtmps://"));
        fprintf(stderr, " %s", secret ? "<stream url>" : a.argv[i]);
    }
    fprintf(stderr, "\n");
    return 1;
}

static void wait_ffmpeg(void)
{
    if (C.pid <= 0) return;
    int status = 0;
    for (int i = 0; i < 400; ++i) {   /* up to 20 s to finish the file / close the stream */
        const pid_t r = waitpid(C.pid, &status, WNOHANG);
        if (r == C.pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                pthread_mutex_lock(&C.lock);
                if (!C.error[0]) {
                    char tail[300];
                    log_tail(tail, sizeof(tail));
                    snprintf(C.error, sizeof(C.error), "ffmpeg exited with %d%s%s", WEXITSTATUS(status), tail[0] ? ": " : "", tail);
                }
                pthread_mutex_unlock(&C.lock);
            }
            C.pid = 0;
            return;
        }
        if (r < 0) { C.pid = 0; return; }   /* reaped elsewhere */
        if (i == 300) kill(C.pid, SIGINT);
        usleep(50000);
    }
    kill(C.pid, SIGKILL);
    waitpid(C.pid, &status, 0);
    C.pid = 0;
}

static void *video_main(void *opaque)
{
    (void)opaque;
    for (;;) {
        pthread_mutex_lock(&C.lock);
        while (!C.q_count && !C.stopping) pthread_cond_wait(&C.cond, &C.lock);
        if (!C.q_count) { pthread_mutex_unlock(&C.lock); break; }
        Frame f = C.queue[C.q_head];
        C.q_head = (C.q_head + 1) % QUEUE_SIZE;
        C.q_count--;
        const int stopping = C.stopping;
        pthread_mutex_unlock(&C.lock);

        if (stopping) { f.release(f.slot); continue; }   /* drain */
        if (C.pid <= 0 && !C.error[0]) {
            if (!spawn_ffmpeg()) {
                pthread_mutex_lock(&C.lock);
                set_error_locked(C.error[0] ? C.error : "cannot start ffmpeg");
                pthread_mutex_unlock(&C.lock);
                f.release(f.slot);
                continue;
            }
        }
        /* Constant frame rate: the frame fills every timeline slot since the last one (a slow game repeats frames). */
        uint64_t repeat = f.due > C.written ? f.due - C.written : 0;
        const uint64_t cap = (uint64_t)C.fps * 10;
        if (repeat > cap) repeat = cap;
        const size_t bytes = (size_t)C.w * C.h * 4;
        int ok = 1;
        for (uint64_t i = 0; ok && i < repeat; ++i) ok = send_all(C.video_fd, f.rgba, bytes, 0);
        f.release(f.slot);
        pthread_mutex_lock(&C.lock);
        if (ok && f.due > C.written) C.written = f.due;   /* past the 10 s cap the gap is skipped */
        else set_error_locked(C.stream ? "the stream stopped" : "ffmpeg stopped");
        pthread_mutex_unlock(&C.lock);
    }

    /* Finish: EOF to ffmpeg, which then writes the end of the file. Audio keeps flowing until the video input is closed:
     * ffmpeg holds back an input that runs ahead of the other, so stopping the audio first would stall the last frames. */
    if (C.video_fd >= 0) shutdown(C.video_fd, SHUT_WR);
    pthread_mutex_lock(&C.lock);
    C.audio_stop = 1;
    pthread_mutex_unlock(&C.lock);
    if (C.audio_thread_started) { pthread_join(C.audio_thread, NULL); C.audio_thread_started = 0; }
    if (C.audio_fd >= 0) shutdown(C.audio_fd, SHUT_WR);
    pthread_mutex_lock(&C.lock);
    if (C.video_fd >= 0) { close(C.video_fd); C.video_fd = -1; }
    if (C.audio_fd >= 0) { close(C.audio_fd); C.audio_fd = -1; }
    pthread_mutex_unlock(&C.lock);
    wait_ffmpeg();
    pthread_mutex_lock(&audio_lock);
    for (int i = 0; i < AUDIO_RINGS; ++i) rings[i].used = 0;
    pthread_mutex_unlock(&audio_lock);
    pthread_mutex_lock(&C.lock);
    C.phase = IDLE;
    fprintf(stderr, "[CAPTURE] finished: %llu frames written (%llu received, %llu dropped, %llu late)%s%s\n",
            (unsigned long long)C.written, (unsigned long long)C.received, (unsigned long long)C.dropped,
            (unsigned long long)C.busy, C.error[0] ? "; " : "", C.error);
    pthread_cond_broadcast(&C.cond);
    pthread_mutex_unlock(&C.lock);
    return NULL;
}

/* ------------------------------------------------------------------ control */

static void json_escape(const char *in, char *out, size_t size)
{
    size_t k = 0;
    for (; *in && k + 7 < size; ++in) {
        const unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c < 0x20) k += (size_t)snprintf(out + k, size - k, "\\u%04x", c);
        else out[k++] = (char)c;
    }
    out[k] = 0;
}

static int mkdir_p(const char *dir)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(tmp, 0755);
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static int get_bool(const char *request, const char *key, int fallback)
{
    int32_t v;
    return beer_json_milli(request, key, &v) ? v != 0 : fallback;
}

static int get_int(const char *request, const char *key, int fallback, int lo, int hi)
{
    int32_t v;
    if (!beer_json_milli(request, key, &v)) return fallback;
    v /= 1000;
    return v < lo ? lo : v > hi ? hi : v;
}

static int encoder_allowed(const char *name)
{
    static const char *const ok[] = { "h264_nvenc", "hevc_nvenc", "av1_nvenc", "h264_vaapi", "hevc_vaapi", "av1_vaapi",
                                      "libx264", "libx265", "libsvtav1", NULL };
    for (int i = 0; ok[i]; ++i) if (!strcmp(name, ok[i])) return 1;
    return 0;
}

static const char *start(const char *request)
{
    if (C.phase != IDLE) return C.phase == RUNNING ? "already capturing" : "the last capture is still finishing";
    char mode[16] = "record";
    beer_json_string(request, "mode", mode, sizeof(mode));
    C.stream = !strcmp(mode, "stream");
    if (!C.stream && strcmp(mode, "record")) return "mode must be record or stream";
    C.save_copy = C.stream && get_bool(request, "save_copy", 0);
    C.game_time = get_bool(request, "game_time", 0);
    C.audio = get_bool(request, "audio", 1);
    C.fps = get_int(request, "fps", 60, 10, 240);
    C.height = get_int(request, "height", 0, 0, 4320);
    if (C.height && C.height < 144) C.height = 144;
    C.bitrate = get_int(request, "bitrate", C.stream ? 6000 : 20000, 300, 500000);
    C.audio_bitrate = get_int(request, "audio_bitrate", 160, 64, 512);
    snprintf(C.encoder, sizeof(C.encoder), "libx264");
    beer_json_string(request, "encoder", C.encoder, sizeof(C.encoder));
    if (!encoder_allowed(C.encoder)) return "unknown encoder";
    snprintf(C.container, sizeof(C.container), "mp4");
    beer_json_string(request, "container", C.container, sizeof(C.container));
    if (strcmp(C.container, "mp4") && strcmp(C.container, "mkv")) return "container must be mp4 or mkv";
    snprintf(C.ffmpeg, sizeof(C.ffmpeg), "ffmpeg");
    beer_json_string(request, "ffmpeg", C.ffmpeg, sizeof(C.ffmpeg));

    C.url[0] = 0;
    if (C.stream) {
        beer_json_string(request, "url", C.url, sizeof(C.url));
        if (strncmp(C.url, "rtmp://", 7) && strncmp(C.url, "rtmps://", 8)) return "the stream address must start with rtmp:// or rtmps://";
    }
    C.path[0] = 0;
    if (!C.stream || C.save_copy) {
        const char *home = getenv("HOME");
        char folder[700] = "";
        if (!beer_json_string(request, "folder", folder, sizeof(folder)) || !folder[0])
            snprintf(folder, sizeof(folder), "%s/Videos/Beer", home && *home ? home : "/tmp");
        if (!mkdir_p(folder)) return "cannot create the video folder";
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        snprintf(C.path, sizeof(C.path), "%s/%s-%04d%02d%02d-%02d%02d%02d.%s", folder, C.game, tm_now.tm_year + 1900,
                 tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, C.container);
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime) snprintf(C.log_path, sizeof(C.log_path), "%s/beer/capture-%d.log", runtime, (int)getpid());
    else snprintf(C.log_path, sizeof(C.log_path), "/tmp/beer-%d/capture-%d.log", (int)getuid(), (int)getpid());
    {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", C.log_path);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; mkdir_p(dir); }
    }

    C.stopping = 0;
    C.audio_stop = 0;
    C.stop_ns = 0;
    C.started = 0;
    C.planned = C.written = C.received = C.dropped = C.busy = C.audio_written = 0;
    C.q_head = C.q_count = 0;
    C.pid = 0;
    C.error[0] = 0;
    C.w = C.h = 0;
    C.phase = RUNNING;
    if (pthread_create(&C.video_thread, NULL, video_main, NULL) != 0) { C.phase = IDLE; return "cannot start the encoder thread"; }
    pthread_detach(C.video_thread);
    C.audio_thread_started = 0;
    if (C.audio && !C.game_time) {
        if (pthread_create(&C.audio_thread, NULL, audio_main, NULL) == 0) C.audio_thread_started = 1;
        atomic_store(&g_audio_on, 1);
    }
    atomic_store(&g_active, 1);
    fprintf(stderr, "[CAPTURE] %s started: %s %d kbit/s, %d fps, height %d, %s timeline, audio %s\n", C.stream ? "stream" : "recording",
            C.encoder, C.bitrate, C.fps, C.height, C.game_time ? "game" : "real", C.audio && !C.game_time ? "on" : "off");
    return NULL;
}

static void status_json(char *out, size_t size)
{
    static const char *const names[] = { "idle", "running", "finishing" };
    const int phase = C.phase == RUNNING && C.stopping ? FINISHING : C.phase;
    char path[1200], error[500];
    json_escape(C.path, path, sizeof(path));
    json_escape(C.error, error, sizeof(error));
    struct stat st;
    const unsigned long long bytes = C.path[0] && stat(C.path, &st) == 0 ? (unsigned long long)st.st_size : 0;
    const uint64_t ms = C.fps ? C.written * 1000u / (uint64_t)C.fps : 0;
    snprintf(out, size,
             "{\"ok\":true,\"state\":\"%s\",\"mode\":\"%s\",\"path\":\"%s\",\"error\":\"%s\",\"frames\":%llu,\"dropped\":%llu,"
             "\"late\":%llu,\"seconds\":%llu.%03llu,\"bytes\":%llu,\"width\":%u,\"height\":%u,\"fps\":%d,\"bitrate\":%d,"
             "\"encoder\":\"%s\",\"game_time\":%s,\"audio\":%s}\n",
             names[phase], C.stream ? "stream" : "record", path, error, (unsigned long long)C.written,
             (unsigned long long)C.dropped, (unsigned long long)C.busy, (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000),
             bytes, C.w, C.h, C.fps, C.bitrate, C.encoder, C.game_time ? "true" : "false",
             C.audio && !C.game_time ? "true" : "false");
}

void beer_capture_handle(const char *cmd, const char *request, char *out, size_t size)
{
    pthread_mutex_lock(&C.lock);
    if (!strcmp(cmd, "capture_start")) {
        const char *error = start(request);
        if (error) {
            char escaped[200];
            json_escape(error, escaped, sizeof(escaped));
            snprintf(out, size, "{\"ok\":false,\"error\":\"%s\"}\n", escaped);
        } else {
            status_json(out, size);
        }
    } else if (!strcmp(cmd, "capture_stop")) {
        if (C.phase == RUNNING && !C.stopping) {
            C.stop_ns = mono_ns();
            C.stopping = 1;
            atomic_store(&g_active, 0);
            atomic_store(&g_audio_on, 0);
            pthread_cond_broadcast(&C.cond);
            fprintf(stderr, "[CAPTURE] stop requested\n");
        }
        status_json(out, size);
    } else if (!strcmp(cmd, "capture_status")) {
        status_json(out, size);
    } else {
        snprintf(out, size, "{\"ok\":false,\"error\":\"unknown capture command\"}\n");
    }
    pthread_mutex_unlock(&C.lock);
}
