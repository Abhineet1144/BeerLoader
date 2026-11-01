#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "beer_steam_mms.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CMsgClientMMS* message numbers (requests, replies and unsolicited events). */
enum {
    MMS_CREATE_LOBBY = 6601, MMS_CREATE_LOBBY_RESPONSE = 6602,
    MMS_JOIN_LOBBY = 6603, MMS_JOIN_LOBBY_RESPONSE = 6604,
    MMS_LEAVE_LOBBY = 6605, MMS_LEAVE_LOBBY_RESPONSE = 6606,
    MMS_GET_LOBBY_LIST = 6607, MMS_GET_LOBBY_LIST_RESPONSE = 6608,
    MMS_SET_LOBBY_DATA = 6609, MMS_SET_LOBBY_DATA_RESPONSE = 6610,
    MMS_GET_LOBBY_DATA = 6611, MMS_LOBBY_DATA = 6612,
    MMS_SEND_LOBBY_CHAT_MSG = 6613, MMS_LOBBY_CHAT_MSG = 6614,
    MMS_SET_LOBBY_OWNER = 6615, MMS_SET_LOBBY_OWNER_RESPONSE = 6616,
    MMS_SET_LOBBY_GAME_SERVER = 6617, MMS_LOBBY_GAME_SERVER_SET = 6618,
    MMS_USER_JOINED_LOBBY = 6619, MMS_USER_LEFT_LOBBY = 6620,
    MMS_INVITE_TO_LOBBY = 6621,
};

#define CALL_BASE UINT64_C(0x4d4d530000000000)      /* "MMS": distinct from other API call handles */
#define MAX_LOBBIES 64
#define MAX_LIST 256
#define MAX_CALLS 64
#define MAX_CALLBACKS 256
#define MAX_CHAT 64
#define MAX_FILTERS 32
#define MAX_PERSONAS 512
#define REPLY_CAP (1u << 20)

/* ---- protobuf ---- */
typedef struct { uint8_t *d; size_t n, cap; } Pb;

static void pb_raw(Pb *b, const void *p, size_t len)
{
    if (b->n + len > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + len) cap *= 2;
        uint8_t *grown = realloc(b->d, cap);
        if (!grown) return;
        b->d = grown;
        b->cap = cap;
    }
    memcpy(b->d + b->n, p, len);
    b->n += len;
}
static void pb_varint(Pb *b, uint64_t v)
{
    uint8_t tmp[10];
    size_t n = 0;
    do { tmp[n] = (uint8_t)(v & 0x7f); v >>= 7; if (v) tmp[n] |= 0x80; n++; } while (v);
    pb_raw(b, tmp, n);
}
static void pb_u(Pb *b, uint32_t field, uint64_t v) { pb_varint(b, (uint64_t)field << 3); pb_varint(b, v); }
static void pb_i(Pb *b, uint32_t field, int32_t v) { pb_u(b, field, (uint64_t)(int64_t)v); }
static void pb_f64(Pb *b, uint32_t field, uint64_t v) { pb_varint(b, (uint64_t)field << 3 | 1); pb_raw(b, &v, 8); }
static void pb_bytes(Pb *b, uint32_t field, const void *p, size_t len)
{
    pb_varint(b, (uint64_t)field << 3 | 2);
    pb_varint(b, len);
    pb_raw(b, p, len);
}
static void pb_str(Pb *b, uint32_t field, const char *s) { if (s) pb_bytes(b, field, s, strlen(s)); }

typedef struct { const uint8_t *p, *end; } PbIn;
typedef struct { uint32_t field; int type; uint64_t v; const uint8_t *data; size_t len; } PbF;

static int pb_get_varint(PbIn *r, uint64_t *v)
{
    *v = 0;
    for (int shift = 0; shift < 64 && r->p < r->end; shift += 7) {
        uint8_t byte = *r->p++;
        *v |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return 1;
    }
    return 0;
}
static int pb_next(PbIn *r, PbF *f)
{
    if (r->p >= r->end) return 0;
    uint64_t key;
    if (!pb_get_varint(r, &key)) return -1;
    f->field = (uint32_t)(key >> 3);
    f->type = (int)(key & 7);
    f->data = NULL;
    f->len = 0;
    switch (f->type) {
    case 0: return pb_get_varint(r, &f->v) ? 1 : -1;
    case 1: if (r->end - r->p < 8) return -1; memcpy(&f->v, r->p, 8); r->p += 8; return 1;
    case 5: if (r->end - r->p < 4) return -1; { uint32_t x; memcpy(&x, r->p, 4); f->v = x; } r->p += 4; return 1;
    case 2:
        if (!pb_get_varint(r, &f->v) || f->v > (uint64_t)(r->end - r->p)) return -1;
        f->data = r->p;
        f->len = (size_t)f->v;
        r->p += f->len;
        return 1;
    default: return -1;
    }
}
/* ---- key/value lists and binary KeyValues (lobby metadata) ---- */
typedef struct { char *key, *value; } Kv;
typedef struct { Kv *items; int count; } KvList;

static void kv_clear(KvList *l)
{
    for (int i = 0; i < l->count; i++) { free(l->items[i].key); free(l->items[i].value); }
    free(l->items);
    l->items = NULL;
    l->count = 0;
}
static const char *kv_get(const KvList *l, const char *key)
{
    for (int i = 0; i < l->count; i++) if (!strcmp(l->items[i].key, key)) return l->items[i].value;
    return NULL;
}
static int kv_set(KvList *l, const char *key, const char *value)
{
    for (int i = 0; i < l->count; i++)
        if (!strcmp(l->items[i].key, key)) {
            char *copy = strdup(value);
            if (!copy) return 0;
            free(l->items[i].value);
            l->items[i].value = copy;
            return 1;
        }
    Kv *grown = realloc(l->items, (size_t)(l->count + 1) * sizeof(*grown));
    if (!grown) return 0;
    l->items = grown;
    l->items[l->count].key = strdup(key);
    l->items[l->count].value = strdup(value);
    if (!l->items[l->count].key || !l->items[l->count].value) {
        free(l->items[l->count].key);
        free(l->items[l->count].value);
        return 0;
    }
    l->count++;
    return 1;
}
static int kv_delete(KvList *l, const char *key)
{
    for (int i = 0; i < l->count; i++)
        if (!strcmp(l->items[i].key, key)) {
            free(l->items[i].key);
            free(l->items[i].value);
            l->items[i] = l->items[--l->count];
            return 1;
        }
    return 0;
}

