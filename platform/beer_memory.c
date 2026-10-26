#include "beer_memory.h"
#include "beer_control.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_CANDIDATES (4u * 1024u * 1024u)
#define PAGE_RESULTS 500u
#define MAX_FROZEN 64

typedef enum { T_I8, T_I16, T_I32, T_I64, T_F32, T_F64, T_STR, T_BYTES, T_COUNT } Type;
static const char *const type_names[T_COUNT] = { "i8", "i16", "i32", "i64", "f32", "f64", "str", "bytes" };
static const unsigned type_sizes[T_COUNT] = { 1, 2, 4, 8, 4, 8, 0, 0 };

static int mem_fd = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int open_mem(void)
{
    if (mem_fd < 0) mem_fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    return mem_fd >= 0;
}

static ssize_t read_mem(uint64_t addr, void *buf, size_t len)
{
    if (!open_mem()) return -1;
    return pread(mem_fd, buf, len, (off_t)addr);
}

static ssize_t write_mem(uint64_t addr, const void *buf, size_t len)
{
    if (!open_mem()) return -1;
    return pwrite(mem_fd, buf, len, (off_t)addr);
}

/* ---- value encoding ---- */

typedef struct { Type type; uint8_t bytes[256]; size_t length; } Value;

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex_bytes(const char *text, uint8_t *out, size_t cap, size_t *length)
{
    size_t n = 0;
    int high = -1;
    for (; *text; ++text) {
        if (isspace((unsigned char)*text)) continue;
        const int v = hex_value(*text);
        if (v < 0) return 0;
        if (high < 0) high = v;
        else { if (n >= cap) return 0; out[n++] = (uint8_t)(high << 4 | v); high = -1; }
    }
    *length = n;
    return high < 0 && n > 0;
}

static int type_from_name(const char *name, Type *type)
{
    for (int i = 0; i < T_COUNT; ++i)
        if (!strcmp(name, type_names[i])) { *type = (Type)i; return 1; }
    return 0;
}

static int parse_value(Type type, const char *text, Value *v)
{
    v->type = type;
    char *end = NULL;
    errno = 0;
    switch (type) {
    case T_I8: case T_I16: case T_I32: case T_I64: {
        const long long n = strtoll(text, &end, 0);
        if (end == text || *end || errno) return 0;
        v->length = type_sizes[type];
        memcpy(v->bytes, &n, v->length);   /* little endian */
        return 1;
    }
    case T_F32: {
        const float f = strtof(text, &end);
        if (end == text || *end) return 0;
        v->length = 4;
        memcpy(v->bytes, &f, 4);
        return 1;
    }
    case T_F64: {
        const double d = strtod(text, &end);
        if (end == text || *end) return 0;
        v->length = 8;
        memcpy(v->bytes, &d, 8);
        return 1;
    }
    case T_STR:
        v->length = strlen(text);
        if (!v->length || v->length > sizeof(v->bytes)) return 0;
        memcpy(v->bytes, text, v->length);
        return 1;
    case T_BYTES:
        return parse_hex_bytes(text, v->bytes, sizeof(v->bytes), &v->length);
    default:
        return 0;
    }
}

static void format_value(Type type, const uint8_t *bytes, size_t length, char *out, size_t size)
{
    switch (type) {
    case T_I8: snprintf(out, size, "%d", (int)(int8_t)bytes[0]); break;
    case T_I16: { int16_t v; memcpy(&v, bytes, 2); snprintf(out, size, "%d", (int)v); break; }
    case T_I32: { int32_t v; memcpy(&v, bytes, 4); snprintf(out, size, "%d", (int)v); break; }
    case T_I64: { int64_t v; memcpy(&v, bytes, 8); snprintf(out, size, "%lld", (long long)v); break; }
    case T_F32: { float v; memcpy(&v, bytes, 4); snprintf(out, size, "%.6g", (double)v); break; }
    case T_F64: { double v; memcpy(&v, bytes, 8); snprintf(out, size, "%.12g", v); break; }
    default: {
        size_t i = 0;
        for (; i < length && i * 2 + 3 < size; ++i) snprintf(out + i * 2, size - i * 2, "%02x", bytes[i]);
        out[i * 2] = 0;
    }
    }
}

/* Typed comparison of two stored values: <0, 0, >0. */
static int compare_values(Type type, const uint8_t *a, const uint8_t *b, size_t length)
{
    switch (type) {
    case T_I8: return (int8_t)a[0] < (int8_t)b[0] ? -1 : (int8_t)a[0] > (int8_t)b[0];
    case T_I16: { int16_t x, y; memcpy(&x, a, 2); memcpy(&y, b, 2); return x < y ? -1 : x > y; }
    case T_I32: { int32_t x, y; memcpy(&x, a, 4); memcpy(&y, b, 4); return x < y ? -1 : x > y; }
    case T_I64: { int64_t x, y; memcpy(&x, a, 8); memcpy(&y, b, 8); return x < y ? -1 : x > y; }
    case T_F32: { float x, y; memcpy(&x, a, 4); memcpy(&y, b, 4); return x < y ? -1 : x > y; }
    case T_F64: { double x, y; memcpy(&x, a, 8); memcpy(&y, b, 8); return x < y ? -1 : x > y; }
    default: return memcmp(a, b, length);
    }
}

