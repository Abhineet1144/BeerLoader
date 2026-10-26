/* Shader-constant finder and tweaks.
 *
 * Every draw resolves its constant buffers through bd12_resolve_descriptor_set; the hook there calls
 * bd12_cbt_observe for each CBV that lives in CPU-visible memory. While capture is on, the first sample of each
 * (shader, stage, slot, size) per frame is kept so the manager can browse and scan the floats (like a memory
 * scan, but over shader constants). Tweaks overwrite chosen floats in that memory right before the draw reads it.
 */
#include "bd12.h"
#include "../../platform/beer_control.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_MAX 4096u           /* bytes kept per constant buffer */
#define MAX_ENTRIES 1024
#define MAX_TWEAKS 64
#define MAX_CANDIDATES (3u * 1024u * 1024u)
#define ORIG_SLOTS 8192

int g_bd12_cbt_active;

typedef struct {
    uint64_t id, pso;
    uint32_t stage, space, reg, size;
    uint32_t draws, sample_len, gpu_only;
    uint8_t used;
    uint8_t sample[SAMPLE_MAX];
} Entry;

typedef struct { uint64_t id; uint32_t index; float value; } Candidate;

typedef struct {
    int used, enabled;
    uint32_t tid;
    uint64_t pso;                  /* 0 = any shader */
    uint32_t stage, space, reg, size, offset;
    int mode;                      /* 0 set, 1 scale, 2 add */
    float value;
    char label[48];
    uint32_t hits, last_hits;
} Tweak;

typedef struct { uintptr_t addr; uint32_t tid; uint32_t orig, written; } OrigSlot;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int capture_on;
static Entry *cur, *last;
static uint32_t cur_overflow;
static Tweak tweaks[MAX_TWEAKS];
static uint32_t next_tid = 1;
static OrigSlot orig_slots[ORIG_SLOTS];
static Candidate *cands;
static size_t cand_count;
static int scan_active;

static const char *const stage_names[] = { "compute", "vertex", "pixel", "?", "?", "geometry", "hull", "domain" };

static uint64_t make_id(uint64_t pso, uint32_t stage, uint32_t space, uint32_t reg, uint32_t size)
{
    uint64_t h = 1469598103934665603ull;
    const uint64_t parts[5] = { pso, stage, space, reg, size };
    for (int i = 0; i < 5; ++i) { h ^= parts[i]; h *= 1099511628211ull; h ^= h >> 29; }
    return h ? h : 1;
}

static void refresh_active(void)
{
    int active = capture_on;
    for (int i = 0; i < MAX_TWEAKS; ++i) if (tweaks[i].used && tweaks[i].enabled) active = 1;
    __atomic_store_n(&g_bd12_cbt_active, active, __ATOMIC_RELAXED);
}

static Entry *find_entry(Entry *table, uint64_t id, int create)
{
    if (!table) return NULL;
    uint32_t slot = (uint32_t)(id % MAX_ENTRIES);
    for (uint32_t probe = 0; probe < MAX_ENTRIES; ++probe, slot = (slot + 1) % MAX_ENTRIES) {
        Entry *e = &table[slot];
        if (e->used && e->id == id) return e;
        if (!e->used) {
            if (!create) return NULL;
            return e;
        }
    }
    return NULL;
}

static OrigSlot *orig_for(uintptr_t addr, uint32_t tid)
{
    uint32_t slot = (uint32_t)(((addr >> 2) ^ (tid * 2654435761u)) % ORIG_SLOTS);
    for (uint32_t probe = 0; probe < 16; ++probe, slot = (slot + 1) % ORIG_SLOTS) {
        OrigSlot *o = &orig_slots[slot];
        if (o->addr == addr && o->tid == tid) return o;
        if (!o->addr) return o;
    }
    return &orig_slots[(uint32_t)((addr >> 2) % ORIG_SLOTS)];   /* table crowded: reuse */
}