/* Binary KeyValues: an unnamed root object holding string entries. */
static void kv_encode(const KvList *l, Pb *out)
{
    static const uint8_t root[2] = { 0x00, 0x00 }, end[2] = { 0x08, 0x08 };
    pb_raw(out, root, 2);
    for (int i = 0; i < l->count; i++) {
        static const uint8_t string_type = 0x01;
        pb_raw(out, &string_type, 1);
        pb_raw(out, l->items[i].key, strlen(l->items[i].key) + 1);
        pb_raw(out, l->items[i].value, strlen(l->items[i].value) + 1);
    }
    pb_raw(out, end, 2);
}

static const uint8_t *kv_cstr(const uint8_t *p, const uint8_t *end, const char **s)
{
    const uint8_t *z = memchr(p, 0, (size_t)(end - p));
    if (!z) return NULL;
    *s = (const char *)p;
    return z + 1;
}
/* Reads the entries of one object; nested objects are skipped, numbers become text. */
static const uint8_t *kv_decode_object(const uint8_t *p, const uint8_t *end, KvList *out, int depth)
{
    while (p && p < end) {
        uint8_t type = *p++;
        if (type == 0x08 || type == 0x0b) return p;
        const char *key;
        if (!(p = kv_cstr(p, end, &key))) return NULL;
        char number[32];
        switch (type) {
        case 0x00:
            if (depth > 8) return NULL;
            p = kv_decode_object(p, end, NULL, depth + 1);
            break;
        case 0x01: {
            const char *value;
            if (!(p = kv_cstr(p, end, &value))) return NULL;
            if (out) kv_set(out, key, value);
            break;
        }
        case 0x02: case 0x04: case 0x06: {
            if (end - p < 4) return NULL;
            int32_t v; memcpy(&v, p, 4); p += 4;
            snprintf(number, sizeof(number), "%d", v);
            if (out && type == 0x02) kv_set(out, key, number);
            break;
        }
        case 0x03: {
            if (end - p < 4) return NULL;
            float v; memcpy(&v, p, 4); p += 4;
            snprintf(number, sizeof(number), "%g", (double)v);
            if (out) kv_set(out, key, number);
            break;
        }
        case 0x07: case 0x0a: {
            if (end - p < 8) return NULL;
            uint64_t v; memcpy(&v, p, 8); p += 8;
            snprintf(number, sizeof(number), type == 0x07 ? "%llu" : "%lld", (unsigned long long)v);
            if (out) kv_set(out, key, number);
            break;
        }
        default: return NULL;
        }
    }
    return p;
}
static void kv_decode(const uint8_t *data, size_t len, KvList *out)
{
    kv_clear(out);
    if (len < 2) return;
    const uint8_t *p = data, *end = data + len;
    if (*p++ != 0x00) return;                          /* root object */
    const char *name;
    if (!(p = kv_cstr(p, end, &name))) return;
    kv_decode_object(p, end, out, 0);
}

/* ---- state ---- */
typedef struct {
    uint64_t id;
    char persona[128];
    KvList data;
} Member;

typedef struct {
    uint64_t id, owner;
    int max_members, type, flags, entered, joinable, used;
    KvList data;
    Member *members;
    int member_count;
    uint32_t server_ip;
    uint16_t server_port;
    uint64_t server_id;
    int has_server;
} Lobby;

typedef struct { uint64_t handle; uint32_t id, size; uint8_t data[48]; int done, failed, used; } Call;
typedef struct { uint64_t lobby, user; int type, len; uint8_t *data; } ChatEntry;
typedef struct { char key[256], value[256]; int comparison, type; } Filter;

typedef enum { JOB_LIST, JOB_CREATE, JOB_JOIN, JOB_LEAVE, JOB_PUSH_DATA, JOB_PUSH_MEMBER, JOB_SET_OWNER } JobType;
typedef struct Job {
    JobType type;
    uint64_t call, lobby, user;
    int a, b;
    Filter *filters;
    int filter_count, result_count;
    struct Job *next;
} Job;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t work;
    int ready;
    BeerMmsTransport io;
    uint32_t appid;
    uint64_t self;
    char persona[128];
    Lobby lobbies[MAX_LOBBIES];
    uint64_t list[MAX_LIST];
    int list_count;
    Call calls[MAX_CALLS];
    uint64_t next_call;
    BeerMmsCallback callbacks[MAX_CALLBACKS];
    int callback_head, callback_count;
    ChatEntry chat[MAX_CHAT];
    int chat_next;
    Filter filters[MAX_FILTERS];
    int filter_count, result_count;
    struct { uint64_t id; char name[128]; } personas[MAX_PERSONAS];
    int persona_count;
    Job *jobs, **jobs_tail;
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .work = PTHREAD_COND_INITIALIZER };

/* Caller holds g.lock. */
static void post_locked(uint32_t id, const void *data, uint32_t size)
{
    if (size > sizeof(g.callbacks[0].data)) return;
    if (g.callback_count == MAX_CALLBACKS) {                  /* full: drop the oldest */
        g.callback_head = (g.callback_head + 1) % MAX_CALLBACKS;
        g.callback_count--;
    }
    BeerMmsCallback *cb = &g.callbacks[(g.callback_head + g.callback_count) % MAX_CALLBACKS];
    cb->id = id;
    cb->size = size;
    memset(cb->data, 0, sizeof(cb->data));
    memcpy(cb->data, data, size);
    g.callback_count++;
}

static uint64_t new_call_locked(uint32_t id, uint32_t size)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        Call *c = &g.calls[(g.next_call + (uint64_t)i) % MAX_CALLS];
        if (c->used) continue;
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->handle = CALL_BASE | ++g.next_call;
        c->id = id;
        c->size = size;
        return c->handle;
    }
    return 0;
}
static Call *call_locked(uint64_t handle)
{
    for (int i = 0; i < MAX_CALLS; i++) if (g.calls[i].used && g.calls[i].handle == handle) return &g.calls[i];
    return NULL;
}
/* Completes an API call and announces it with SteamAPICallCompleted_t, as Steam does. */
static void complete_locked(uint64_t handle, const void *result, uint32_t size, int failed)
{
    Call *c = call_locked(handle);
    if (!c) return;
    if (result && size <= sizeof(c->data)) memcpy(c->data, result, size);
    c->done = 1;
    c->failed = failed;
    uint8_t done[16] = { 0 };
    memcpy(done, &handle, 8);
    memcpy(done + 8, &c->id, 4);
    memcpy(done + 12, &c->size, 4);
    post_locked(BEER_CB_API_CALL_COMPLETED, done, sizeof(done));
}

