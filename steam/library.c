#include "library.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "net.h"
#include "vdf.h"

#include <zlib.h>

#define BATCH 100
#define PICS_TIMEOUT_MS 60000
#define PICS_RESPONSE_PENDING 6

int library_parse_licenses(const uint8_t *body, size_t len, SteamLicense **licenses, size_t *count)
{
    *licenses = NULL;
    *count = 0;
    int eresult = ERESULT_FAIL;
    size_t cap = 0;
    PbReader r = { body, body + len };
    PbField f;
    while (pb_next(&r, &f) == 1) {
        if (f.field == 1) { eresult = (int)(int32_t)f.value; continue; }
        if (f.field != 2 || f.type != PB_BYTES) continue;
        SteamLicense license = {0};
        PbReader lr = { f.data, f.data + f.len };
        PbField lf;
        while (pb_next(&lr, &lf) == 1) {
            if (lf.field == 1) license.package_id = (uint32_t)lf.value;
            else if (lf.field == 12) license.owner_id = (uint32_t)lf.value;
            else if (lf.field == 17) license.access_token = lf.value;
        }
        if (*count == cap) {
            cap = cap ? cap * 2 : 64;
            SteamLicense *grown = realloc(*licenses, cap * sizeof(**licenses));
            if (!grown) break;
            *licenses = grown;
        }
        (*licenses)[(*count)++] = license;
    }
    return eresult;
}

/* ---------------------------------------------------------------- growable app-id set */

typedef struct {
    uint32_t *ids;
    int *shared;
    size_t count, cap;
} IdSet;

static void idset_add(IdSet *set, uint32_t id, int shared)
{
    for (size_t i = 0; i < set->count; i++)
        if (set->ids[i] == id) { if (!shared) set->shared[i] = 0; return; }
    if (set->count == set->cap) {
        size_t cap = set->cap ? set->cap * 2 : 256;
        uint32_t *ids = realloc(set->ids, cap * sizeof(*ids));
        if (!ids) return;
        set->ids = ids;
        int *flags = realloc(set->shared, cap * sizeof(*flags));
        if (!flags) return;
        set->shared = flags;
        set->cap = cap;
    }
    set->ids[set->count] = id;
    set->shared[set->count++] = shared;
}

/* ---------------------------------------------------------------- PICS */

/* Calls back for each app or package info record in the reply parts. kind: 1 = apps, 3 = packages. */
typedef void (*InfoFn)(void *context, uint32_t id, const uint8_t *buffer, size_t len);

/* Large app records are served over HTTP instead of inline: <host>/appinfo/<id>/sha/<sha>.txt.gz. */
static uint8_t *http_record(const char *host, uint32_t id, const uint8_t *sha, size_t sha_len, size_t *out_len)
{
    if (!host[0] || sha_len != 20) return NULL;
    char hex[41], path[160], err[256];
    for (int i = 0; i < 20; i++) snprintf(hex + 2 * i, 3, "%02x", sha[i]);
    snprintf(path, sizeof(path), "/appinfo/%u/sha/%s.txt.gz", id, hex);
    char *body;
    size_t len;
    int status = https_get(host, path, &body, &len, err, sizeof(err));
    if (status != 200) {
        fprintf(stderr, "[STEAM] app %u record from %s: %s\n", id, host, status ? "HTTP error" : err);
        free(body);
        return NULL;
    }
    if (len < 2 || (uint8_t)body[0] != 0x1f || (uint8_t)body[1] != 0x8b) { *out_len = len; return (uint8_t *)body; }
    size_t cap = len * 8 + 4096, n = 0;
    uint8_t *out = malloc(cap);
    z_stream z = {0};
    int status_z = out && inflateInit2(&z, 16 + MAX_WBITS) == Z_OK ? Z_OK : Z_MEM_ERROR;
    z.next_in = (Bytef *)body;
    z.avail_in = (uInt)len;
    while (status_z == Z_OK) {
        if (n == cap) {
            uint8_t *grown = cap < (64u << 20) ? realloc(out, cap *= 2) : NULL;
            if (!grown) { status_z = Z_MEM_ERROR; break; }
            out = grown;
        }
        z.next_out = out + n;
        z.avail_out = (uInt)(cap - n);
        status_z = inflate(&z, Z_NO_FLUSH);
        n = cap - z.avail_out;
    }
    inflateEnd(&z);
    free(body);
    if (status_z != Z_STREAM_END) { free(out); return NULL; }
    *out_len = n;
    return out;
}

