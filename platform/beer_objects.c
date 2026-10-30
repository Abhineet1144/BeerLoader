#include "beer_objects.h"
#include "beer_control.h"

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* loader.c: index 0 = the executable, then each guest DLL; 0 past the end. */
int beer_guest_module_info(int index, uint64_t *base, uint64_t *size, const char **name);
/* loader.c: delivers a key to the game window like a real key press. */
void beer_control_inject_key(unsigned vk, unsigned dik, int extended, int down);

#define MAX_MODULES 128
#define MAX_INSTANCES (1u << 20)
#define MAX_GROUPS (1u << 16)
#define CHUNK (1u << 20)
#define TAIL 128u                      /* overlap between chunks: the largest transform is 96 bytes */
#define DEFAULT_MAX_OFFSET 0x800u      /* how far before a transform its object may start */

enum { K_MAT4, K_MAT34, K_MAT4T, K_QTS, K_QTS64, K_COUNT };
static const char *const kind_names[K_COUNT] = { "mat4", "mat34", "mat4t", "qts", "qts64" };
static const unsigned kind_sizes[K_COUNT] = { 64, 64, 64, 48, 96 };

typedef struct { uint64_t transform, base; uint32_t group; float pos[3]; } Instance;
/* A group is one kind of object: its type (vtable), where its transform sits and, for an entity that only points at a
 * transform holder (RAGE fwTransform, a Unity/Unreal component), the offset of that pointer (via >= 0). */
typedef struct { uint64_t vtable; uint32_t offset; uint8_t kind; int32_t via; uint32_t count, first; char name[112]; } Group;
typedef struct { uint64_t base, size; char name[64]; } Module;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_mem_fd = -1;
static Module g_modules[MAX_MODULES];
static int g_module_count;
static Instance *g_instances;          /* sorted by group after a scan */
static uint32_t g_instance_count;
static Group *g_groups;
static uint32_t g_group_count;

static ssize_t read_mem(uint64_t addr, void *buf, size_t len)
{
    if (g_mem_fd < 0) g_mem_fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    return g_mem_fd < 0 ? -1 : pread(g_mem_fd, buf, len, (off_t)addr);
}

static ssize_t write_mem(uint64_t addr, const void *buf, size_t len)
{
    if (g_mem_fd < 0) g_mem_fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    return g_mem_fd < 0 ? -1 : pwrite(g_mem_fd, buf, len, (off_t)addr);
}

/* Every copy of one position (obj_copies), moved together by obj_shift. */
#define MAX_COPIES 512
typedef struct { uint64_t addr; uint8_t is_double; } Copy;
static Copy g_copies[MAX_COPIES];
static uint32_t g_copy_count;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void load_modules(void)
{
    g_module_count = 0;
    uint64_t base, size;
    const char *name;
    for (int i = 0; g_module_count < MAX_MODULES && beer_guest_module_info(i, &base, &size, &name); ++i) {
        if (!base || !size) continue;
        Module *m = &g_modules[g_module_count++];
        m->base = base; m->size = size;
        snprintf(m->name, sizeof(m->name), "%s", name ? name : "?");
    }
}

static const Module *module_at(uint64_t addr)
{
    for (int i = 0; i < g_module_count; ++i)
        if (addr - g_modules[i].base < g_modules[i].size) return &g_modules[i];
    return NULL;
}

/* ---- transform recognisers ---- */

static float dot3f(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static int finite_all(const float *v, int n)
{
    for (int i = 0; i < n; ++i) if (!isfinite(v[i])) return 0;
    return 1;
}

static int plausible_position(double x, double y, double z)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) return 0;
    if (fabs(x) > 1e6 || fabs(y) > 1e6 || fabs(z) > 1e6) return 0;
    return fabs(x) > 1e-3 || fabs(y) > 1e-3 || fabs(z) > 1e-3;   /* objects parked at the origin are templates */
}

/* Three rows a, b, c: lengths (the scale) in [1e-2, 1e2], mutually orthogonal, right-handed. */
static int rotation_rows(const float *a, const float *b, const float *c)
{
    const float la = dot3f(a, a), lb = dot3f(b, b), lc = dot3f(c, c);
    if (!(la > 1e-4f && la < 1e4f && lb > 1e-4f && lb < 1e4f && lc > 1e-4f && lc < 1e4f)) return 0;
    const float ab = dot3f(a, b), ac = dot3f(a, c), bc = dot3f(b, c);
    if (ab * ab > 1e-6f * la * lb || ac * ac > 1e-6f * la * lc || bc * bc > 1e-6f * lb * lc) return 0;
    const float cross[3] = { b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0] };
    return dot3f(a, cross) > 0;
}

/* m is 16-byte aligned; returns the kind or -1. */
static int match_matrix(const float *m)
{
    if (!finite_all(m, 16)) return -1;
    const float a[3] = { m[0], m[1], m[2] }, b[3] = { m[4], m[5], m[6] }, c[3] = { m[8], m[9], m[10] };
    if (!rotation_rows(a, b, c)) return -1;
    if (m[3] == 0 && m[7] == 0 && m[11] == 0 && m[15] == 1.0f)
        return plausible_position(m[12], m[13], m[14]) ? K_MAT4 : -1;
    if (m[12] == 0 && m[13] == 0 && m[14] == 0 && m[15] == 1.0f)            /* translation in the w lanes */
        return plausible_position(m[3], m[7], m[11]) ? K_MAT4T : -1;
    /* Unused w lanes: only an exact rigid (unit-scale) rotation, or random data would slip through. */
    const float la = dot3f(a, a), lb = dot3f(b, b), lc = dot3f(c, c);
    if (fabsf(la - 1) > 1e-3f || fabsf(lb - 1) > 1e-3f || fabsf(lc - 1) > 1e-3f) return -1;
    return plausible_position(m[12], m[13], m[14]) ? K_MAT34 : -1;
}