static Lobby *lobby_locked(uint64_t id, int create)
{
    Lobby *spare = NULL;
    for (int i = 0; i < MAX_LOBBIES; i++) {
        if (g.lobbies[i].used && g.lobbies[i].id == id) return &g.lobbies[i];
        if (!g.lobbies[i].used && !spare) spare = &g.lobbies[i];
    }
    if (!create) return NULL;
    if (!spare)                                              /* recycle a list result that was never entered */
        for (int i = 0; i < MAX_LOBBIES && !spare; i++) if (!g.lobbies[i].entered) spare = &g.lobbies[i];
    if (!spare) return NULL;
    kv_clear(&spare->data);
    for (int i = 0; i < spare->member_count; i++) kv_clear(&spare->members[i].data);
    free(spare->members);
    memset(spare, 0, sizeof(*spare));
    spare->used = 1;
    spare->id = id;
    spare->joinable = 1;
    return spare;
}
static void lobby_forget_locked(Lobby *l)
{
    kv_clear(&l->data);
    for (int i = 0; i < l->member_count; i++) kv_clear(&l->members[i].data);
    free(l->members);
    memset(l, 0, sizeof(*l));
}

static void remember_persona_locked(uint64_t id, const char *name)
{
    if (!id || !name || !*name) return;
    int slot = -1;
    for (int i = 0; i < g.persona_count; i++) if (g.personas[i].id == id) slot = i;
    if (slot < 0) {
        if (g.persona_count == MAX_PERSONAS) return;
        slot = g.persona_count++;
    }
    g.personas[slot].id = id;
    snprintf(g.personas[slot].name, sizeof(g.personas[slot].name), "%s", name);
}

static Member *member_locked(Lobby *l, uint64_t id, int create)
{
    for (int i = 0; i < l->member_count; i++) if (l->members[i].id == id) return &l->members[i];
    if (!create) return NULL;
    Member *grown = realloc(l->members, (size_t)(l->member_count + 1) * sizeof(*grown));
    if (!grown) return NULL;
    l->members = grown;
    Member *m = &l->members[l->member_count++];
    memset(m, 0, sizeof(*m));
    m->id = id;
    return m;
}
static void member_remove_locked(Lobby *l, uint64_t id)
{
    for (int i = 0; i < l->member_count; i++)
        if (l->members[i].id == id) {
            kv_clear(&l->members[i].data);
            l->members[i] = l->members[--l->member_count];
            return;
        }
}

/* Members of a JoinLobbyResponse / LobbyData (field members): { steam_id = 1, persona_name = 2, metadata = 3 }. */
static void read_member_locked(Lobby *l, const uint8_t *data, size_t len)
{
    PbIn r = { data, data + len };
    PbF f;
    uint64_t id = 0;
    const uint8_t *meta = NULL;
    size_t meta_len = 0;
    char name[128] = "";
    while (pb_next(&r, &f) == 1) {
        if (f.field == 1) id = f.v;
        else if (f.field == 2 && f.type == 2) snprintf(name, sizeof(name), "%.*s", (int)f.len, (const char *)f.data);
        else if (f.field == 3 && f.type == 2) { meta = f.data; meta_len = f.len; }
    }
    Member *m = id ? member_locked(l, id, 1) : NULL;
    if (!m) return;
    if (*name) snprintf(m->persona, sizeof(m->persona), "%s", name);
    remember_persona_locked(id, name);
    if (meta) kv_decode(meta, meta_len, &m->data);
}

static void lobby_data_update_locked(uint64_t lobby, uint64_t member, int success)
{
    uint8_t cb[24] = { 0 };
    memcpy(cb, &lobby, 8);
    memcpy(cb + 8, &member, 8);
    cb[16] = (uint8_t)(success != 0);
    post_locked(BEER_CB_LOBBY_DATA_UPDATE, cb, sizeof(cb));
}

static void lobby_enter_cb(uint8_t out[24], uint64_t lobby, uint32_t response)
{
    memset(out, 0, 24);
    memcpy(out, &lobby, 8);                                  /* m_ulSteamIDLobby */
    memcpy(out + 16, &response, 4);                          /* m_EChatRoomEnterResponse; perms 0, unlocked */
}

/* ---- worker: request/reply operations ---- */
static void lobby_state_body_locked(const Lobby *l, uint64_t member, Pb *b)
{
    pb_u(b, 1, g.appid);
    pb_f64(b, 2, l->id);
    if (member) {
        pb_f64(b, 3, member);
        Pb meta = { 0 };
        const Member *m = NULL;
        for (int i = 0; i < l->member_count; i++) if (l->members[i].id == member) m = &l->members[i];
        KvList empty = { 0 };
        kv_encode(m ? &m->data : &empty, &meta);
        pb_bytes(b, 7, meta.d, meta.n);
        free(meta.d);
    } else {
        pb_i(b, 4, l->max_members);
        pb_i(b, 5, l->type);
        pb_i(b, 6, l->flags);
        Pb meta = { 0 };
        kv_encode(&l->data, &meta);
        pb_bytes(b, 7, meta.d, meta.n);
        free(meta.d);
    }
}

static int reply_eresult(const uint8_t *data, long len, uint32_t field)
{
    if (len < 0) return 0;
    PbIn r = { data, data + len };
    PbF f;
    while (pb_next(&r, &f) == 1) if (f.field == field && f.type == 0) return (int)(int32_t)f.v;
    return 2;                                                /* k_EResultFail */
}