void bd12_cbt_observe(uint64_t pso_hash, uint32_t stage, uint32_t space, uint32_t reg,
                      BD12Buffer *buffer, VkDeviceSize offset, VkDeviceSize range)
{
    if (!buffer || range == 0) return;
    pthread_mutex_lock(&lock);
    const uint32_t size = (uint32_t)(range > 0xffffffffu ? 0xffffffffu : range);
    if (capture_on && cur) {
        const uint64_t id = make_id(pso_hash, stage, space, reg, size);
        Entry *e = find_entry(cur, id, 1);
        if (!e) ++cur_overflow;
        else {
            if (!e->used) {
                memset(e, 0, offsetof(Entry, sample));
                e->used = 1; e->id = id; e->pso = pso_hash; e->stage = stage; e->space = space; e->reg = reg; e->size = size;
                if (buffer->cpu) {
                    e->sample_len = size < SAMPLE_MAX ? size : SAMPLE_MAX;
                    memcpy(e->sample, (const uint8_t *)buffer->cpu + offset, e->sample_len);
                } else e->gpu_only = 1;
            }
            ++e->draws;
        }
    }
    if (buffer->cpu) {
        for (int i = 0; i < MAX_TWEAKS; ++i) {
            Tweak *t = &tweaks[i];
            if (!t->used || !t->enabled || t->stage != stage || t->reg != reg || t->space != space) continue;
            if (t->pso && t->pso != pso_hash) continue;
            if (t->size && t->size != size) continue;
            if ((uint64_t)t->offset + 4 > range) continue;
            uint32_t *word = (uint32_t *)((uint8_t *)buffer->cpu + offset + t->offset);
            uint32_t bits = *word;
            float result = t->value;
            if (t->mode != 0) {
                OrigSlot *o = orig_for((uintptr_t)word, t->tid);
                uint32_t original = bits;
                if (o->addr == (uintptr_t)word && o->tid == t->tid && o->written == bits) original = o->orig;   /* our own earlier write */
                float f;
                memcpy(&f, &original, 4);
                result = t->mode == 1 ? f * t->value : f + t->value;
                uint32_t out_bits;
                memcpy(&out_bits, &result, 4);
                o->addr = (uintptr_t)word; o->tid = t->tid; o->orig = original; o->written = out_bits;
            }
            memcpy(word, &result, 4);
            ++t->hits;
        }
    }
    pthread_mutex_unlock(&lock);
}