static int scale_ok(double s) { return isfinite(s) && s > 1e-3 && s < 1e3; }

static int match_qts(const float *v)
{
    if (!finite_all(v, 12)) return -1;
    const float q = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3];
    if (fabsf(q - 1) > 1e-3f || v[7] != 0 || v[11] != 0) return -1;
    if (!scale_ok(v[8]) || !scale_ok(v[9]) || !scale_ok(v[10])) return -1;
    return plausible_position(v[4], v[5], v[6]) ? K_QTS : -1;
}

static int match_qts64(const double *v)
{
    for (int i = 0; i < 12; ++i) if (!isfinite(v[i])) return -1;
    const double q = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3];
    if (fabs(q - 1) > 1e-6 || v[7] != 0 || v[11] != 0) return -1;
    if (!scale_ok(v[8]) || !scale_ok(v[9]) || !scale_ok(v[10])) return -1;
    return plausible_position(v[4], v[5], v[6]) ? K_QTS64 : -1;
}

/* Position of a transform as it sits in memory (bytes from its start). */
static void read_position(const uint8_t *t, int kind, float pos[3])
{
    const float *f = (const float *)t;
    switch (kind) {
    case K_MAT4: case K_MAT34: pos[0] = f[12]; pos[1] = f[13]; pos[2] = f[14]; break;
    case K_MAT4T: pos[0] = f[3]; pos[1] = f[7]; pos[2] = f[11]; break;
    case K_QTS: pos[0] = f[4]; pos[1] = f[5]; pos[2] = f[6]; break;
    case K_QTS64: { const double *d = (const double *)t; pos[0] = (float)d[4]; pos[1] = (float)d[5]; pos[2] = (float)d[6]; break; }
    }
}

/* ---- owners and types ---- */

/* The object owning a transform starts at the nearest earlier pointer into a guest module that points at two more
 * module pointers: a C++ vtable. Returns its offset before the transform, 0 if none was found. */
static uint32_t find_owner(uint64_t transform, uint32_t max_offset, uint64_t *vtable)
{
    static __thread uint64_t words[DEFAULT_MAX_OFFSET / 8];
    if (max_offset > sizeof(words)) max_offset = sizeof(words);
    if (transform < max_offset) return 0;
    const uint64_t start = transform - max_offset;
    if (read_mem(start, words, max_offset) != (ssize_t)max_offset) return 0;
    for (uint32_t back = 8; back <= max_offset; back += 8) {
        const uint64_t v = words[(max_offset - back) / 8];
        if ((v & 7) || !module_at(v)) continue;
        uint64_t entries[2];
        if (read_mem(v, entries, sizeof(entries)) != (ssize_t)sizeof(entries)) continue;
        if (!module_at(entries[0]) || !module_at(entries[1])) continue;
        *vtable = v;
        return back;
    }
    return 0;
}

/* ".?AVPed@rage@@" -> "rage::Ped" */
static void demangle(const char *raw, char *out, size_t size)
{
    const char *s = raw;
    if (!strncmp(s, ".?AV", 4) || !strncmp(s, ".?AU", 4)) s += 4;
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", s);
    char *end = strstr(tmp, "@@");
    if (end) *end = 0;
    if (strchr(tmp, '?')) { snprintf(out, size, "%s", tmp); return; }   /* templates: keep as mangled */
    char *parts[16];
    int n = 0;
    for (char *p = strtok(tmp, "@"); p && n < 16; p = strtok(NULL, "@")) parts[n++] = p;
    out[0] = 0;
    size_t used = 0;
    for (int i = n - 1; i >= 0; --i)
        used += (size_t)snprintf(out + used, used < size ? size - used : 0, "%s%s", parts[i], i ? "::" : "");
}

/* Name of the class behind a vtable: MSVC x64 RTTI (vtable[-1] -> CompleteObjectLocator -> TypeDescriptor),
 * else module + offset. */
static void type_name(uint64_t vtable, char *out, size_t size)
{
    const Module *m = module_at(vtable);
    uint64_t locator = 0;
    uint32_t col[6];
    if (read_mem(vtable - 8, &locator, 8) == 8 && read_mem(locator, col, sizeof(col)) == (ssize_t)sizeof(col) &&
        col[0] == 1) {
        const Module *lm = module_at(locator);
        if (lm && lm->base + col[5] == locator) {
            char raw[128] = "";
            if (read_mem(lm->base + col[3] + 16, raw, sizeof(raw) - 1) > 4 && !strncmp(raw, ".?A", 3)) {
                raw[sizeof(raw) - 1] = 0;
                demangle(raw, out, size);
                return;
            }
        }
    }
    snprintf(out, size, "%s vtable+0x%llx", m ? m->name : "?", (unsigned long long)(m ? vtable - m->base : vtable));
}

/* ---- scan ---- */

typedef struct { uint64_t vtable; uint32_t offset; uint8_t kind; int32_t via; uint32_t group; } GroupKey;
static GroupKey *g_keys;               /* open-addressing table while scanning */
#define KEY_SLOTS (MAX_GROUPS * 2u)