static void run_job(Job *job, uint8_t *reply)
{
    Pb body = { 0 };
    long got;
    switch (job->type) {
    case JOB_LIST: {
        pb_u(&body, 1, g.appid);
        pb_i(&body, 3, job->result_count > 0 ? job->result_count : 50);
        for (int i = 0; i < job->filter_count; i++) {
            Pb filter = { 0 };
            pb_str(&filter, 1, job->filters[i].key);
            pb_str(&filter, 2, job->filters[i].value);
            pb_i(&filter, 3, job->filters[i].comparison);
            pb_i(&filter, 4, job->filters[i].type);
            pb_bytes(&body, 6, filter.d, filter.n);
            free(filter.d);
        }
        got = g.io.request(g.appid, MMS_GET_LOBBY_LIST, body.d, body.n, reply, REPLY_CAP);
        pthread_mutex_lock(&g.lock);
        g.list_count = 0;
        int ok = got >= 0 && reply_eresult(reply, got, 3) == 1;
        PbIn r = { reply, reply + (got > 0 ? got : 0) };
        PbF f;
        while (ok && pb_next(&r, &f) == 1) {
            if (f.field != 4 || f.type != 2 || g.list_count == MAX_LIST) continue;
            PbIn lr = { f.data, f.data + f.len };
            PbF lf;
            uint64_t id = 0;
            int max = 0, type = 0, flags = 0;
            const uint8_t *meta = NULL;
            size_t meta_len = 0;
            while (pb_next(&lr, &lf) == 1) {
                if (lf.field == 1) id = lf.v;
                else if (lf.field == 2) max = (int)lf.v;
                else if (lf.field == 3) type = (int)lf.v;
                else if (lf.field == 4) flags = (int)lf.v;
                else if (lf.field == 5 && lf.type == 2) { meta = lf.data; meta_len = lf.len; }
            }
            Lobby *l = id ? lobby_locked(id, 1) : NULL;
            if (!l) continue;
            if (!l->entered) {
                l->max_members = max;
                l->type = type;
                l->flags = flags;
                if (meta) kv_decode(meta, meta_len, &l->data);
            }
            g.list[g.list_count++] = id;
        }
        uint32_t count = (uint32_t)g.list_count;
        complete_locked(job->call, &count, 4, got < 0);
        pthread_mutex_unlock(&g.lock);
        break;
    }
    case JOB_CREATE: {
        pb_u(&body, 1, g.appid);
        pb_i(&body, 2, job->b);
        pb_i(&body, 3, job->a);
        pb_i(&body, 4, 0);
        Pb meta = { 0 };
        KvList empty = { 0 };
        kv_encode(&empty, &meta);
        pb_bytes(&body, 7, meta.d, meta.n);
        free(meta.d);
        pb_str(&body, 8, g.persona);
        got = g.io.request(g.appid, MMS_CREATE_LOBBY, body.d, body.n, reply, REPLY_CAP);
        uint64_t lobby = 0;
        int eresult = got >= 0 ? reply_eresult(reply, got, 3) : 3;   /* k_EResultNoConnection */
        PbIn r = { reply, reply + (got > 0 ? got : 0) };
        PbF f;
        while (got > 0 && pb_next(&r, &f) == 1) if (f.field == 2) lobby = f.v;
        pthread_mutex_lock(&g.lock);
        if (eresult == 1 && lobby) {
            Lobby *l = lobby_locked(lobby, 1);
            if (l) {
                l->owner = g.self;
                l->max_members = job->b;
                l->type = job->a;
                l->entered = 1;
                Member *m = member_locked(l, g.self, 1);
                if (m) snprintf(m->persona, sizeof(m->persona), "%s", g.persona);
            }
            uint8_t enter[24];
            lobby_enter_cb(enter, lobby, 1);                 /* k_EChatRoomEnterResponseSuccess */
            post_locked(BEER_CB_LOBBY_ENTER, enter, sizeof(enter));
        }
        uint8_t created[16] = { 0 };
        int32_t er = eresult;
        memcpy(created, &er, 4);
        if (eresult == 1) memcpy(created + 8, &lobby, 8);
        complete_locked(job->call, created, sizeof(created), got < 0);
        pthread_mutex_unlock(&g.lock);
        break;
    }
    case JOB_JOIN: {
        pb_u(&body, 1, g.appid);
        pb_f64(&body, 2, job->lobby);
        pb_str(&body, 3, g.persona);
        got = g.io.request(g.appid, MMS_JOIN_LOBBY, body.d, body.n, reply, REPLY_CAP);
        uint32_t response = 2;                                /* k_EChatRoomEnterResponseDoesntExist */
        pthread_mutex_lock(&g.lock);
        if (got >= 0) {
            PbIn r = { reply, reply + got };
            PbF f;
            Lobby *l = lobby_locked(job->lobby, 1);
            while (pb_next(&r, &f) == 1) {
                if (f.field == 3) response = (uint32_t)f.v;
                if (!l) continue;
                if (f.field == 4) l->max_members = (int)f.v;
                else if (f.field == 5) l->type = (int)f.v;
                else if (f.field == 6) l->flags = (int)f.v;
                else if (f.field == 7) l->owner = f.v;
                else if (f.field == 8 && f.type == 2) kv_decode(f.data, f.len, &l->data);
                else if (f.field == 9 && f.type == 2) read_member_locked(l, f.data, f.len);
            }
            if (l && response == 1) {
                l->entered = 1;
                if (!member_locked(l, g.self, 0)) {
                    Member *m = member_locked(l, g.self, 1);
                    if (m) snprintf(m->persona, sizeof(m->persona), "%s", g.persona);
                }
            } else if (l && !l->entered) {
                lobby_forget_locked(l);
            }
        }
        uint8_t enter[24];
        lobby_enter_cb(enter, job->lobby, response);
        post_locked(BEER_CB_LOBBY_ENTER, enter, sizeof(enter));
        complete_locked(job->call, enter, sizeof(enter), got < 0);
        pthread_mutex_unlock(&g.lock);
        break;
    }
    case JOB_LEAVE:
        pb_u(&body, 1, g.appid);
        pb_f64(&body, 2, job->lobby);
        g.io.request(g.appid, MMS_LEAVE_LOBBY, body.d, body.n, reply, REPLY_CAP);
        break;
    case JOB_PUSH_DATA:
    case JOB_PUSH_MEMBER: {
        pthread_mutex_lock(&g.lock);
        Lobby *l = lobby_locked(job->lobby, 0);
        if (l) lobby_state_body_locked(l, job->type == JOB_PUSH_MEMBER ? g.self : 0, &body);
        pthread_mutex_unlock(&g.lock);
        if (!l) break;
        got = g.io.request(g.appid, MMS_SET_LOBBY_DATA, body.d, body.n, reply, REPLY_CAP);
        int eresult = got >= 0 ? reply_eresult(reply, got, 3) : 3;
        if (eresult != 1)
            fprintf(stderr, "[STEAM] lobby %llu: setting %s data failed (EResult %d)\n",
                    (unsigned long long)job->lobby, job->type == JOB_PUSH_MEMBER ? "member" : "lobby", eresult);
        pthread_mutex_lock(&g.lock);
        lobby_data_update_locked(job->lobby, job->type == JOB_PUSH_MEMBER ? g.self : job->lobby, eresult == 1);
        pthread_mutex_unlock(&g.lock);
        break;
    }
    case JOB_SET_OWNER:
        pb_u(&body, 1, g.appid);
        pb_f64(&body, 2, job->lobby);
        pb_f64(&body, 3, job->user);
        g.io.request(g.appid, MMS_SET_LOBBY_OWNER, body.d, body.n, reply, REPLY_CAP);
        break;
    }
    free(body.d);
}