static int product_info(Cm *cm, const PbBuf *request, int kind, InfoFn fn, void *context)
{
    CmPart *parts;
    if (!cm_request(cm, EMSG_CLIENT_PICS_PRODUCT_INFO_REQUEST, request, PICS_RESPONSE_PENDING,
                    PICS_TIMEOUT_MS, &parts))
        return 0;
    for (CmPart *part = parts; part; part = part->next) {
        char host[256] = "";
        PbField hf;
        if (pb_find(part->body.data, part->body.len, 8, &hf) && hf.type == PB_BYTES && hf.len < sizeof(host)) {
            memcpy(host, hf.data, hf.len);
            host[hf.len] = 0;
        }
        PbReader r = { part->body.data, part->body.data + part->body.len };
        PbField f;
        while (pb_next(&r, &f) == 1) {
            if ((int)f.field != kind || f.type != PB_BYTES) continue;
            PbField id, buffer, sha;
            if (!pb_find(f.data, f.len, 1, &id)) continue;
            if (pb_find(f.data, f.len, 5, &buffer) && buffer.type == PB_BYTES && buffer.len) {
                fn(context, (uint32_t)id.value, buffer.data, buffer.len);
                continue;
            }
            size_t len = 0;
            uint8_t *fetched = kind == 1 && pb_find(f.data, f.len, 4, &sha) && sha.type == PB_BYTES
                             ? http_record(host, (uint32_t)id.value, sha.data, sha.len, &len) : NULL;
            fn(context, (uint32_t)id.value, fetched, len);
            free(fetched);
        }
    }
    cm_parts_free(parts);
    return 1;
}

typedef struct {
    IdSet *apps;
    int shared;
} PackageContext;

static void on_package(void *context, uint32_t id, const uint8_t *buffer, size_t len)
{
    PackageContext *pc = context;
    (void)id;
    if (!buffer) return;
    /* Package records are binary KeyValues after a 4-byte prefix. */
    VdfNode *root = len > 4 ? vdf_parse_binary(buffer + 4, len - 4) : NULL;
    if (!root) root = vdf_parse_binary(buffer, len);
    if (!root) return;
    VdfNode *package = root->child;
    VdfNode *appids = package ? vdf_child(package, "appids") : NULL;
    for (VdfNode *n = appids ? appids->child : NULL; n; n = n->next)
        if (n->value) idset_add(pc->apps, (uint32_t)strtoul(n->value, NULL, 10), pc->shared);
    vdf_free(root);
}

typedef struct {
    SteamApp *apps;
    size_t count;
    size_t missing;
} AppContext;

static char *lowercase_dup(const char *s)
{
    char *out = strdup(s);
    for (char *p = out; p && *p; p++) *p = (char)tolower((unsigned char)*p);
    return out;
}

static void on_app(void *context, uint32_t id, const uint8_t *buffer, size_t len)
{
    AppContext *ac = context;
    SteamApp *app = NULL;
    for (size_t i = 0; i < ac->count; i++)
        if (ac->apps[i].appid == id) { app = &ac->apps[i]; break; }
    if (!app) return;
    if (!buffer) { ac->missing++; return; }
    while (len && !buffer[len - 1]) len--;          /* the text is NUL-terminated */
    VdfNode *root = vdf_parse_text((const char *)buffer, len);
    if (!root) { ac->missing++; return; }
    VdfNode *info = vdf_child(root, "appinfo");
    if (!info) info = root;
    const char *name = vdf_string(info, "common/name", NULL);
    const char *type = vdf_string(info, "common/type", NULL);
    const char *oslist = vdf_string(info, "common/oslist", "");
    if (name) { free(app->name); app->name = strdup(name); }
    if (type) { free(app->type); app->type = lowercase_dup(type); }
    app->windows = !*oslist || strcasestr(oslist, "windows") != NULL;
    vdf_free(root);
}