static uint32_t group_for(uint64_t vtable, uint32_t offset, uint8_t kind, int32_t via)
{
    uint64_t h = (vtable * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)offset << 3) ^ kind ^ ((uint64_t)(uint32_t)via << 20);
    for (uint32_t probe = 0; probe < KEY_SLOTS; ++probe) {
        GroupKey *k = &g_keys[(h + probe) % KEY_SLOTS];
        if (k->group && k->vtable == vtable && k->offset == offset && k->kind == kind && k->via == via) return k->group - 1;
        if (!k->group) {
            if (g_group_count >= MAX_GROUPS) return UINT32_MAX;
            Group *g = &g_groups[g_group_count];
            memset(g, 0, sizeof(*g));
            g->vtable = vtable; g->offset = offset; g->kind = kind; g->via = via;
            k->vtable = vtable; k->offset = offset; k->kind = kind; k->via = via; k->group = ++g_group_count;
            return g_group_count - 1;
        }
    }
    return UINT32_MAX;
}

static int scannable_region(const char *perms, const char *path, uint64_t size)
{
    if (perms[0] != 'r' || perms[1] != 'w') return 0;
    if (size > (24ull << 30)) return 0;
    return !*path || !strcmp(path, "[heap]");   /* anonymous memory: game heaps, the image's data; not files/devices */
}

static void add_instance(uint64_t transform, int kind, const uint8_t *bytes, uint32_t max_offset)
{
    if (g_instance_count >= MAX_INSTANCES) return;
    uint64_t vtable = 0;
    const uint32_t offset = find_owner(transform, max_offset, &vtable);
    const uint32_t group = group_for(vtable, offset, (uint8_t)kind, -1);
    if (group == UINT32_MAX) return;
    Instance *in = &g_instances[g_instance_count++];
    in->transform = transform;
    in->base = transform - offset;
    in->group = group;
    read_position(bytes, kind, in->pos);
    g_groups[group].count++;
}

/* Second pass: objects that hold a pointer to a transform holder.  Engines often keep the matrix in a small
 * component (RAGE's fwTransform, Unity/Unreal scene components) that the entity points at; the holder's own vtable
 * says nothing about what the object is.  Every typed holder start is looked up as a pointer value anywhere in the
 * scanned memory, and the object around each hit (nearest vtable before it) becomes an entity instance. */
typedef struct { uint64_t base; uint32_t instance; } Target;

static void link_entities(uint8_t *buffer, uint32_t max_offset, uint64_t *scanned)
{
    const uint32_t direct = g_instance_count;
    uint32_t slots = 1;
    while (slots < direct * 2u) slots <<= 1;
    Target *targets = calloc(slots, sizeof(Target));
    if (!targets) return;
    uint64_t lowest = UINT64_MAX, highest = 0;
    for (uint32_t i = 0; i < direct; ++i) {
        const Instance *in = &g_instances[i];
        if (!g_groups[in->group].vtable) continue;
        uint64_t h = (in->base * 0x9E3779B97F4A7C15ull) >> 20;
        for (;; ++h) {                                  /* first transform of a holder (lowest address) wins */
            Target *t = &targets[h & (slots - 1)];
            if (t->base == in->base) break;
            if (!t->base) { t->base = in->base; t->instance = i; break; }
        }
        if (in->base < lowest) lowest = in->base;
        if (in->base > highest) highest = in->base;
    }
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    while (maps && fgets(line, sizeof(line), maps)) {
        unsigned long long start, end;
        char perms[8] = "", path[256] = "";
        if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %255[^\n]", &start, &end, perms, path) < 3) continue;
        const char *p = path;
        while (*p == ' ') ++p;
        if (!scannable_region(perms, p, end - start)) continue;
        for (uint64_t at = start; at < end; at += CHUNK) {
            const size_t want = end - at < CHUNK ? (size_t)(end - at) : CHUNK;
            const ssize_t got = read_mem(at, buffer, want);
            if (got < 8) continue;
            *scanned += (uint64_t)got;
            const uint64_t *words = (const uint64_t *)buffer;
            for (size_t w = 0; w < (size_t)got / 8; ++w) {
                const uint64_t v = words[w];
                if (v < lowest || v > highest || (v & 7)) continue;
                uint64_t h = (v * 0x9E3779B97F4A7C15ull) >> 20;
                const Target *t;
                for (;; ++h) {
                    t = &targets[h & (slots - 1)];
                    if (!t->base || t->base == v) break;
                }
                if (!t->base) continue;
                const uint64_t where = at + w * 8;
                if (where - v < 0x1000) continue;               /* the holder pointing into itself */
                uint64_t vtable = 0;
                const uint32_t back = find_owner(where, max_offset, &vtable);
                if (!back || g_instance_count >= MAX_INSTANCES) continue;
                const Instance holder = g_instances[t->instance];
                if (vtable == g_groups[holder.group].vtable) continue;   /* same type: a list of holders */
                const uint32_t group = group_for(vtable, g_groups[holder.group].offset, g_groups[holder.group].kind,
                                                 (int32_t)back);
                if (group == UINT32_MAX) continue;
                Instance *in = &g_instances[g_instance_count++];
                *in = holder;
                in->base = where - back;
                in->group = group;
                g_groups[group].count++;
            }
        }
    }
    if (maps) fclose(maps);
    free(targets);
}