static void *worker(void *arg)
{
    (void)arg;
    uint8_t *reply = malloc(REPLY_CAP);
    if (!reply) return NULL;
    for (;;) {
        pthread_mutex_lock(&g.lock);
        while (!g.jobs) pthread_cond_wait(&g.work, &g.lock);
        Job *job = g.jobs;
        g.jobs = job->next;
        if (!g.jobs) g.jobs_tail = &g.jobs;
        pthread_mutex_unlock(&g.lock);
        run_job(job, reply);
        free(job->filters);
        free(job);
    }
    return NULL;
}

/* Caller holds g.lock. Data pushes for the same lobby collapse into the one already queued. */
static void queue_job_locked(Job *job)
{
    if (job->type == JOB_PUSH_DATA || job->type == JOB_PUSH_MEMBER)
        for (Job *q = g.jobs; q; q = q->next)
            if (q->type == job->type && q->lobby == job->lobby) { free(job); return; }
    job->next = NULL;
    *g.jobs_tail = job;
    g.jobs_tail = &job->next;
    pthread_cond_signal(&g.work);
}
static Job *job_new(JobType type) { Job *j = calloc(1, sizeof(*j)); if (j) j->type = type; return j; }

/* ---- public ---- */
void beer_mms_init(const BeerMmsTransport *transport, uint32_t appid, uint64_t self, const char *persona)
{
    pthread_mutex_lock(&g.lock);
    if (!g.ready && transport && appid && self) {
        g.io = *transport;
        g.appid = appid;
        g.self = self;
        snprintf(g.persona, sizeof(g.persona), "%s", persona ? persona : "");
        remember_persona_locked(self, g.persona);
        g.jobs_tail = &g.jobs;
        pthread_t thread;
        if (pthread_create(&thread, NULL, worker, NULL) == 0) {
            pthread_detach(thread);
            g.ready = 1;
        }
    }
    pthread_mutex_unlock(&g.lock);
}

int beer_mms_ready(void)
{
    pthread_mutex_lock(&g.lock);
    int ready = g.ready;
    pthread_mutex_unlock(&g.lock);
    return ready;
}

static void handle_event_locked(uint32_t emsg, const uint8_t *data, size_t len)
{
    PbIn r = { data, data + len };
    PbF f;
    uint64_t lobby_id = 0, user = 0;
    char name[128] = "";
    switch (emsg) {
    case MMS_LOBBY_DATA: {
        while (pb_next(&r, &f) == 1) if (f.field == 2) lobby_id = f.v;
        Lobby *l = lobby_id ? lobby_locked(lobby_id, 1) : NULL;
        if (!l) return;
        r = (PbIn){ data, data + len };
        int has_members = 0;
        while (pb_next(&r, &f) == 1) {
            if (f.field == 4) l->max_members = (int)f.v;
            else if (f.field == 5) l->type = (int)f.v;
            else if (f.field == 6) l->flags = (int)f.v;
            else if (f.field == 7) l->owner = f.v;
            else if (f.field == 8 && f.type == 2) kv_decode(f.data, f.len, &l->data);
            else if (f.field == 9 && f.type == 2) {
                if (!has_members) {                                 /* the message carries the full list */
                    for (int i = 0; i < l->member_count; i++) kv_clear(&l->members[i].data);
                    l->member_count = 0;
                    has_members = 1;
                }
                read_member_locked(l, f.data, f.len);
            }
        }
        lobby_data_update_locked(lobby_id, lobby_id, 1);
        for (int i = 0; i < l->member_count; i++) lobby_data_update_locked(lobby_id, l->members[i].id, 1);
        return;
    }
    case MMS_USER_JOINED_LOBBY:
    case MMS_USER_LEFT_LOBBY: {
        while (pb_next(&r, &f) == 1) {
            if (f.field == 2) lobby_id = f.v;
            else if (f.field == 3) user = f.v;
            else if (f.field == 4 && f.type == 2) snprintf(name, sizeof(name), "%.*s", (int)f.len, (const char *)f.data);
        }
        Lobby *l = lobby_id ? lobby_locked(lobby_id, 0) : NULL;
        if (!l || !user) return;
        remember_persona_locked(user, name);
        if (emsg == MMS_USER_JOINED_LOBBY) {
            Member *m = member_locked(l, user, 1);
            if (m && *name) snprintf(m->persona, sizeof(m->persona), "%s", name);
        } else {
            member_remove_locked(l, user);
        }
        uint8_t cb[32] = { 0 };
        uint32_t state = emsg == MMS_USER_JOINED_LOBBY ? 0x1u : 0x2u; /* k_EChatMemberStateChangeEntered / Left */
        memcpy(cb, &lobby_id, 8);
        memcpy(cb + 8, &user, 8);
        memcpy(cb + 16, &user, 8);
        memcpy(cb + 24, &state, 4);
        post_locked(BEER_CB_LOBBY_CHAT_UPDATE, cb, sizeof(cb));
        return;
    }
    case MMS_LOBBY_CHAT_MSG: {
        const uint8_t *msg = NULL;
        size_t msg_len = 0;
        while (pb_next(&r, &f) == 1) {
            if (f.field == 2) lobby_id = f.v;
            else if (f.field == 3) user = f.v;
            else if (f.field == 4 && f.type == 2) { msg = f.data; msg_len = f.len; }
        }
        if (!lobby_id || !lobby_locked(lobby_id, 0)) return;
        int id = g.chat_next++;
        ChatEntry *e = &g.chat[id % MAX_CHAT];
        free(e->data);
        e->data = msg_len ? malloc(msg_len) : NULL;
        if (e->data) memcpy(e->data, msg, msg_len);
        e->lobby = lobby_id;
        e->user = user;
        e->type = 1;                                                    /* k_EChatEntryTypeChatMsg */
        e->len = e->data ? (int)msg_len : 0;
        uint8_t cb[24] = { 0 };
        uint32_t chat_id = (uint32_t)id;
        memcpy(cb, &lobby_id, 8);
        memcpy(cb + 8, &user, 8);
        cb[16] = 1;
        memcpy(cb + 20, &chat_id, 4);
        post_locked(BEER_CB_LOBBY_CHAT_MSG, cb, sizeof(cb));
        return;
    }
    case MMS_LOBBY_GAME_SERVER_SET: {
        uint32_t ip = 0;
        uint16_t port = 0;
        uint64_t server = 0;
        while (pb_next(&r, &f) == 1) {
            if (f.field == 2) lobby_id = f.v;
            else if (f.field == 3) ip = (uint32_t)f.v;
            else if (f.field == 4) port = (uint16_t)f.v;
            else if (f.field == 5) server = f.v;
            else if (f.field == 6 && f.type == 2) {                     /* CMsgIPAddress { fixed32 v4 = 1 } */
                PbIn ir = { f.data, f.data + f.len };
                PbF iff;
                while (pb_next(&ir, &iff) == 1) if (iff.field == 1) ip = (uint32_t)iff.v;
            }
        }
        Lobby *l = lobby_id ? lobby_locked(lobby_id, 0) : NULL;
        if (!l) return;
        l->server_ip = ip;
        l->server_port = port;
        l->server_id = server;
        l->has_server = 1;
        uint8_t cb[24] = { 0 };
        memcpy(cb, &lobby_id, 8);
        memcpy(cb + 8, &server, 8);
        memcpy(cb + 16, &ip, 4);
        memcpy(cb + 20, &port, 2);
        post_locked(BEER_CB_LOBBY_GAME_CREATED, cb, sizeof(cb));
        return;
    }
    default:
        fprintf(stderr, "[STEAM] matchmaking message %u from Steam not handled\n", emsg);
        return;
    }
}