int library_fetch(Cm *cm, uint32_t account_id, const SteamLicense *licenses, size_t license_count,
                  SteamApp **out, size_t *out_count, char *err, size_t errlen)
{
    *out = NULL;
    *out_count = 0;
    IdSet ids = {0};

    /* 1. Packages to app IDs. */
    for (size_t start = 0; start < license_count; start += BATCH) {
        for (int pass = 0; pass < 2; pass++) {
            /* Pass 0: own licenses; pass 1: family-shared ones, so own ownership wins. */
            PbBuf request = {0};
            size_t added = 0;
            for (size_t i = start; i < license_count && i < start + BATCH; i++) {
                int shared = licenses[i].owner_id && licenses[i].owner_id != account_id;
                if (shared != pass) continue;
                PbBuf info = {0};
                pb_u64(&info, 1, licenses[i].package_id);
                if (licenses[i].access_token) pb_u64(&info, 2, licenses[i].access_token);
                pb_msg(&request, 1, &info);
                pb_free(&info);
                added++;
            }
            int ok = 1;
            if (added) {
                PackageContext pc = { &ids, pass };
                ok = product_info(cm, &request, 3, on_package, &pc);
            }
            pb_free(&request);
            if (!ok) { snprintf(err, errlen, "Steam did not return package information"); goto fail; }
        }
    }

    /* 2. Access tokens for the apps (some app records are only visible with one). */
    uint64_t *tokens = calloc(ids.count ? ids.count : 1, sizeof(*tokens));
    if (!tokens) goto fail;
    for (size_t start = 0; start < ids.count; start += BATCH * 5) {
        PbBuf request = {0};
        for (size_t i = start; i < ids.count && i < start + BATCH * 5; i++) pb_u64(&request, 2, ids.ids[i]);
        CmPart *parts;
        if (cm_request(cm, EMSG_CLIENT_PICS_ACCESS_TOKEN_REQUEST, &request, 0, PICS_TIMEOUT_MS, &parts)) {
            for (CmPart *part = parts; part; part = part->next) {
                PbReader r = { part->body.data, part->body.data + part->body.len };
                PbField f;
                while (pb_next(&r, &f) == 1) {
                    if (f.field != 3 || f.type != PB_BYTES) continue;
                    PbField appid, token;
                    if (!pb_find(f.data, f.len, 1, &appid) || !pb_find(f.data, f.len, 2, &token)) continue;
                    for (size_t i = 0; i < ids.count; i++)
                        if (ids.ids[i] == (uint32_t)appid.value) { tokens[i] = token.value; break; }
                }
            }
            cm_parts_free(parts);
        }
        pb_free(&request);
    }

    /* 3. App records. */
    AppContext ac = { calloc(ids.count ? ids.count : 1, sizeof(SteamApp)), ids.count, 0 };
    if (!ac.apps) { free(tokens); goto fail; }
    for (size_t i = 0; i < ids.count; i++) {
        ac.apps[i].appid = ids.ids[i];
        ac.apps[i].shared = ids.shared[i];
        ac.apps[i].windows = 1;
    }
    for (size_t start = 0; start < ids.count; start += BATCH) {
        PbBuf request = {0};
        for (size_t i = start; i < ids.count && i < start + BATCH; i++) {
            PbBuf info = {0};
            pb_u64(&info, 1, ids.ids[i]);
            if (tokens[i]) pb_u64(&info, 2, tokens[i]);
            pb_msg(&request, 2, &info);
            pb_free(&info);
        }
        int ok = product_info(cm, &request, 1, on_app, &ac);
        pb_free(&request);
        if (!ok) {
            free(tokens);
            library_free(ac.apps, ac.count);
            snprintf(err, errlen, "Steam did not return app information");
            goto fail;
        }
    }
    free(tokens);
    if (ac.missing) fprintf(stderr, "[STEAM] %zu app records had no inline data\n", ac.missing);
    free(ids.ids);
    free(ids.shared);
    *out = ac.apps;
    *out_count = ac.count;
    return 1;
fail:
    free(ids.ids);
    free(ids.shared);
    return 0;
}