/* Float "exact" match tolerates the rounding of the text the user typed. */
static int values_match(Type type, const uint8_t *found, const Value *wanted)
{
    if (type == T_F32) {
        float x, y;
        memcpy(&x, found, 4); memcpy(&y, wanted->bytes, 4);
        const float d = x > y ? x - y : y - x;
        return d <= 0.0005f * (y < 0 ? -y : y) + 1e-6f;
    }
    if (type == T_F64) {
        double x, y;
        memcpy(&x, found, 8); memcpy(&y, wanted->bytes, 8);
        const double d = x > y ? x - y : y - x;
        return d <= 0.0000005 * (y < 0 ? -y : y) + 1e-9;
    }
    return !memcmp(found, wanted->bytes, wanted->length);
}

/* ---- scan session ---- */

typedef struct { uint64_t addr; uint8_t value[8]; } Candidate;   /* previous value kept for changed/increased/... */
/* Unknown-initial-value scan: a copy of the memory plus one "still a candidate" bit per element. It
 * collapses into a normal candidate list once few enough elements are left. */
typedef struct { uint64_t start; size_t size; uint8_t *data; uint64_t *bits; size_t elements; int mapped; } Snap;
#define SNAPSHOT_LIMIT (12ull << 30)   /* snapshot copies live in an unlinked file, so RAM is not the limit */

/* Snapshot copies are backed by an unlinked temporary file (page cache, evictable) instead of anonymous memory,
 * so a snapshot of several GB does not compete with the game for RAM. Falls back to malloc. */
static int snap_dir_ready(char *dir, size_t size)
{
    const char *home = getenv("HOME");
    const char *override = getenv("BEER_SCAN_TMPDIR");
    if (override && *override) snprintf(dir, size, "%s", override);
    else snprintf(dir, size, "%s/.cache/beer", home && *home ? home : "/tmp");
    char parent[300];
    snprintf(parent, sizeof(parent), "%s", dir);
    char *slash = strrchr(parent, '/');
    if (slash && !override) { *slash = 0; mkdir(parent, 0700); }
    mkdir(dir, 0700);
    return 1;
}

static uint8_t *snap_map(size_t size)
{
    char dir[300], path[340];
    snap_dir_ready(dir, sizeof(dir));
    struct statvfs fs;
    if (statvfs(dir, &fs) != 0 || (uint64_t)fs.f_bavail * fs.f_frsize < size + (2ull << 30)) return NULL;
    snprintf(path, sizeof(path), "%s/scan-XXXXXX", dir);
    const int fd = mkstemp(path);
    if (fd < 0) return NULL;
    unlink(path);
    void *mapped = MAP_FAILED;
    if (ftruncate(fd, (off_t)size) == 0) mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return mapped == MAP_FAILED ? NULL : (uint8_t *)mapped;
}

static void snap_release(Snap *snap)
{
    if (snap->data) { if (snap->mapped) munmap(snap->data, snap->size); else free(snap->data); }
    free(snap->bits);
    snap->data = NULL; snap->bits = NULL;
}

static struct {
    int active;
    Type type;
    size_t length;
    Candidate *items;
    size_t count;
    Snap *snaps;
    size_t snap_count;
    uint64_t snap_total;     /* candidates still alive in the snapshot */
    int truncated;           /* snapshot hit SNAPSHOT_LIMIT */
} session;

static void session_reset(void)
{
    for (size_t i = 0; i < session.snap_count; ++i) snap_release(&session.snaps[i]);
    free(session.snaps);
    free(session.items);
    memset(&session, 0, sizeof(session));
}

typedef struct { uint64_t start, end; char perms[5]; char path[160]; } Region;

static size_t read_regions(Region *regions, size_t cap)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    size_t n = 0;
    char line[512];
    while (n < cap && fgets(line, sizeof(line), f)) {
        unsigned long long start, end;
        char perms[8] = "", path[256] = "";
        if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %255[^\n]", &start, &end, perms, path) < 3) continue;
        Region *r = &regions[n++];
        r->start = start; r->end = end;
        snprintf(r->perms, sizeof(r->perms), "%s", perms);
        const char *p = path;
        while (*p == ' ') ++p;
        snprintf(r->path, sizeof(r->path), "%s", p);
    }
    fclose(f);
    return n;
}

/* Why a region is (not) scanned: 0 = scan it, otherwise the skip reason. */
enum { SKIP_NONE, SKIP_UNREADABLE, SKIP_READONLY, SKIP_DEVICE, SKIP_HUGE };
#define REGION_HUGE_LIMIT (24ull << 30)