void beer_mms_pump(void)
{
    if (!beer_mms_ready()) return;
    static uint8_t buffer[REPLY_CAP];
    static pthread_mutex_t pump_lock = PTHREAD_MUTEX_INITIALIZER;
    if (pthread_mutex_trylock(&pump_lock)) return;
    uint32_t emsg;
    long len;
    while ((len = g.io.next_event(&emsg, buffer, sizeof(buffer))) >= 0) {
        pthread_mutex_lock(&g.lock);
        handle_event_locked(emsg, buffer, (size_t)len);
        pthread_mutex_unlock(&g.lock);
    }
    pthread_mutex_unlock(&pump_lock);
}

int beer_mms_peek_callback(BeerMmsCallback *out)
{
    pthread_mutex_lock(&g.lock);
    int has = g.callback_count > 0;
    if (has) *out = g.callbacks[g.callback_head];
    pthread_mutex_unlock(&g.lock);
    return has;
}

void beer_mms_pop_callback(void)
{
    pthread_mutex_lock(&g.lock);
    if (g.callback_count) {
        g.callback_head = (g.callback_head + 1) % MAX_CALLBACKS;
        g.callback_count--;
    }
    pthread_mutex_unlock(&g.lock);
}

int beer_mms_owns_call(uint64_t call) { return (call & ~UINT64_C(0xffffffff)) == CALL_BASE; }

int beer_mms_call_completed(uint64_t call, int *failed)
{
    pthread_mutex_lock(&g.lock);
    Call *c = call_locked(call);
    int done = c && c->done;
    if (failed) *failed = c ? c->failed : 1;
    pthread_mutex_unlock(&g.lock);
    return done;
}

int beer_mms_call_info(uint64_t call, uint32_t *id, uint32_t *size)
{
    pthread_mutex_lock(&g.lock);
    Call *c = call_locked(call);
    if (c) { *id = c->id; *size = c->size; }
    pthread_mutex_unlock(&g.lock);
    return c != NULL;
}

int beer_mms_call_result(uint64_t call, void *out, uint32_t size, int32_t expected_id, int *failed)
{
    pthread_mutex_lock(&g.lock);
    Call *c = call_locked(call);
    int ok = c && c->done && c->id == (uint32_t)expected_id && c->size == size && out;
    if (failed) *failed = !ok || c->failed;
    if (ok) {
        memcpy(out, c->data, size);
        c->used = 0;
    }
    pthread_mutex_unlock(&g.lock);
    return ok;
}

void beer_mms_add_filter(const char *key, const char *value, int comparison, int filter_type)
{
    pthread_mutex_lock(&g.lock);
    if (g.filter_count < MAX_FILTERS) {
        Filter *f = &g.filters[g.filter_count++];
        snprintf(f->key, sizeof(f->key), "%s", key ? key : "");
        snprintf(f->value, sizeof(f->value), "%s", value ? value : "");
        f->comparison = comparison;
        f->type = filter_type;
    }
    pthread_mutex_unlock(&g.lock);
}

void beer_mms_set_result_count(int count)
{
    pthread_mutex_lock(&g.lock);
    g.result_count = count;
    pthread_mutex_unlock(&g.lock);
}

uint64_t beer_mms_request_lobby_list(void)
{
    if (!beer_mms_ready()) return 0;
    Job *job = job_new(JOB_LIST);
    if (!job) return 0;
    pthread_mutex_lock(&g.lock);
    job->call = new_call_locked(BEER_CB_LOBBY_MATCH_LIST, 4);
    job->filters = g.filter_count ? malloc((size_t)g.filter_count * sizeof(Filter)) : NULL;
    if (job->filters) memcpy(job->filters, g.filters, (size_t)g.filter_count * sizeof(Filter));
    job->filter_count = job->filters ? g.filter_count : 0;
    job->result_count = g.result_count;
    g.filter_count = 0;                                    /* filters apply to the next request only */
    g.result_count = 0;
    uint64_t call = job->call;
    if (call) queue_job_locked(job); else free(job);
    pthread_mutex_unlock(&g.lock);
    return call;
}