void library_free(SteamApp *apps, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(apps[i].name);
        free(apps[i].type);
    }
    free(apps);
}

static int by_name(const void *a, const void *b)
{
    const SteamApp *x = a, *y = b;
    const char *nx = x->name ? x->name : "", *ny = y->name ? y->name : "";
    int c = strcasecmp(nx, ny);
    return c ? c : (x->appid > y->appid) - (x->appid < y->appid);
}

static void append(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (!*buf) return;
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap *= 2;
        char *grown = realloc(*buf, *cap);
        if (!grown) { free(*buf); *buf = NULL; return; }
        *buf = grown;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static void append_json_string(char **buf, size_t *len, size_t *cap, const char *s)
{
    append(buf, len, cap, "\"", 1);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[8];
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; append(buf, len, cap, esc, 2); }
        else if (c < 0x20) { snprintf(esc, sizeof(esc), "\\u%04x", c); append(buf, len, cap, esc, 6); }
        else append(buf, len, cap, (const char *)&c, 1);
    }
    append(buf, len, cap, "\"", 1);
}

char *library_json(const SteamApp *apps, size_t count)
{
    SteamApp *sorted = malloc((count ? count : 1) * sizeof(*sorted));
    if (!sorted) return NULL;
    memcpy(sorted, apps, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), by_name);
    size_t len = 0, cap = 4096;
    char *buf = malloc(cap);
    if (buf) buf[0] = 0;
    append(&buf, &len, &cap, "[", 1);
    for (size_t i = 0; i < count; i++) {
        char head[64];
        int n = snprintf(head, sizeof(head), "%s{\"appid\":%u,\"name\":", i ? "," : "", sorted[i].appid);
        append(&buf, &len, &cap, head, (size_t)n);
        append_json_string(&buf, &len, &cap, sorted[i].name ? sorted[i].name : "");
        append(&buf, &len, &cap, ",\"type\":", 8);
        append_json_string(&buf, &len, &cap, sorted[i].type ? sorted[i].type : "");
        n = snprintf(head, sizeof(head), ",\"windows\":%s,\"shared\":%s}",
                     sorted[i].windows ? "true" : "false", sorted[i].shared ? "true" : "false");
        append(&buf, &len, &cap, head, (size_t)n);
    }
    append(&buf, &len, &cap, "]", 1);
    free(sorted);
    return buf;
}

typedef struct {
    VdfNode *info;
} SingleApp;

static void on_single_app(void *context, uint32_t id, const uint8_t *buffer, size_t len)
{
    SingleApp *single = context;
    (void)id;
    if (!buffer || single->info) return;
    while (len && !buffer[len - 1]) len--;
    single->info = vdf_parse_text((const char *)buffer, len);
}

VdfNode *library_app_info(Cm *cm, uint32_t appid)
{
    uint64_t token = 0;
    PbBuf request = {0};
    pb_u64(&request, 2, appid);
    CmPart *parts;
    if (cm_request(cm, EMSG_CLIENT_PICS_ACCESS_TOKEN_REQUEST, &request, 0, PICS_TIMEOUT_MS, &parts)) {
        PbField f;
        for (CmPart *part = parts; part; part = part->next) {
            PbReader r = { part->body.data, part->body.data + part->body.len };
            while (pb_next(&r, &f) == 1) {
                PbField tf;
                if (f.field == 3 && f.type == PB_BYTES && pb_find(f.data, f.len, 2, &tf)) token = tf.value;
            }
        }
        cm_parts_free(parts);
    }
    pb_free(&request);
    PbBuf info = {0};
    pb_u64(&info, 1, appid);
    if (token) pb_u64(&info, 2, token);
    pb_msg(&request, 2, &info);
    pb_free(&info);
    SingleApp single = { NULL };
    product_info(cm, &request, 1, on_single_app, &single);
    pb_free(&request);
    return single.info;
}