static int skip_reason(const Region *r, int writable_only)
{
    if (r->perms[0] != 'r') return SKIP_UNREADABLE;
    if (writable_only && r->perms[1] != 'w') return SKIP_READONLY;
    if (!strncmp(r->path, "/dev/", 5) || strstr(r->path, "nvidia") || strstr(r->path, "memfd:") ||
        !strcmp(r->path, "[vvar]") || !strcmp(r->path, "[vsyscall]") || !strcmp(r->path, "[vdso]")) return SKIP_DEVICE;
    if (r->end - r->start > REGION_HUGE_LIMIT) return SKIP_HUGE;
    return SKIP_NONE;
}

typedef struct { uint64_t scanned, readonly, device, huge, limit; } Coverage;
static Coverage coverage;

static void coverage_note(const Region *r, int reason)
{
    const uint64_t size = r->end - r->start;
    switch (reason) {
    case SKIP_NONE: coverage.scanned += size; break;
    case SKIP_READONLY: coverage.readonly += size; break;
    case SKIP_DEVICE: coverage.device += size; break;
    case SKIP_HUGE: coverage.huge += size; break;
    default: break;
    }
}

static int scannable(const Region *r, int writable_only)
{
    const int reason = skip_reason(r, writable_only);
    coverage_note(r, reason);
    return reason == SKIP_NONE;
}

static int add_candidate(const uint64_t addr, const uint8_t *value, size_t length)
{
    if (session.count >= MAX_CANDIDATES) return 0;
    if (session.count == 0 && !session.items) {
        session.items = malloc(sizeof(Candidate) * 65536u);
        if (!session.items) return 0;
    }
    /* Grow in powers of two. */
    if (session.count >= 65536u && (session.count & (session.count - 1)) == 0) {
        Candidate *grown = realloc(session.items, sizeof(Candidate) * session.count * 2);
        if (!grown) return 0;
        session.items = grown;
    }
    Candidate *c = &session.items[session.count++];
    c->addr = addr;
    memset(c->value, 0, sizeof(c->value));
    memcpy(c->value, value, length > 8 ? 8 : length);
    return 1;
}

/* Optional overall value filter: the (new) value must satisfy `cond` against `cond_value`. */
static int scan_has_cond;
static char scan_cond[3];
static double scan_cond_value;

static double typed_number(Type type, const uint8_t *b)
{
    switch (type) {
    case T_I8: return (double)(int8_t)b[0];
    case T_I16: { int16_t v; memcpy(&v, b, 2); return (double)v; }
    case T_I32: { int32_t v; memcpy(&v, b, 4); return (double)v; }
    case T_I64: { int64_t v; memcpy(&v, b, 8); return (double)v; }
    case T_F32: { float v; memcpy(&v, b, 4); return (double)v; }
    case T_F64: { double v; memcpy(&v, b, 8); return v; }
    default: return 0;
    }
}

static int cond_ok(Type type, const uint8_t *b)
{
    if (!scan_has_cond || !type_sizes[type]) return 1;
    const double v = typed_number(type, b), c = scan_cond_value;
    if (v != v) return 0;   /* NaN never satisfies a condition */
    if (!strcmp(scan_cond, "lt")) return v < c;
    if (!strcmp(scan_cond, "le")) return v <= c;
    if (!strcmp(scan_cond, "gt")) return v > c;
    if (!strcmp(scan_cond, "ge")) return v >= c;
    if (!strcmp(scan_cond, "eq")) return v == c;
    return 1;
}

static void first_scan(Type type, const Value *wanted, int writable_only)
{
    session_reset();
    memset(&coverage, 0, sizeof(coverage));
    session.active = 1;
    session.type = type;
    session.length = wanted->length;
    Region *regions = malloc(sizeof(Region) * 8192);
    if (!regions) return;
    const size_t region_count = read_regions(regions, 8192);
    const size_t chunk = 1u << 20, step = type_sizes[type] ? type_sizes[type] : 1;
    uint8_t *buffer = malloc(chunk + 512);
    if (!buffer) { free(regions); return; }
    for (size_t i = 0; i < region_count; ++i) {
        const Region *r = &regions[i];
        if (!scannable(r, writable_only)) continue;
        for (uint64_t at = r->start; at < r->end; at += chunk) {
            const size_t want = (size_t)((r->end - at) < chunk + wanted->length ? (r->end - at) : chunk + wanted->length);
            const ssize_t got = read_mem(at, buffer, want);
            if (got < (ssize_t)wanted->length) continue;
            const size_t limit = (size_t)got - wanted->length + 1;
            for (size_t k = 0; k < limit && k < chunk; k += step) {
                if (((at + k) % step) && type_sizes[type]) continue;   /* aligned for numeric types */
                if (values_match(type, buffer + k, wanted) && cond_ok(type, buffer + k)) {
                    if (!add_candidate(at + k, buffer + k, wanted->length)) goto done;
                }
            }
        }
    }
done:
    free(buffer);
    free(regions);
}

static double scan_max_step, scan_min_step;   /* optional "up to" / "at least" size of a change */
static int scan_has_max, scan_has_min;