void bd12_cbt_frame_advance(void)
{
    if (!__atomic_load_n(&g_bd12_cbt_active, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&lock);
    if (capture_on && cur && last) {
        Entry *swap = last;
        last = cur;
        cur = swap;
        memset(cur, 0, sizeof(Entry) * MAX_ENTRIES);
        cur_overflow = 0;
    }
    for (int i = 0; i < MAX_TWEAKS; ++i) { tweaks[i].last_hits = tweaks[i].hits; tweaks[i].hits = 0; }
    pthread_mutex_unlock(&lock);
}

/* ---- commands ---- */

static void error_reply(char *out, size_t size, const char *message)
{
    snprintf(out, size, "{\"ok\":false,\"error\":\"%s\"}\n", message);
}

static uint64_t arg_id(const char *request, const char *key, int *found)
{
    char text[40];
    *found = beer_json_string(request, key, text, sizeof(text));
    return *found ? strtoull(text, NULL, 16) : 0;
}

static int arg_milli(const char *request, const char *key, int32_t *out) { return beer_json_milli(request, key, out); }

static int arg_float(const char *request, const char *key, float *out)
{
    char text[48] = "";
    beer_json_string(request, key, text, sizeof(text));
    if (!text[0]) {
        const char *p = beer_json_find(request, key);
        if (!p || *p == '"') return 0;
        size_t n = strcspn(p, ",}");
        if (n >= sizeof(text)) n = sizeof(text) - 1;
        memcpy(text, p, n);
        text[n] = 0;
    }
    char *stop;
    const double v = strtod(text, &stop);
    if (stop == text) return 0;
    *out = (float)v;
    return 1;
}

static const char *stage_name(uint32_t stage) { return stage < 8 ? stage_names[stage] : "?"; }

static float sample_float(const Entry *e, uint32_t index)
{
    float f;
    memcpy(&f, e->sample + (size_t)index * 4, 4);
    return f;
}

static void put_result(char **c, char *end, const Entry *e, uint32_t index, float value, int comma)
{
    if (end - *c < 260) return;
    *c += snprintf(*c, (size_t)(end - *c),
                   "%s{\"id\":\"%llx\",\"pso\":\"%llx\",\"stage\":\"%s\",\"reg\":%u,\"space\":%u,\"size\":%u,\"offset\":%u,\"value\":%.6g,\"draws\":%u}",
                   comma ? "," : "", (unsigned long long)e->id, (unsigned long long)e->pso, stage_name(e->stage), e->reg, e->space, e->size,
                   index * 4, (double)value, e->draws);
}

static void reply_candidates(char *out, size_t size, size_t offset, size_t limit)
{
    char *c = out, *end = out + size - 64;
    c += snprintf(c, (size_t)(end - c), "{\"ok\":true,\"count\":%zu,\"results\":[", cand_count);
    int first = 1;
    for (size_t i = offset; i < cand_count && i < offset + limit && end - c > 300; ++i) {
        const Entry *e = find_entry(last, cands[i].id, 0);
        if (!e || !e->used) continue;
        const float live = cands[i].index * 4u + 4u <= e->sample_len ? sample_float(e, cands[i].index) : cands[i].value;
        put_result(&c, end, e, cands[i].index, live, !first);
        first = 0;
    }
    snprintf(c, (size_t)(out + size - c), "]}\n");
}

static int floats_equal(float a, float b)
{
    const float d = fabsf(a - b);
    return d <= 0.0005f * fabsf(b) + 1e-6f;
}

void bd12_cbt_handle(const char *cmd, const char *request, char *out, size_t size)
{
    pthread_mutex_lock(&lock);
    if (!strcmp(cmd, "gfx_capture")) {
        int32_t on = 0;
        arg_milli(request, "on", &on);
        if (on && !capture_on) {
            cur = calloc(MAX_ENTRIES, sizeof(Entry));
            last = calloc(MAX_ENTRIES, sizeof(Entry));
            if (!cur || !last) { free(cur); free(last); cur = last = NULL; error_reply(out, size, "out of memory"); goto done; }
            capture_on = 1;
        } else if (!on && capture_on) {
            capture_on = 0;
            free(cur); free(last);
            cur = last = NULL;
            free(cands); cands = NULL; cand_count = 0; scan_active = 0;
        }
        refresh_active();
        snprintf(out, size, "{\"ok\":true,\"capture\":%s}\n", capture_on ? "true" : "false");
    } else if (!strcmp(cmd, "gfx_cbs")) {
        if (!capture_on) { error_reply(out, size, "turn capture on first"); goto done; }
        char *c = out, *end = out + size - 64;
        uint32_t keys = 0, gpu_only = 0;
        for (uint32_t i = 0; i < MAX_ENTRIES; ++i) if (last[i].used) { ++keys; gpu_only += last[i].gpu_only; }
        c += snprintf(c, (size_t)(end - c), "{\"ok\":true,\"keys\":%u,\"gpu_only\":%u,\"cbs\":[", keys, gpu_only);
        int first = 1;
        for (uint32_t i = 0; i < MAX_ENTRIES && end - c > 260; ++i) {
            const Entry *e = &last[i];
            if (!e->used) continue;
            c += snprintf(c, (size_t)(end - c), "%s{\"id\":\"%llx\",\"pso\":\"%llx\",\"stage\":\"%s\",\"reg\":%u,\"space\":%u,\"size\":%u,\"draws\":%u,\"gpu_only\":%s}",
                          first ? "" : ",", (unsigned long long)e->id, (unsigned long long)e->pso, stage_name(e->stage), e->reg, e->space, e->size,
                          e->draws, e->gpu_only ? "true" : "false");
            first = 0;
        }
        snprintf(c, (size_t)(out + size - c), "]}\n");
    } else if (!strcmp(cmd, "gfx_cb_read")) {
        int found;
        const uint64_t id = arg_id(request, "id", &found);
        const Entry *e = capture_on && found ? find_entry(last, id, 0) : NULL;
        if (!e || !e->used) { error_reply(out, size, "that constant buffer was not drawn in the last frame"); goto done; }
        char *c = out, *end = out + size - 64;
        c += snprintf(c, (size_t)(end - c), "{\"ok\":true,\"size\":%u,\"floats\":[", e->size);
        for (uint32_t i = 0; i < e->sample_len / 4 && end - c > 40; ++i)
            c += snprintf(c, (size_t)(end - c), "%s%.6g", i ? "," : "", (double)sample_float(e, i));
        snprintf(c, (size_t)(out + size - c), "]}\n");
    } else if (!strcmp(cmd, "gfx_scan")) {
        if (!capture_on) { error_reply(out, size, "turn capture on first"); goto done; }
        free(cands); cands = NULL; cand_count = 0;
        cands = malloc(sizeof(Candidate) * 65536u);
        size_t capacity = 65536u;
        if (!cands) { error_reply(out, size, "out of memory"); goto done; }
        for (uint32_t i = 0; i < MAX_ENTRIES; ++i) {
            const Entry *e = &last[i];
            if (!e->used || e->gpu_only) continue;
            for (uint32_t k = 0; k < e->sample_len / 4; ++k) {
                const float v = sample_float(e, k);
                if (v != v || !isfinite(v)) continue;
                if (cand_count == capacity) {
                    if (capacity >= MAX_CANDIDATES) goto scan_full;
                    capacity *= 2;
                    Candidate *grown = realloc(cands, sizeof(Candidate) * capacity);
                    if (!grown) goto scan_full;
                    cands = grown;
                }
                cands[cand_count++] = (Candidate){ e->id, k, v };
            }
        }
    scan_full:
        scan_active = 1;
        reply_candidates(out, size, 0, 300);
    } else if (!strcmp(cmd, "gfx_next")) {
        if (!scan_active) { error_reply(out, size, "start a scan first"); goto done; }
        char mode[24] = "exact";
        beer_json_string(request, "mode", mode, sizeof(mode));
        float wanted = 0, at_least = 0, up_to = 0;
        const int has_value = arg_float(request, "value", &wanted);
        const int has_min = arg_float(request, "at_least", &at_least), has_max = arg_float(request, "up_to", &up_to);
        if (!strcmp(mode, "exact") && !has_value) { error_reply(out, size, "exact needs a value"); goto done; }
        size_t kept = 0;
        for (size_t i = 0; i < cand_count; ++i) {
            const Entry *e = find_entry(last, cands[i].id, 0);
            if (!e || !e->used || cands[i].index * 4u + 4u > e->sample_len) continue;
            const float now = sample_float(e, cands[i].index), before = cands[i].value;
            if (now != now || !isfinite(now)) continue;
            const float delta = now - before, magnitude = fabsf(delta);
            const int moved = magnitude > 1e-5f * (fabsf(before) > 1.0f ? fabsf(before) : 1.0f);
            int keep = 0;
            if (!strcmp(mode, "exact")) keep = floats_equal(now, wanted);
            else if (!strcmp(mode, "unchanged")) keep = !moved;
            else if (!strcmp(mode, "changed") || !strcmp(mode, "increased") || !strcmp(mode, "decreased")) {
                keep = moved && (strcmp(mode, "increased") || delta > 0) && (strcmp(mode, "decreased") || delta < 0) &&
                       (!has_max || magnitude <= up_to) && (!has_min || magnitude >= at_least);
            }
            if (!keep) continue;
            cands[kept] = cands[i];
            cands[kept].value = now;
            ++kept;
        }
        cand_count = kept;
        reply_candidates(out, size, 0, 300);
    } else if (!strcmp(cmd, "gfx_results")) {
        if (!scan_active) { error_reply(out, size, "no scan in progress"); goto done; }
        reply_candidates(out, size, 0, 300);
    } else if (!strcmp(cmd, "gfx_scan_reset")) {
        free(cands); cands = NULL; cand_count = 0; scan_active = 0;
        snprintf(out, size, "{\"ok\":true}\n");
    } else if (!strcmp(cmd, "gfx_tweak_set")) {
        /* Either a known constant buffer id (scope "shader" = that shader only, "slot" = any shader with the same slot and size)
         * or explicit pso/stage/reg/space/size (used when restoring saved tweaks). */
        Tweak t;
        memset(&t, 0, sizeof(t));
        int found;
        const uint64_t id = arg_id(request, "id", &found);
        char scope[12] = "shader", mode[8] = "set", stage_text[16] = "";
        beer_json_string(request, "scope", scope, sizeof(scope));
        beer_json_string(request, "mode", mode, sizeof(mode));
        beer_json_string(request, "label", t.label, sizeof(t.label));
        int32_t offset_milli = 0;
        if (!arg_float(request, "value", &t.value) || !arg_milli(request, "offset", &offset_milli)) { error_reply(out, size, "needs value and offset"); goto done; }
        t.offset = (uint32_t)(offset_milli / 1000);
        t.mode = !strcmp(mode, "scale") ? 1 : !strcmp(mode, "add") ? 2 : 0;
        if (found) {
            const Entry *e = capture_on ? find_entry(last, id, 0) : NULL;
            if (!e || !e->used) { error_reply(out, size, "that constant buffer was not drawn in the last frame"); goto done; }
            t.pso = !strcmp(scope, "slot") ? 0 : e->pso;
            t.stage = e->stage; t.space = e->space; t.reg = e->reg; t.size = e->size;
        } else {
            char pso_text[40] = "";
            int32_t v;
            beer_json_string(request, "pso", pso_text, sizeof(pso_text));
            t.pso = strtoull(pso_text, NULL, 16);
            beer_json_string(request, "stage", stage_text, sizeof(stage_text));
            for (uint32_t s = 0; s < 8; ++s) if (!strcmp(stage_text, stage_names[s])) t.stage = s;
            if (arg_milli(request, "reg", &v)) t.reg = (uint32_t)(v / 1000);
            if (arg_milli(request, "space", &v)) t.space = (uint32_t)(v / 1000);
            if (arg_milli(request, "size", &v)) t.size = (uint32_t)(v / 1000);
        }
        t.used = 1; t.enabled = 1;
        int slot = -1;
        for (int i = 0; i < MAX_TWEAKS; ++i) {   /* same target replaces the previous tweak */
            if (tweaks[i].used && tweaks[i].pso == t.pso && tweaks[i].stage == t.stage && tweaks[i].reg == t.reg &&
                tweaks[i].space == t.space && tweaks[i].size == t.size && tweaks[i].offset == t.offset) { slot = i; break; }
            if (!tweaks[i].used && slot < 0) slot = i;
        }
        if (slot < 0) { error_reply(out, size, "too many tweaks (max 64)"); goto done; }
        t.tid = tweaks[slot].used ? tweaks[slot].tid : next_tid++;
        tweaks[slot] = t;
        refresh_active();
        snprintf(out, size, "{\"ok\":true,\"tid\":%u}\n", t.tid);
    } else if (!strcmp(cmd, "gfx_tweak_update") || !strcmp(cmd, "gfx_tweak_remove") || !strcmp(cmd, "gfx_tweak_enable")) {
        int32_t tid_milli = 0;
        if (!arg_milli(request, "tid", &tid_milli)) { error_reply(out, size, "tid missing"); goto done; }
        const uint32_t tid = (uint32_t)(tid_milli / 1000);
        Tweak *t = NULL;
        for (int i = 0; i < MAX_TWEAKS; ++i) if (tweaks[i].used && tweaks[i].tid == tid) t = &tweaks[i];
        if (!t) { error_reply(out, size, "no such tweak"); goto done; }
        if (!strcmp(cmd, "gfx_tweak_remove")) t->used = 0;
        else if (!strcmp(cmd, "gfx_tweak_enable")) { int32_t on = 1; arg_milli(request, "on", &on); t->enabled = on != 0; }
        else { float v; if (arg_float(request, "value", &v)) t->value = v; }
        refresh_active();
        snprintf(out, size, "{\"ok\":true}\n");
    } else if (!strcmp(cmd, "gfx_tweaks")) {
        char *c = out, *end = out + size - 64;
        c += snprintf(c, (size_t)(end - c), "{\"ok\":true,\"tweaks\":[");
        int first = 1;
        for (int i = 0; i < MAX_TWEAKS && end - c > 300; ++i) {
            const Tweak *t = &tweaks[i];
            if (!t->used) continue;
            c += snprintf(c, (size_t)(end - c),
                          "%s{\"tid\":%u,\"label\":\"%s\",\"pso\":\"%llx\",\"stage\":\"%s\",\"reg\":%u,\"space\":%u,\"size\":%u,\"offset\":%u,\"mode\":\"%s\",\"value\":%.6g,\"enabled\":%s,\"hits\":%u}",
                          first ? "" : ",", t->tid, t->label, (unsigned long long)t->pso, stage_name(t->stage), t->reg, t->space, t->size, t->offset,
                          t->mode == 1 ? "scale" : t->mode == 2 ? "add" : "set", (double)t->value, t->enabled ? "true" : "false", t->last_hits);
            first = 0;
        }
        snprintf(c, (size_t)(out + size - c), "]}\n");
    } else {
        error_reply(out, size, "unknown graphics command");
    }
done:
    pthread_mutex_unlock(&lock);
}