static void scan(uint32_t max_offset, uint64_t *scanned)
{
    free(g_instances); free(g_groups);
    g_instances = calloc(MAX_INSTANCES, sizeof(Instance));
    g_groups = calloc(MAX_GROUPS, sizeof(Group));
    g_keys = calloc(KEY_SLOTS, sizeof(GroupKey));
    g_instance_count = g_group_count = 0;
    *scanned = 0;
    uint8_t *buffer = malloc(CHUNK + TAIL);
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!g_instances || !g_groups || !g_keys || !buffer || !maps) goto out;
    load_modules();
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
        unsigned long long start, end;
        char perms[8] = "", path[256] = "";
        if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %255[^\n]", &start, &end, perms, path) < 3) continue;
        const char *p = path;
        while (*p == ' ') ++p;
        if (!scannable_region(perms, p, end - start)) continue;
        for (uint64_t at = start; at < end; at += CHUNK) {
            size_t want = end - at < CHUNK + TAIL ? (size_t)(end - at) : CHUNK + TAIL;
            const ssize_t got = read_mem(at, buffer, want);
            if (got < 64) continue;
            *scanned += (uint64_t)(got < CHUNK ? got : CHUNK);
            const size_t limit = (size_t)got;
            const size_t stop = limit < CHUNK ? limit : CHUNK;   /* transforms starting in the overlap belong to the next chunk */
            for (size_t off = 0; off < stop; off += 16) {
                const uint8_t *b = buffer + off;
                if (off + 64 <= limit) {
                    const int k = match_matrix((const float *)b);
                    if (k >= 0) { add_instance(at + off, k, b, max_offset); continue; }
                }
                if (off + 48 <= limit && match_qts((const float *)b) >= 0) { add_instance(at + off, K_QTS, b, max_offset); continue; }
                if (off + 96 <= limit && match_qts64((const double *)b) >= 0) add_instance(at + off, K_QTS64, b, max_offset);
            }
        }
    }
    if (g_instance_count) link_entities(buffer, max_offset, scanned);
out:
    if (maps) fclose(maps);
    free(buffer);
    free(g_keys); g_keys = NULL;
    if (!g_instances || !g_groups) return;
    /* Instances grouped together: counting sort by group. */
    uint32_t next = 0;
    for (uint32_t g = 0; g < g_group_count; ++g) { g_groups[g].first = next; next += g_groups[g].count; }
    Instance *sorted = malloc(sizeof(Instance) * (g_instance_count ? g_instance_count : 1));
    if (sorted) {
        uint32_t *fill = calloc(g_group_count ? g_group_count : 1, sizeof(uint32_t));
        if (fill) {
            for (uint32_t i = 0; i < g_instance_count; ++i) {
                const Instance *in = &g_instances[i];
                sorted[g_groups[in->group].first + fill[in->group]++] = *in;
            }
            free(fill);
            free(g_instances);
            g_instances = sorted;
        } else free(sorted);
    }
    for (uint32_t g = 0; g < g_group_count; ++g) {
        if (g_groups[g].vtable) {
            type_name(g_groups[g].vtable, g_groups[g].name, sizeof(g_groups[g].name));
            if (g_groups[g].via < 0) {
                const size_t used = strlen(g_groups[g].name);
                snprintf(g_groups[g].name + used, sizeof(g_groups[g].name) - used, " [transform holder]");
            }
        } else snprintf(g_groups[g].name, sizeof(g_groups[g].name), "Untyped %s transforms", kind_names[g_groups[g].kind]);
    }
}

/* ---- replies ---- */

static void reply_error(char *out, size_t size, const char *message)
{
    snprintf(out, size, "{\"ok\":false,\"error\":\"%s\"}\n", message);
}

static void json_text(char *out, size_t size, const char *text)
{
    size_t n = 0;
    for (; *text && n + 3 < size; ++text) {
        const unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c >= 32 && c < 127) out[n++] = (char)c;
    }
    out[n] = 0;
}

static long long arg_int(const char *request, const char *key, long long fallback)
{
    int32_t milli;
    return beer_json_milli(request, key, &milli) ? milli / 1000 : fallback;
}

static double arg_float(const char *request, const char *key, double fallback)
{
    const char *value = beer_json_find(request, key);
    if (!value) return fallback;
    char *end = NULL;
    const double d = strtod(value, &end);
    return end == value ? fallback : d;
}

/* Live position of an instance; 0 when its memory no longer holds a transform. */
static int live_position(const Instance *in, float pos[3])
{
    const Group *g = &g_groups[in->group];
    uint8_t bytes[96];
    if (read_mem(in->transform, bytes, kind_sizes[g->kind]) != (ssize_t)kind_sizes[g->kind]) return 0;
    const int still = g->kind == K_QTS ? match_qts((const float *)bytes) : g->kind == K_QTS64
                      ? match_qts64((const double *)bytes) : match_matrix((const float *)bytes);
    if (still < 0) return 0;
    read_position(bytes, g->kind, pos);
    return 1;
}

static void snapshot(float *pos, uint8_t *valid)
{
    for (uint32_t i = 0; i < g_instance_count; ++i)
        valid[i] = g_groups[g_instances[i].group].vtable && live_position(&g_instances[i], &pos[3 * i]);
}

static void hold_key(unsigned vk, unsigned dik, unsigned ms)
{
    beer_control_inject_key(vk, dik, 0, 1);
    usleep(ms * 1000u);
    beer_control_inject_key(vk, dik, 0, 0);
    usleep(300000);                                    /* let the character settle */
}