static double typed_delta(Type type, const uint8_t *now, const uint8_t *old)
{
    switch (type) {
    case T_I8: return (double)(int8_t)now[0] - (double)(int8_t)old[0];
    case T_I16: { int16_t x, y; memcpy(&x, now, 2); memcpy(&y, old, 2); return (double)x - (double)y; }
    case T_I32: { int32_t x, y; memcpy(&x, now, 4); memcpy(&y, old, 4); return (double)x - (double)y; }
    case T_I64: { int64_t x, y; memcpy(&x, now, 8); memcpy(&y, old, 8); return (double)x - (double)y; }
    case T_F32: { float x, y; memcpy(&x, now, 4); memcpy(&y, old, 4); return (double)x - (double)y; }
    case T_F64: { double x, y; memcpy(&x, now, 8); memcpy(&y, old, 8); return x - y; }
    default: return 0;
    }
}

/* changed / increased / decreased, optionally only when the step is within the threshold. */
static int step_matches(Type type, const char *mode, const uint8_t *now, const uint8_t *old, size_t length)
{
    const int c = compare_values(type, now, old, length);
    if (c == 0) return !strcmp(mode, "unchanged");
    double delta = 0;
    const int bounded = (scan_has_max || scan_has_min) && type_sizes[type];
    if (bounded) delta = typed_delta(type, now, old);
    const double magnitude = delta < 0 ? -delta : delta;
    const int within = !bounded || ((!scan_has_max || magnitude <= scan_max_step) && (!scan_has_min || magnitude >= scan_min_step));
    if (!strcmp(mode, "changed")) return within;
    if (!strcmp(mode, "increased")) return c > 0 && within;
    if (!strcmp(mode, "decreased")) return c < 0 && within;
    return 0;
}

static void next_scan(const char *mode, const Value *wanted)
{
    size_t kept = 0;
    uint8_t now[256];
    for (size_t i = 0; i < session.count; ++i) {
        Candidate *c = &session.items[i];
        if (read_mem(c->addr, now, session.length) != (ssize_t)session.length) continue;
        int keep = 0;
        const size_t cmp_len = session.length > 8 ? 8 : session.length;
        if (!strcmp(mode, "exact")) keep = wanted && values_match(session.type, now, wanted);
        else keep = step_matches(session.type, mode, now, c->value, cmp_len);
        if (keep && !cond_ok(session.type, now)) keep = 0;
        if (!keep) continue;
        memcpy(c->value, now, cmp_len);
        session.items[kept++] = *c;
    }
    session.count = kept;
}

static void first_scan_all(Type type, int writable_only)
{
    session_reset();
    memset(&coverage, 0, sizeof(coverage));
    session.active = 1;
    session.type = type;
    session.length = type_sizes[type];
    const size_t step = type_sizes[type];
    Region *regions = malloc(sizeof(Region) * 8192);
    if (!regions) return;
    const size_t region_count = read_regions(regions, 8192);
    session.snaps = calloc(region_count ? region_count : 1, sizeof(Snap));
    uint64_t total_bytes = 0;
    for (size_t i = 0; i < region_count && session.snaps; ++i) {
        const Region *r = &regions[i];
        if (!scannable(r, writable_only)) continue;
        const size_t size = (size_t)(r->end - r->start) / step * step;
        if (!size) continue;
        if (total_bytes + size > SNAPSHOT_LIMIT) { session.truncated = 1; coverage.limit += size; continue; }
        Snap *snap = &session.snaps[session.snap_count];
        snap->data = snap_map(size);
        snap->mapped = snap->data != NULL;
        if (!snap->data) snap->data = malloc(size);
        snap->elements = size / step;
        snap->bits = calloc((snap->elements + 63) / 64, sizeof(uint64_t));
        if (!snap->data || !snap->bits) { snap_release(snap); memset(snap, 0, sizeof(*snap)); session.truncated = 1; coverage.limit += size; continue; }
        snap->start = r->start;
        snap->size = size;
        const size_t chunk = 1u << 20;
        for (size_t off = 0; off < size; off += chunk) {
            const size_t want = size - off < chunk ? size - off : chunk;
            if (read_mem(r->start + off, snap->data + off, want) != (ssize_t)want) continue;   /* bits stay clear */
            for (size_t e = off / step; e < (off + want) / step; ++e)
                if (cond_ok(type, snap->data + e * step)) snap->bits[e / 64] |= 1ull << (e % 64);
        }
        for (size_t w = 0; w < (snap->elements + 63) / 64; ++w) session.snap_total += (uint64_t)__builtin_popcountll(snap->bits[w]);
        total_bytes += size;
        ++session.snap_count;
    }
    free(regions);
}

static void collapse_snapshot(void)
{
    const size_t step = type_sizes[session.type];
    for (size_t i = 0; i < session.snap_count; ++i) {
        Snap *snap = &session.snaps[i];
        for (size_t w = 0; w < (snap->elements + 63) / 64; ++w) {
            uint64_t bits = snap->bits[w];
            while (bits) {
                const size_t e = w * 64 + (size_t)__builtin_ctzll(bits);
                bits &= bits - 1;
                add_candidate(snap->start + e * step, snap->data + e * step, step);
            }
        }
        snap_release(snap);
    }
    free(session.snaps);
    session.snaps = NULL;
    session.snap_count = 0;
    session.snap_total = 0;
}