uint64_t beer_mms_lobby_by_index(int index)
{
    pthread_mutex_lock(&g.lock);
    uint64_t id = index >= 0 && index < g.list_count ? g.list[index] : 0;
    pthread_mutex_unlock(&g.lock);
    return id;
}

uint64_t beer_mms_create_lobby(int type, int max_members)
{
    if (!beer_mms_ready()) return 0;
    Job *job = job_new(JOB_CREATE);
    if (!job) return 0;
    job->a = type;
    job->b = max_members;
    pthread_mutex_lock(&g.lock);
    job->call = new_call_locked(BEER_CB_LOBBY_CREATED, 16);
    uint64_t call = job->call;
    if (call) queue_job_locked(job); else free(job);
    pthread_mutex_unlock(&g.lock);
    return call;
}

uint64_t beer_mms_join_lobby(uint64_t lobby)
{
    if (!beer_mms_ready() || !lobby) return 0;
    Job *job = job_new(JOB_JOIN);
    if (!job) return 0;
    job->lobby = lobby;
    pthread_mutex_lock(&g.lock);
    job->call = new_call_locked(BEER_CB_LOBBY_ENTER, 24);
    uint64_t call = job->call;
    if (call) queue_job_locked(job); else free(job);
    pthread_mutex_unlock(&g.lock);
    return call;
}

void beer_mms_leave_lobby(uint64_t lobby)
{
    if (!beer_mms_ready()) return;
    pthread_mutex_lock(&g.lock);
    Lobby *l = lobby_locked(lobby, 0);
    int entered = l && l->entered;
    if (l) lobby_forget_locked(l);
    Job *job = entered ? job_new(JOB_LEAVE) : NULL;
    if (job) { job->lobby = lobby; queue_job_locked(job); }
    pthread_mutex_unlock(&g.lock);
}

int beer_mms_invite(uint64_t lobby, uint64_t user)
{
    if (!beer_mms_ready() || !lobby || !user) return 0;
    Pb body = { 0 };
    pb_u(&body, 1, g.appid);
    pb_f64(&body, 2, lobby);
    pb_f64(&body, 3, user);
    int ok = g.io.send(g.appid, MMS_INVITE_TO_LOBBY, body.d, body.n);
    free(body.d);
    return ok;
}

int beer_mms_member_count(uint64_t lobby)
{
    pthread_mutex_lock(&g.lock);
    Lobby *l = lobby_locked(lobby, 0);
    int count = l && l->entered ? l->member_count : 0;
    pthread_mutex_unlock(&g.lock);
    return count;
}

uint64_t beer_mms_member_by_index(uint64_t lobby, int index)
{
    pthread_mutex_lock(&g.lock);
    Lobby *l = lobby_locked(lobby, 0);
    uint64_t id = l && l->entered && index >= 0 && index < l->member_count ? l->members[index].id : 0;
    pthread_mutex_unlock(&g.lock);
    return id;
}

/* Returned strings stay valid until the value changes, as the Steam client's do. */
const char *beer_mms_lobby_data(uint64_t lobby, const char *key)
{
    pthread_mutex_lock(&g.lock);
    Lobby *l = key ? lobby_locked(lobby, 0) : NULL;
    const char *value = l ? kv_get(&l->data, key) : NULL;
    pthread_mutex_unlock(&g.lock);
    return value ? value : "";
}

int beer_mms_set_lobby_data(uint64_t lobby, const char *key, const char *value)
{
    if (!beer_mms_ready() || !key || !value) return 0;
    pthread_mutex_lock(&g.lock);
    Lobby *l = lobby_locked(lobby, 0);
    int ok = l && l->entered && l->owner == g.self && kv_set(&l->data, key, value);
    Job *job = ok ? job_new(JOB_PUSH_DATA) : NULL;
    if (job) { job->lobby = lobby; queue_job_locked(job); }
    pthread_mutex_unlock(&g.lock);
    return ok;
}

int beer_mms_lobby_data_count(uint64_t lobby)
{
    pthread_mutex_lock(&g.lock);
    Lobby *l = lobby_locked(lobby, 0);
    int count = l ? l->data.count : 0;
    pthread_mutex_unlock(&g.lock);
    return count;
}

int beer_mms_lobby_data_by_index(uint64_t lobby, int index, char *key, int key_cap, char *value, int value_cap)
{
    pthread_mutex_lock(&g.lock);
    Lobby *l = lobby_locked(lobby, 0);
    int ok = l && index >= 0 && index < l->data.count;
    if (ok) {
        if (key && key_cap > 0) snprintf(key, (size_t)key_cap, "%s", l->data.items[index].key);
        if (value && value_cap > 0) snprintf(value, (size_t)value_cap, "%s", l->data.items[index].value);
    }
    pthread_mutex_unlock(&g.lock);
    return ok;
}

int beer_mms_delete_lobby_data(uint64_t lobby, const char *key)
{
    if (!beer_mms_ready() || !key) return 0;
    pthread_mutex_lock(&g.lock);
    Lobby *l = lobby_locked(lobby, 0);
    int ok = l && l->entered && l->owner == g.self && kv_delete(&l->data, key);
    Job *job = ok ? job_new(JOB_PUSH_DATA) : NULL;
    if (job) { job->lobby = lobby; queue_job_locked(job); }
    pthread_mutex_unlock(&g.lock);
    return ok;
}

const char *beer_mms_member_data(uint64_t lobby, uint64_t user, const char *key)
{
    pthread_mutex_lock(&g.lock);
    Lobby *l = key ? lobby_locked(lobby, 0) : NULL;
    Member *m = l ? member_locked(l, user, 0) : NULL;
    const char *value = m ? kv_get(&m->data, key) : NULL;
    pthread_mutex_unlock(&g.lock);
    return value ? value : "";
}

void beer_mms_set_member_data(uint64_t lobby, const char *key, const char *value)
{
    if (!beer_mms_ready() || !key || !value) return;
    pthread_mutex_lock(&g.lock);
    Lobby *l = lobby_locked(lobby, 0);
    Member *m = l && l->entered ? member_locked(l, g.self, 1) : NULL;
    if (m && kv_set(&m->data, key, value)) {
        Job *job = job_new(JOB_PUSH_MEMBER);
        if (job) { job->lobby = lobby; queue_job_locked(job); }
    }
    pthread_mutex_unlock(&g.lock);
}