static int compare_groups(const void *a, const void *b)
{
    const Group *x = &g_groups[*(const uint32_t *)a], *y = &g_groups[*(const uint32_t *)b];
    const int rank_x = !x->vtable ? 0 : x->via >= 0 ? 2 : 1, rank_y = !y->vtable ? 0 : y->via >= 0 ? 2 : 1;
    if (rank_x != rank_y) return rank_y - rank_x;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static size_t group_json(char *out, size_t size, uint32_t id)
{
    const Group *g = &g_groups[id];
    char name[240];
    json_text(name, sizeof(name), g->name);
    return (size_t)snprintf(out, size, "{\"id\":%u,\"name\":\"%s\",\"kind\":\"%s\",\"offset\":%u,\"via\":%d,\"vtable\":\"0x%llx\",\"count\":%u}",
                            id, name, kind_names[g->kind], g->offset, g->via, (unsigned long long)g->vtable, g->count);
}

static size_t transform_json(char *out, size_t size, uint64_t transform, int kind)
{
    uint8_t bytes[96];
    if (read_mem(transform, bytes, kind_sizes[kind]) != (ssize_t)kind_sizes[kind]) return (size_t)snprintf(out, size, "null");
    size_t n = (size_t)snprintf(out, size, "[");
    if (kind == K_QTS64) {
        const double *d = (const double *)bytes;
        for (int i = 0; i < 12; ++i) n += (size_t)snprintf(out + n, size - n, "%s%.9g", i ? "," : "", d[i]);
    } else {
        const float *f = (const float *)bytes;
        const int count = kind == K_QTS ? 12 : 16;
        for (int i = 0; i < count; ++i) n += (size_t)snprintf(out + n, size - n, "%s%.9g", i ? "," : "", (double)f[i]);
    }
    return n + (size_t)snprintf(out + n, size - n, "]");
}

void beer_objects_handle(const char *cmd, const char *request, char *out, size_t size)
{
    pthread_mutex_lock(&g_lock);
    char *cursor = out, *end = out + size - 64;
    if (!strcmp(cmd, "obj_scan")) {
        long long max_offset = arg_int(request, "max_offset", DEFAULT_MAX_OFFSET);
        if (max_offset < 8 || max_offset > DEFAULT_MAX_OFFSET) max_offset = DEFAULT_MAX_OFFSET;
        const uint64_t started = now_ms();
        uint64_t scanned = 0;
        scan((uint32_t)max_offset, &scanned);
        uint32_t typed = 0;
        uint32_t *order = malloc(sizeof(uint32_t) * (g_group_count ? g_group_count : 1));
        for (uint32_t g = 0; g < g_group_count; ++g) { if (order) order[g] = g; if (g_groups[g].vtable) typed += g_groups[g].count; }
        if (order) qsort(order, g_group_count, sizeof(uint32_t), compare_groups);
        cursor += snprintf(cursor, (size_t)(end - cursor),
                           "{\"ok\":true,\"ms\":%llu,\"scanned_mb\":%llu,\"transforms\":%u,\"typed\":%u,\"group_count\":%u,\"groups\":[",
                           (unsigned long long)(now_ms() - started), (unsigned long long)(scanned >> 20), g_instance_count, typed,
                           g_group_count);
        for (uint32_t i = 0; order && i < g_group_count && end - cursor > 400; ++i) {
            if (i) *cursor++ = ',';
            cursor += group_json(cursor, (size_t)(end - cursor), order[i]);
        }
        snprintf(cursor, (size_t)(out + size - cursor), "]}\n");
        free(order);
    } else if (!strcmp(cmd, "obj_list")) {
        const long long id = arg_int(request, "group", -1);
        long long offset = arg_int(request, "offset", 0), limit = arg_int(request, "limit", 100);
        if (id < 0 || id >= (long long)g_group_count) { reply_error(out, size, "no such group (scan first)"); goto done; }
        const Group *g = &g_groups[id];
        if (offset < 0) offset = 0;
        if (limit < 1 || limit > 300) limit = 300;
        cursor += snprintf(cursor, (size_t)(end - cursor), "{\"ok\":true,\"group\":");
        cursor += group_json(cursor, (size_t)(end - cursor), (uint32_t)id);
        cursor += snprintf(cursor, (size_t)(end - cursor), ",\"items\":[");
        for (long long i = offset; i < g->count && i < offset + limit && end - cursor > 1200; ++i) {
            const Instance *in = &g_instances[g->first + i];
            cursor += snprintf(cursor, (size_t)(end - cursor), "%s{\"index\":%lld,\"base\":\"0x%llx\",\"transform\":\"0x%llx\",\"v\":",
                               i > offset ? "," : "", i, (unsigned long long)in->base, (unsigned long long)in->transform);
            cursor += transform_json(cursor, (size_t)(end - cursor), in->transform, g->kind);
            *cursor++ = '}';
        }
        snprintf(cursor, (size_t)(out + size - cursor), "]}\n");
    } else if (!strcmp(cmd, "obj_moved")) {
        long long limit = arg_int(request, "limit", 50);
        int32_t min_milli = 100, max_milli = 50000;
        beer_json_milli(request, "min", &min_milli);
        beer_json_milli(request, "max", &max_milli);
        const float min = (float)min_milli / 1000.0f, max = (float)max_milli / 1000.0f;
        if (limit < 1 || limit > 200) limit = 200;
        typedef struct { uint32_t index; float distance; } Moved;
        Moved *moved = malloc(sizeof(Moved) * 4096);
        uint32_t moved_count = 0;
        for (uint32_t i = 0; i < g_instance_count; ++i) {
            Instance *in = &g_instances[i];
            const Group *g = &g_groups[in->group];
            if (!g->vtable) continue;                         /* untyped transforms are mostly not objects */
            uint8_t bytes[96];
            if (read_mem(in->transform, bytes, kind_sizes[g->kind]) != (ssize_t)kind_sizes[g->kind]) continue;
            /* Freed and reused memory no longer holds a transform: leave the remembered position alone. */
            const int still = g->kind == K_QTS ? match_qts((const float *)bytes) : g->kind == K_QTS64
                              ? match_qts64((const double *)bytes) : match_matrix((const float *)bytes);
            if (still < 0) continue;
            float pos[3];
            read_position(bytes, g->kind, pos);
            const float dx = pos[0] - in->pos[0], dy = pos[1] - in->pos[1], dz = pos[2] - in->pos[2];
            const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
            memcpy(in->pos, pos, sizeof(pos));
            /* A jump beyond `max` between two queries is a pooled slot handed to another object, not motion. */
            if (!(distance >= min) || distance > max || !moved) continue;
            if (moved_count < 4096) moved[moved_count++] = (Moved){ i, distance };
        }
        /* Most-moved first; one entry per transform, the entity rather than its transform holder. */
        uint32_t emitted = 0;
        uint64_t *seen = malloc(sizeof(uint64_t) * 2 * (size_t)limit);
        cursor += snprintf(cursor, (size_t)(end - cursor), "{\"ok\":true,\"moved\":%u,\"items\":[", moved_count);
        for (uint32_t i = 0; seen && i < moved_count && emitted < (uint32_t)limit && end - cursor > 1200; ++i) {
            uint32_t best = i;
            for (uint32_t j = i + 1; j < moved_count; ++j) {
                const float a = moved[j].distance, b = moved[best].distance;
                if (a > b || (a == b && g_groups[g_instances[moved[j].index].group].via >
                                         g_groups[g_instances[moved[best].index].group].via)) best = j;
            }
            const Moved t = moved[i]; moved[i] = moved[best]; moved[best] = t;
            const Instance *in = &g_instances[moved[i].index];
            int duplicate = 0;
            for (uint32_t k = 0; k < emitted; ++k) if (seen[2 * k] == in->transform || seen[2 * k + 1] == in->base) duplicate = 1;
            if (duplicate) continue;
            seen[2 * emitted] = in->transform;
            seen[2 * emitted + 1] = in->base;
            const Group *g = &g_groups[in->group];
            char name[240];
            json_text(name, sizeof(name), g->name);
            cursor += snprintf(cursor, (size_t)(end - cursor),
                               "%s{\"group\":%u,\"name\":\"%s\",\"kind\":\"%s\",\"index\":%u,\"base\":\"0x%llx\",\"transform\":\"0x%llx\",\"moved\":%.3f,\"v\":",
                               emitted++ ? "," : "", in->group, name, kind_names[g->kind], moved[i].index - g->first,
                               (unsigned long long)in->base, (unsigned long long)in->transform, (double)moved[i].distance);
            cursor += transform_json(cursor, (size_t)(end - cursor), in->transform, g->kind);
            *cursor++ = '}';
        }
        snprintf(cursor, (size_t)(out + size - cursor), "]}\n");
        free(seen);
        free(moved);
    } else if (!strcmp(cmd, "obj_find_player")) {
        /* Walks the controlled character forward (W) and back (S): it is what moves one way and then the other. */
        const unsigned hold = (unsigned)arg_int(request, "hold_ms", 1500);
        float *p0 = malloc(sizeof(float) * 3 * (g_instance_count ? g_instance_count : 1));
        float *p1 = malloc(sizeof(float) * 3 * (g_instance_count ? g_instance_count : 1));
        float *p2 = malloc(sizeof(float) * 3 * (g_instance_count ? g_instance_count : 1));
        uint8_t *v0 = malloc(g_instance_count + 1), *v1 = malloc(g_instance_count + 1), *v2 = malloc(g_instance_count + 1);
        if (!g_instance_count || !p0 || !p1 || !p2 || !v0 || !v1 || !v2) {
            reply_error(out, size, g_instance_count ? "out of memory" : "scan first");
        } else {
            snapshot(p0, v0);
            hold_key(0x57, 0x11, hold);   /* W */
            snapshot(p1, v1);
            hold_key(0x53, 0x1F, hold);   /* S */
            snapshot(p2, v2);
            typedef struct { uint32_t index; float there, back, score; } Hit;
            Hit hits[64];
            uint32_t hit_count = 0;
            for (uint32_t i = 0; i < g_instance_count; ++i) {
                if (!v0[i] || !v1[i] || !v2[i]) continue;
                float d1[3], d2[3];
                for (int k = 0; k < 3; ++k) { d1[k] = p1[3 * i + k] - p0[3 * i + k]; d2[k] = p2[3 * i + k] - p1[3 * i + k]; }
                const float l1 = sqrtf(dot3f(d1, d1)), l2 = sqrtf(dot3f(d2, d2));
                if (l1 < 0.3f || l2 < 0.3f || l1 > 30 || l2 > 30) continue;
                const float cosine = dot3f(d1, d2) / (l1 * l2);
                if (cosine > -0.5f) continue;
                /* Opposite and equal moves score highest; entities beat their transform holders. */
                const float balance = (l1 < l2 ? l1 / l2 : l2 / l1);
                const float score = -cosine * balance + (g_groups[g_instances[i].group].via >= 0 ? 0.5f : 0);
                if (hit_count < 64) hits[hit_count++] = (Hit){ i, l1, l2, score };
                else {
                    uint32_t worst = 0;
                    for (uint32_t k = 1; k < 64; ++k) if (hits[k].score < hits[worst].score) worst = k;
                    if (score > hits[worst].score) hits[worst] = (Hit){ i, l1, l2, score };
                }
            }
            for (uint32_t a = 0; a < hit_count; ++a)
                for (uint32_t b = a + 1; b < hit_count; ++b)
                    if (hits[b].score > hits[a].score) { const Hit t = hits[a]; hits[a] = hits[b]; hits[b] = t; }
            cursor += snprintf(cursor, (size_t)(end - cursor), "{\"ok\":true,\"items\":[");
            uint32_t emitted = 0;
            uint64_t seen[64];
            for (uint32_t h = 0; h < hit_count && end - cursor > 1200; ++h) {
                const Instance *in = &g_instances[hits[h].index];
                int duplicate = 0;
                for (uint32_t k = 0; k < emitted; ++k) if (seen[k] == in->base) duplicate = 1;
                if (duplicate) continue;
                seen[emitted] = in->base;
                const Group *g = &g_groups[in->group];
                char name[240];
                json_text(name, sizeof(name), g->name);
                cursor += snprintf(cursor, (size_t)(end - cursor),
                                   "%s{\"group\":%u,\"name\":\"%s\",\"kind\":\"%s\",\"via\":%d,\"index\":%u,\"base\":\"0x%llx\",\"transform\":\"0x%llx\",\"forward\":%.2f,\"back\":%.2f,\"score\":%.2f,\"v\":",
                                   emitted++ ? "," : "", in->group, name, kind_names[g->kind], g->via, hits[h].index - g->first,
                                   (unsigned long long)in->base, (unsigned long long)in->transform,
                                   (double)hits[h].there, (double)hits[h].back, (double)hits[h].score);
                cursor += transform_json(cursor, (size_t)(end - cursor), in->transform, g->kind);
                *cursor++ = '}';
            }
            snprintf(cursor, (size_t)(out + size - cursor), "]}\n");
        }
        free(p0); free(p1); free(p2); free(v0); free(v1); free(v2);
    } else if (!strcmp(cmd, "obj_copies")) {
        /* Engines keep a position in several places (physics body, render matrix, previous frame, AI caches) and
         * rebuild some from others every frame: writing one copy is undone at once.  Find them all: three
         * consecutive floats (or doubles) equal to x, y, z within tol. */
        const double x = arg_float(request, "x", 0), y = arg_float(request, "y", 0), z = arg_float(request, "z", 0);
        const double tol = arg_float(request, "tol", 0.05);
        const uint64_t started = now_ms();
        uint8_t *buffer = malloc(CHUNK + 32);
        FILE *maps = fopen("/proc/self/maps", "r");
        g_copy_count = 0;
        char line[512];
        while (buffer && maps && fgets(line, sizeof(line), maps) && g_copy_count < MAX_COPIES) {
            unsigned long long start, stop;
            char perms[8] = "", path[256] = "";
            if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %255[^\n]", &start, &stop, perms, path) < 3) continue;
            const char *p = path;
            while (*p == ' ') ++p;
            if (!scannable_region(perms, p, stop - start)) continue;
            for (uint64_t at = start; at < stop && g_copy_count < MAX_COPIES; at += CHUNK) {
                const size_t want = stop - at < CHUNK + 32 ? (size_t)(stop - at) : CHUNK + 32;
                const ssize_t got = read_mem(at, buffer, want);
                if (got < 24) continue;
                const size_t limit = (size_t)got, range = limit < CHUNK ? limit : CHUNK;
                for (size_t off = 0; off < range && off + 12 <= limit && g_copy_count < MAX_COPIES; off += 4) {
                    const float *f = (const float *)(buffer + off);
                    if (fabs(f[0] - x) > tol || fabs(f[1] - y) > tol || fabs(f[2] - z) > tol) continue;
                    g_copies[g_copy_count++] = (Copy){ at + off, 0 };
                }
                for (size_t off = 0; off < range && off + 24 <= limit && g_copy_count < MAX_COPIES; off += 8) {
                    const double *d = (const double *)(buffer + off);
                    if (fabs(d[0] - x) > tol || fabs(d[1] - y) > tol || fabs(d[2] - z) > tol) continue;
                    g_copies[g_copy_count++] = (Copy){ at + off, 1 };
                }
            }
        }
        if (maps) fclose(maps);
        free(buffer);
        cursor += snprintf(cursor, (size_t)(end - cursor), "{\"ok\":true,\"ms\":%llu,\"count\":%u,\"items\":[",
                           (unsigned long long)(now_ms() - started), g_copy_count);
        for (uint32_t i = 0; i < g_copy_count && end - cursor > 80; ++i)
            cursor += snprintf(cursor, (size_t)(end - cursor), "%s{\"addr\":\"0x%llx\",\"type\":\"%s\"}", i ? "," : "",
                               (unsigned long long)g_copies[i].addr, g_copies[i].is_double ? "f64" : "f32");
        snprintf(cursor, (size_t)(out + size - cursor), "]}\n");
    } else if (!strcmp(cmd, "obj_shift")) {
        /* Adds (dx,dy,dz) to every copy found by obj_copies and keeps writing the result for hold_ms, so per-frame
         * rebuilds pick the new position up instead of restoring the old one. */
        const double dx = arg_float(request, "dx", 0), dy = arg_float(request, "dy", 0), dz = arg_float(request, "dz", 0);
        long long hold = arg_int(request, "hold_ms", 300);
        if (hold < 0) hold = 0;
        if (hold > 3000) hold = 3000;
        static double targets[MAX_COPIES][3];
        uint32_t valid = 0;
        uint8_t ok[MAX_COPIES];
        for (uint32_t i = 0; i < g_copy_count; ++i) {
            double v[3];
            ok[i] = 0;
            if (g_copies[i].is_double) {
                if (read_mem(g_copies[i].addr, v, sizeof(v)) != (ssize_t)sizeof(v)) continue;
            } else {
                float f[3];
                if (read_mem(g_copies[i].addr, f, sizeof(f)) != (ssize_t)sizeof(f)) continue;
                v[0] = f[0]; v[1] = f[1]; v[2] = f[2];
            }
            targets[i][0] = v[0] + dx; targets[i][1] = v[1] + dy; targets[i][2] = v[2] + dz;
            ok[i] = 1;
            ++valid;
        }
        const uint64_t until = now_ms() + (uint64_t)hold;
        do {
            for (uint32_t i = 0; i < g_copy_count; ++i) {
                if (!ok[i]) continue;
                if (g_copies[i].is_double) write_mem(g_copies[i].addr, targets[i], sizeof(targets[i]));
                else {
                    const float f[3] = { (float)targets[i][0], (float)targets[i][1], (float)targets[i][2] };
                    write_mem(g_copies[i].addr, f, sizeof(f));
                }
            }
            if (hold) usleep(1000);
        } while (now_ms() < until);
        snprintf(out, size, "{\"ok\":true,\"shifted\":%u}\n", valid);
    } else if (!strcmp(cmd, "obj_near")) {
        /* Objects around a point (the player): one entry per object, entities before their transform holders. */
        const float centre[3] = { (float)arg_float(request, "x", 0), (float)arg_float(request, "y", 0), (float)arg_float(request, "z", 0) };
        const float radius = (float)arg_float(request, "radius", 50);
        long long limit = arg_int(request, "limit", 300);
        if (limit < 1 || limit > 300) limit = 300;
        const char *untyped_arg = beer_json_find(request, "untyped");
        const int untyped = untyped_arg && !strncmp(untyped_arg, "true", 4);   /* also bare transforms (physics, caches) */
        typedef struct { uint32_t index; float distance; } Near;
        Near *near = malloc(sizeof(Near) * 20000);
        uint32_t near_count = 0;
        for (uint32_t i = 0; near && i < g_instance_count && near_count < 20000; ++i) {
            const Group *g = &g_groups[g_instances[i].group];
            if (!g->vtable && !untyped) continue;
            float pos[3];
            if (!live_position(&g_instances[i], pos)) continue;
            const float d[3] = { pos[0] - centre[0], pos[1] - centre[1], pos[2] - centre[2] };
            const float distance = sqrtf(dot3f(d, d));
            if (distance <= radius) near[near_count++] = (Near){ i, distance };
        }
        /* Nearest first; entities ahead of holders at the same spot. */
        for (uint32_t a = 1; a < near_count; ++a) {
            const Near t = near[a];
            uint32_t b = a;
            while (b > 0 && (near[b - 1].distance > t.distance ||
                             (near[b - 1].distance == t.distance &&
                              g_groups[g_instances[near[b - 1].index].group].via < g_groups[g_instances[t.index].group].via))) {
                near[b] = near[b - 1];
                --b;
            }
            near[b] = t;
        }
        cursor += snprintf(cursor, (size_t)(end - cursor), "{\"ok\":true,\"total\":%u,\"items\":[", near_count);
        uint32_t emitted = 0;
        uint64_t *seen = malloc(sizeof(uint64_t) * 2 * (size_t)limit);
        for (uint32_t n = 0; seen && n < near_count && emitted < (uint32_t)limit && end - cursor > 1200; ++n) {
            const Instance *in = &g_instances[near[n].index];
            int duplicate = 0;
            for (uint32_t k = 0; k < emitted; ++k) if (seen[2 * k] == in->transform || seen[2 * k + 1] == in->base) duplicate = 1;
            if (duplicate) continue;
            seen[2 * emitted] = in->transform;
            seen[2 * emitted + 1] = in->base;
            const Group *g = &g_groups[in->group];
            char name[240];
            json_text(name, sizeof(name), g->name);
            cursor += snprintf(cursor, (size_t)(end - cursor),
                               "%s{\"group\":%u,\"name\":\"%s\",\"kind\":\"%s\",\"via\":%d,\"index\":%u,\"base\":\"0x%llx\",\"transform\":\"0x%llx\",\"distance\":%.2f,\"v\":",
                               emitted++ ? "," : "", in->group, name, kind_names[g->kind], g->via, near[n].index - g->first,
                               (unsigned long long)in->base, (unsigned long long)in->transform, (double)near[n].distance);
            cursor += transform_json(cursor, (size_t)(end - cursor), in->transform, g->kind);
            *cursor++ = '}';
        }
        snprintf(cursor, (size_t)(out + size - cursor), "]}\n");
        free(seen);
        free(near);
    } else {
        reply_error(out, size, "unknown obj command");
    }
done:
    pthread_mutex_unlock(&g_lock);
}