static void next_scan_snapshot(const char *mode, const Value *wanted)
{
    const size_t step = type_sizes[session.type];
    const Type type = session.type;
    const int m_exact = !strcmp(mode, "exact"), m_changed = !strcmp(mode, "changed"), m_unchanged = !strcmp(mode, "unchanged"),
              m_increased = !strcmp(mode, "increased"), m_decreased = !strcmp(mode, "decreased");
    const size_t chunk = 1u << 20;
    uint8_t *now = malloc(chunk);
    if (!now) return;
    uint64_t alive = 0;
    for (size_t i = 0; i < session.snap_count; ++i) {
        Snap *snap = &session.snaps[i];
        for (size_t off = 0; off < snap->size; off += chunk) {
            const size_t want = snap->size - off < chunk ? snap->size - off : chunk;
            const int readable = read_mem(snap->start + off, now, want) == (ssize_t)want;
            const size_t first = off / step, last = (off + want) / step;
            for (size_t w = first / 64; w <= (last ? (last - 1) / 64 : 0) && w < (snap->elements + 63) / 64; ++w) {
                uint64_t bits = snap->bits[w];
                while (bits) {
                    const size_t e = w * 64 + (size_t)__builtin_ctzll(bits);
                    bits &= bits - 1;
                    if (e < first || e >= last) continue;
                    uint8_t *old = snap->data + e * step;
                    const uint8_t *cur = now + (e - first) * step;
                    int keep = 0;
                    if (readable) {
                        if (m_exact) keep = wanted && values_match(type, cur, wanted);
                        else {
                            const int c = compare_values(type, cur, old, step);
                            if (m_unchanged) keep = c == 0;
                            else if (m_changed || m_increased || m_decreased) keep = step_matches(type, mode, cur, old, step);
                        }
                    }
                    if (keep && !cond_ok(type, cur)) keep = 0;
                    if (keep) { memcpy(old, cur, step); ++alive; }
                    else snap->bits[e / 64] &= ~(1ull << (e % 64));
                }
            }
        }
    }
    free(now);
    session.snap_total = alive;
    if (alive <= MAX_CANDIDATES) collapse_snapshot();
}

/* ---- freeze (value lock) ---- */

typedef struct { int used; uint64_t addr; Value value; } Frozen;
static Frozen frozen[MAX_FROZEN];
static pthread_t freezer;
static int freezer_started;