int beer_mms_send_chat(uint64_t lobby, const void *data, int len)
{
    if (!beer_mms_ready() || len < 0 || len > 4096 || (len && !data)) return 0;
    pthread_mutex_lock(&g.lock);
    Lobby *l = lobby_locked(lobby, 0);
    int entered = l && l->entered;
    pthread_mutex_unlock(&g.lock);
    if (!entered) return 0;
    Pb body = { 0 };
    pb_u(&body, 1, g.appid);
    pb_f64(&body, 2, lobby);
    pb_f64(&body, 3, 0);                                   /* steam_id_target: everyone */
    pb_bytes(&body, 4, data, (size_t)len);
    int ok = g.io.send(g.appid, MMS_SEND_LOBBY_CHAT_MSG, body.d, body.n);
    free(body.d);
    return ok;
}

int beer_mms_chat_entry(uint64_t lobby, int chat_id, uint64_t *user, void *data, int cap, int *type)
{
    pthread_mutex_lock(&g.lock);
    int len = 0;
    if (chat_id >= 0 && chat_id < g.chat_next && chat_id >= g.chat_next - MAX_CHAT) {
        ChatEntry *e = &g.chat[chat_id % MAX_CHAT];
        if (e->lobby == lobby) {
            if (user) *user = e->user;
            if (type) *type = e->type;
            len = e->len < cap ? e->len : cap;
            if (len > 0 && data) memcpy(data, e->data, (size_t)len);
        }
    }
    pthread_mutex_unlock(&g.lock);
    return len;
}

int beer_mms_request_lobby_data(uint64_t lobby)
{
    if (!beer_mms_ready() || !lobby) return 0;
    Pb body = { 0 };
    pb_u(&body, 1, g.appid);
    pb_f64(&body, 2, lobby);
    int ok = g.io.send(g.appid, MMS_GET_LOBBY_DATA, body.d, body.n);  /* the answer arrives as LobbyData */
    free(body.d);
    return ok;
}

void beer_mms_set_game_server(uint64_t lobby, uint32_t ip, uint16_t port, uint64_t server)
{
    if (!beer_mms_ready()) return;
    Pb body = { 0 }, address = { 0 };
    pb_u(&body, 1, g.appid);
    pb_f64(&body, 2, lobby);
    pb_u(&body, 4, port);
    pb_f64(&body, 5, server);
    pb_varint(&address, 1u << 3 | 5);                     /* CMsgIPAddress.v4 (fixed32) */
    pb_raw(&address, &ip, 4);
    pb_bytes(&body, 6, address.d, address.n);
    g.io.send(g.appid, MMS_SET_LOBBY_GAME_SERVER, body.d, body.n);
    free(address.d);
    free(body.d);
}

int beer_mms_game_server(uint64_t lobby, uint32_t *ip, uint16_t *port, uint64_t *server)
{
    pthread_mutex_lock(&g.lock);
    Lobby *l = lobby_locked(lobby, 0);
    int has = l && l->has_server;
    if (has) {
        if (ip) *ip = l->server_ip;
        if (port) *port = l->server_port;
        if (server) *server = l->server_id;
    }
    pthread_mutex_unlock(&g.lock);
    return has;
}

static int owner_change_locked(uint64_t lobby, Lobby **out)
{
    Lobby *l = lobby_locked(lobby, 0);
    *out = l;
    return l && l->entered && l->owner == g.self;
}

int beer_mms_set_member_limit(uint64_t lobby, int max_members)
{
    if (!beer_mms_ready()) return 0;
    pthread_mutex_lock(&g.lock);
    Lobby *l;
    int ok = owner_change_locked(lobby, &l);
    if (ok) {
        l->max_members = max_members;
        Job *job = job_new(JOB_PUSH_DATA);
        if (job) { job->lobby = lobby; queue_job_locked(job); }
    }
    pthread_mutex_unlock(&g.lock);
    return ok;
}

int beer_mms_member_limit(uint64_t lobby)
{
    pthread_mutex_lock(&g.lock);
    Lobby *l = lobby_locked(lobby, 0);
    int max = l ? l->max_members : 0;
    pthread_mutex_unlock(&g.lock);
    return max;
}

int beer_mms_set_type(uint64_t lobby, int type)
{
    if (!beer_mms_ready()) return 0;
    pthread_mutex_lock(&g.lock);
    Lobby *l;
    int ok = owner_change_locked(lobby, &l);
    if (ok) {
        l->type = type;
        Job *job = job_new(JOB_PUSH_DATA);
        if (job) { job->lobby = lobby; queue_job_locked(job); }
    }
    pthread_mutex_unlock(&g.lock);
    return ok;
}

/* Joinability is a lobby-owner setting kept by the Steam client; Beer records it locally (the MMS wire flag
 * for it is not identified yet), so lobbies stay joinable on Steam's side. */
int beer_mms_set_joinable(uint64_t lobby, int joinable)
{
    pthread_mutex_lock(&g.lock);
    Lobby *l;
    int ok = owner_change_locked(lobby, &l);
    if (ok) l->joinable = joinable != 0;
    pthread_mutex_unlock(&g.lock);
    return ok;
}

uint64_t beer_mms_owner(uint64_t lobby)
{
    pthread_mutex_lock(&g.lock);
    Lobby *l = lobby_locked(lobby, 0);
    uint64_t owner = l && l->entered ? l->owner : 0;
    pthread_mutex_unlock(&g.lock);
    return owner;
}

int beer_mms_set_owner(uint64_t lobby, uint64_t owner)
{
    if (!beer_mms_ready() || !owner) return 0;
    pthread_mutex_lock(&g.lock);
    Lobby *l;
    int ok = owner_change_locked(lobby, &l);
    Job *job = ok ? job_new(JOB_SET_OWNER) : NULL;
    if (job) { job->lobby = lobby; job->user = owner; queue_job_locked(job); }
    pthread_mutex_unlock(&g.lock);
    return ok;
}

const char *beer_mms_persona(uint64_t user)
{
    pthread_mutex_lock(&g.lock);
    const char *name = NULL;
    for (int i = 0; i < g.persona_count && !name; i++) if (g.personas[i].id == user) name = g.personas[i].name;
    pthread_mutex_unlock(&g.lock);
    return name;
}