static void *freeze_thread(void *opaque)
{
    (void)opaque;
    for (;;) {
        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_FROZEN; ++i)
            if (frozen[i].used) write_mem(frozen[i].addr, frozen[i].value.bytes, frozen[i].value.length);
        pthread_mutex_unlock(&lock);
        struct timespec ts = { 0, 8 * 1000000L };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ---- command plumbing ---- */

static void error_reply(char *out, size_t size, const char *message)
{
    snprintf(out, size, "{\"ok\":false,\"error\":\"%s\"}\n", message);
}

static int arg_addr(const char *request, const char *key, uint64_t *addr)
{
    char text[40];
    if (!beer_json_string(request, key, text, sizeof(text))) {
        const char *p = beer_json_find(request, key);   /* also accept a bare number */
        if (!p) return 0;
        *addr = strtoull(p, NULL, 10);
        return 1;
    }
    char *end;
    *addr = strtoull(text, &end, 0);
    return end != text;
}

static int arg_int(const char *request, const char *key, long long fallback, long long *out)
{
    const char *p = beer_json_find(request, key);
    if (!p) { *out = fallback; return 1; }
    if (*p == '"') ++p;
    *out = strtoll(p, NULL, 0);
    return 1;
}

static void result_item(char **cursor, char *end, uint64_t addr, Type type, size_t length, int comma)
{
    uint8_t bytes[256];
    char value[600];
    const ssize_t got = read_mem(addr, bytes, length > sizeof(bytes) ? sizeof(bytes) : length);
    if (got == (ssize_t)length) format_value(type, bytes, length, value, sizeof(value));
    else snprintf(value, sizeof(value), "??");
    if (end - *cursor < 160) return;
    *cursor += snprintf(*cursor, (size_t)(end - *cursor), "%s{\"addr\":\"0x%llx\",\"value\":\"%s\"}", comma ? "," : "", (unsigned long long)addr, value);
}

static void coverage_json(char *out, size_t size)
{
    snprintf(out, size, "{\"scanned_mb\":%llu,\"readonly_mb\":%llu,\"device_mb\":%llu,\"huge_mb\":%llu,\"limit_mb\":%llu}",
             (unsigned long long)(coverage.scanned >> 20), (unsigned long long)(coverage.readonly >> 20),
             (unsigned long long)(coverage.device >> 20), (unsigned long long)(coverage.huge >> 20), (unsigned long long)(coverage.limit >> 20));
}

static void reply_results(char *out, size_t size, size_t offset, size_t limit)
{
    char *cursor = out, *end = out + size - 64;
    char cov[200];
    coverage_json(cov, sizeof(cov));
    if (session.snaps) {   /* still a snapshot: too many candidates to list yet */
        snprintf(out, size, "{\"ok\":true,\"type\":\"%s\",\"count\":%llu,\"snapshot\":true,\"truncated\":%s,\"coverage\":%s,\"results\":[]}\n",
                 type_names[session.type], (unsigned long long)session.snap_total, session.truncated ? "true" : "false", cov);
        return;
    }
    cursor += snprintf(cursor, (size_t)(end - cursor), "{\"ok\":true,\"type\":\"%s\",\"count\":%zu,\"coverage\":%s,\"results\":[", type_names[session.type], session.count, cov);
    for (size_t i = offset; i < session.count && i < offset + limit && end - cursor > 200; ++i)
        result_item(&cursor, end, session.items[i].addr, session.type, session.length, i > offset);
    snprintf(cursor, (size_t)(out + size - cursor), "]}\n");
}

int beer_memory_peek(unsigned long long addr, const char *type_name, char *out, size_t size)
{
    Type type;
    if (!type_from_name(type_name, &type) || !type_sizes[type]) return 0;
    pthread_mutex_lock(&lock);
    uint8_t bytes[8];
    const int ok = open_mem() && read_mem(addr, bytes, type_sizes[type]) == (ssize_t)type_sizes[type];
    pthread_mutex_unlock(&lock);
    if (!ok) return 0;
    format_value(type, bytes, type_sizes[type], out, size);
    return 1;
}

void beer_memory_handle(const char *cmd, const char *request, char *out, size_t size)
{
    pthread_mutex_lock(&lock);
    if (!open_mem()) { pthread_mutex_unlock(&lock); error_reply(out, size, "cannot open /proc/self/mem"); return; }
    char typename_[16] = "i32", valuetext[600] = "", modetext[24] = "exact";
    beer_json_string(request, "type", typename_, sizeof(typename_));
    beer_json_string(request, "value", valuetext, sizeof(valuetext));
    if (!valuetext[0]) {   /* numbers arrive without quotes */
        const char *p = beer_json_find(request, "value");
        if (p && *p != '"') { size_t n = strcspn(p, ",}"); if (n >= sizeof(valuetext)) n = sizeof(valuetext) - 1; memcpy(valuetext, p, n); valuetext[n] = 0; }
    }
    scan_has_cond = 0;
    {
        char op[8] = "", text[48] = "";
        beer_json_string(request, "cond", op, sizeof(op));
        beer_json_string(request, "cond_value", text, sizeof(text));
        if (!text[0]) {
            const char *p = beer_json_find(request, "cond_value");
            if (p && *p != '"') { size_t n = strcspn(p, ",}"); if (n >= sizeof(text)) n = sizeof(text) - 1; memcpy(text, p, n); text[n] = 0; }
        }
        char *stop = NULL;
        const double v = text[0] ? strtod(text, &stop) : 0;
        if (text[0] && stop != text && (!strcmp(op, "lt") || !strcmp(op, "le") || !strcmp(op, "gt") || !strcmp(op, "ge") || !strcmp(op, "eq"))) {
            snprintf(scan_cond, sizeof(scan_cond), "%s", op);
            scan_cond_value = v;
            scan_has_cond = 1;
        }
    }
    Type type = T_I32;
    if (!type_from_name(typename_, &type)) { pthread_mutex_unlock(&lock); error_reply(out, size, "unknown type"); return; }

    if (!strcmp(cmd, "mem_regions")) {
        Region *regions = malloc(sizeof(Region) * 8192);
        size_t n = regions ? read_regions(regions, 8192) : 0;
        char *cursor = out, *end = out + size - 64;
        cursor += snprintf(cursor, (size_t)(end - cursor), "{\"ok\":true,\"regions\":[");
        int first = 1;
        for (size_t i = 0; i < n && end - cursor > 400; ++i) {
            if (regions[i].perms[0] != 'r') continue;
            char path[200];
            size_t k = 0;
            for (const char *p = regions[i].path; *p && k + 2 < sizeof(path); ++p) { if (*p == '"' || *p == '\\') path[k++] = '\\'; path[k++] = *p; }
            path[k] = 0;
            cursor += snprintf(cursor, (size_t)(end - cursor), "%s{\"start\":\"0x%llx\",\"end\":\"0x%llx\",\"perms\":\"%s\",\"path\":\"%s\"}",
                               first ? "" : ",", (unsigned long long)regions[i].start, (unsigned long long)regions[i].end, regions[i].perms, path);
            first = 0;
        }
        snprintf(cursor, (size_t)(out + size - cursor), "]}\n");
        free(regions);
    } else if (!strcmp(cmd, "mem_read")) {
        uint64_t addr;
        long long len;
        if (!arg_addr(request, "addr", &addr)) { error_reply(out, size, "addr missing"); goto done; }
        arg_int(request, "len", 64, &len);
        if (len < 1 || len > 8192) { error_reply(out, size, "len must be 1..8192"); goto done; }
        uint8_t *bytes = malloc((size_t)len);
        const ssize_t got = bytes ? read_mem(addr, bytes, (size_t)len) : -1;
        if (got <= 0) { free(bytes); error_reply(out, size, "unreadable address"); goto done; }
        char *cursor = out;
        cursor += snprintf(cursor, size, "{\"ok\":true,\"addr\":\"0x%llx\",\"hex\":\"", (unsigned long long)addr);
        for (ssize_t i = 0; i < got; ++i) cursor += snprintf(cursor, (size_t)(out + size - cursor), "%02x", bytes[i]);
        snprintf(cursor, (size_t)(out + size - cursor), "\"}\n");
        free(bytes);
    } else if (!strcmp(cmd, "mem_write")) {
        uint64_t addr;
        char hex[2048];
        uint8_t bytes[1024];
        size_t length;
        if (!arg_addr(request, "addr", &addr) || !beer_json_string(request, "hex", hex, sizeof(hex)) ||
            !parse_hex_bytes(hex, bytes, sizeof(bytes), &length)) { error_reply(out, size, "needs addr and hex"); goto done; }
        if (write_mem(addr, bytes, length) != (ssize_t)length) { error_reply(out, size, "write failed (unmapped address?)"); goto done; }
        snprintf(out, size, "{\"ok\":true,\"written\":%zu}\n", length);
    } else if (!strcmp(cmd, "mem_scan") && beer_json_find(request, "unknown") && !strncmp(beer_json_find(request, "unknown"), "true", 4)) {
        if (!type_sizes[type]) { error_reply(out, size, "unknown-value scans need a numeric type"); goto done; }
        const char *w = beer_json_find(request, "writable");
        first_scan_all(type, !(w && !strncmp(w, "false", 5)));
        if (session.snap_total <= MAX_CANDIDATES) collapse_snapshot();
        reply_results(out, size, 0, PAGE_RESULTS);
    } else if (!strcmp(cmd, "mem_scan")) {
        Value wanted;
        if (!parse_value(type, valuetext, &wanted)) { error_reply(out, size, "bad value for type"); goto done; }
        char writable[8] = "true";
        beer_json_string(request, "writable", writable, sizeof(writable));
        const char *w = beer_json_find(request, "writable");
        first_scan(type, &wanted, !(w && !strncmp(w, "false", 5)));
        reply_results(out, size, 0, PAGE_RESULTS);
    } else if (!strcmp(cmd, "mem_next")) {
        if (!session.active) { error_reply(out, size, "no scan in progress"); goto done; }
        beer_json_string(request, "mode", modetext, sizeof(modetext));
        scan_has_max = scan_has_min = 0;
        {
            /* "up_to": largest allowed step (alias "threshold"); "at_least": smallest allowed step. */
            const char *keys[3] = { "up_to", "threshold", "at_least" };
            for (int k = 0; k < 3; ++k) {
                char text[48] = "";
                beer_json_string(request, keys[k], text, sizeof(text));
                if (!text[0]) {
                    const char *p = beer_json_find(request, keys[k]);
                    if (p && *p != '"') { size_t n = strcspn(p, ",}"); if (n >= sizeof(text)) n = sizeof(text) - 1; memcpy(text, p, n); text[n] = 0; }
                }
                char *stop = NULL;
                const double v = text[0] ? strtod(text, &stop) : 0;
                if (!text[0] || stop == text || v < 0) continue;
                if (k == 2) { scan_min_step = v; scan_has_min = 1; }
                else { scan_max_step = v; scan_has_max = 1; }
            }
        }
        Value wanted;
        int have_value = valuetext[0] && parse_value(session.type, valuetext, &wanted);
        if (!strcmp(modetext, "exact") && !have_value) { error_reply(out, size, "exact needs a value"); goto done; }
        if (session.snaps) next_scan_snapshot(modetext, have_value ? &wanted : NULL);
        else next_scan(modetext, have_value ? &wanted : NULL);
        reply_results(out, size, 0, PAGE_RESULTS);
    } else if (!strcmp(cmd, "mem_results")) {
        if (!session.active) { error_reply(out, size, "no scan in progress"); goto done; }
        long long offset, limit;
        arg_int(request, "offset", 0, &offset);
        arg_int(request, "limit", PAGE_RESULTS, &limit);
        if (offset < 0) offset = 0;
        if (limit < 1 || limit > PAGE_RESULTS) limit = PAGE_RESULTS;
        reply_results(out, size, (size_t)offset, (size_t)limit);
    } else if (!strcmp(cmd, "mem_set_all")) {
        if (!session.active || session.snaps) { error_reply(out, size, "narrow the scan down to a list of results first"); goto done; }
        Value wanted;
        if (!parse_value(session.type, valuetext, &wanted)) { error_reply(out, size, "bad value for the scan type"); goto done; }
        size_t written = 0, failed = 0;
        for (size_t i = 0; i < session.count; ++i) {
            if (write_mem(session.items[i].addr, wanted.bytes, wanted.length) == (ssize_t)wanted.length) ++written;
            else ++failed;
        }
        snprintf(out, size, "{\"ok\":true,\"written\":%zu,\"failed\":%zu}\n", written, failed);
    } else if (!strcmp(cmd, "mem_remove")) {
        const char *p = beer_json_find(request, "addrs");
        if (!session.active || session.snaps || !p || *p != '[') { error_reply(out, size, "needs a result list and addrs"); goto done; }
        uint64_t drop[64];
        size_t drops = 0;
        for (++p; *p && *p != ']' && drops < 64;) {
            while (*p == ' ' || *p == ',' || *p == '"') ++p;
            if (*p == ']') break;
            char *stop;
            const uint64_t addr = strtoull(p, &stop, 0);
            if (stop == p) break;
            p = stop;
            while (*p == '"') ++p;
            drop[drops++] = addr;
        }
        size_t kept = 0;
        for (size_t i = 0; i < session.count; ++i) {
            int remove = 0;
            for (size_t d = 0; d < drops; ++d) if (session.items[i].addr == drop[d]) { remove = 1; break; }
            if (!remove) session.items[kept++] = session.items[i];
        }
        session.count = kept;
        snprintf(out, size, "{\"ok\":true,\"count\":%zu}\n", kept);
    } else if (!strcmp(cmd, "mem_reset")) {
        session_reset();
        snprintf(out, size, "{\"ok\":true}\n");
    } else if (!strcmp(cmd, "mem_values")) {
        const char *p = beer_json_find(request, "addrs");
        if (!p || *p != '[') { error_reply(out, size, "addrs missing"); goto done; }
        long long length;
        arg_int(request, "length", 0, &length);
        const size_t item = type_sizes[type] ? type_sizes[type] : (size_t)(length > 0 && length <= 64 ? length : 8);
        char *cursor = out, *end = out + size - 64;
        cursor += snprintf(cursor, (size_t)(end - cursor), "{\"ok\":true,\"values\":[");
        int first = 1;
        for (++p; *p && *p != ']' && end - cursor > 200;) {
            while (*p == ' ' || *p == ',' || *p == '"') ++p;
            if (*p == ']') break;
            char *stop;
            const uint64_t addr = strtoull(p, &stop, 0);
            if (stop == p) break;
            p = stop;
            while (*p == '"') ++p;
            result_item(&cursor, end, addr, type, item, !first);
            first = 0;
        }
        snprintf(cursor, (size_t)(out + size - cursor), "]}\n");
    } else if (!strcmp(cmd, "mem_set")) {
        uint64_t addr;
        Value wanted;
        if (!arg_addr(request, "addr", &addr) || !parse_value(type, valuetext, &wanted)) { error_reply(out, size, "needs addr, type and value"); goto done; }
        if (write_mem(addr, wanted.bytes, wanted.length) != (ssize_t)wanted.length) { error_reply(out, size, "write failed (unmapped address?)"); goto done; }
        snprintf(out, size, "{\"ok\":true}\n");
    } else if (!strcmp(cmd, "mem_freeze")) {
        uint64_t addr;
        Value wanted;
        if (!arg_addr(request, "addr", &addr) || !parse_value(type, valuetext, &wanted)) { error_reply(out, size, "needs addr, type and value"); goto done; }
        int slot = -1;
        for (int i = 0; i < MAX_FROZEN; ++i) {
            if (frozen[i].used && frozen[i].addr == addr) { slot = i; break; }
            if (!frozen[i].used && slot < 0) slot = i;
        }
        if (slot < 0) { error_reply(out, size, "too many frozen values"); goto done; }
        frozen[slot] = (Frozen){ 1, addr, wanted };
        if (!freezer_started && pthread_create(&freezer, NULL, freeze_thread, NULL) == 0) { pthread_detach(freezer); freezer_started = 1; }
        snprintf(out, size, "{\"ok\":true}\n");
    } else if (!strcmp(cmd, "mem_unfreeze")) {
        uint64_t addr;
        if (!arg_addr(request, "addr", &addr)) { error_reply(out, size, "addr missing"); goto done; }
        for (int i = 0; i < MAX_FROZEN; ++i) if (frozen[i].used && frozen[i].addr == addr) frozen[i].used = 0;
        snprintf(out, size, "{\"ok\":true}\n");
    } else if (!strcmp(cmd, "mem_frozen")) {
        char *cursor = out;
        cursor += snprintf(cursor, size, "{\"ok\":true,\"frozen\":[");
        int first = 1;
        for (int i = 0; i < MAX_FROZEN; ++i) {
            if (!frozen[i].used) continue;
            cursor += snprintf(cursor, (size_t)(out + size - cursor), "%s\"0x%llx\"", first ? "" : ",", (unsigned long long)frozen[i].addr);
            first = 0;
        }
        snprintf(cursor, (size_t)(out + size - cursor), "]}\n");
    } else {
        error_reply(out, size, "unknown memory command");
    }
done:
    pthread_mutex_unlock(&lock);
}
