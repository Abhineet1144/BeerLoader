#include "install.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "content.h"
#include "library.h"
#include "vdf.h"

#define WORKERS 8
#define MAX_SERVERS 16
#define MAX_DEPOTS 64
#define CHUNK_ATTEMPTS 6

enum { JOB_QUEUED, JOB_PREPARING, JOB_VERIFYING, JOB_DOWNLOADING, JOB_COMMITTING, JOB_PAUSED, JOB_DONE,
       JOB_ERROR, JOB_CANCELLED };
static const char *const JOB_STATES[] = { "queued", "preparing", "verifying", "downloading", "committing",
                                          "paused", "installed", "error", "cancelled" };
static const char *const ACTIONS[] = { "install", "update", "verify", "move" };

typedef struct Job {
    InstallRequest req;
    uint32_t *owned;
    int state;
    int pause_requested, cancel_requested;
    char error[512];
    uint64_t done, total;        /* bytes for the current phase */
    double rate;                 /* bytes per second, smoothed */
    struct Job *next;
} Job;

struct Installer {
    Cm *cm;
    InstallEventFn event;
    void *context;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    Job *queue;
    uint64_t bandwidth;
};

/* ---------------------------------------------------------------- small helpers */

static void json_escape(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; s && *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = 0;
}

static int mkdirs(const char *path, mode_t mode)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, mode) && errno != EEXIST) return 0;
        *p = '/';
    }
    return !mkdir(tmp, mode) || errno == EEXIST;
}

static int mkdirs_parent(const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return 1;
    *slash = 0;
    return mkdirs(tmp, 0755);
}

/* Manifest paths must stay inside the install directory. */
static int safe_relative(const char *name)
{
    if (!*name || name[0] == '/') return 0;
    for (const char *p = name; *p;) {
        size_t n = strcspn(p, "/");
        if (n == 0 || (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
        p += n + (p[n] == '/');
    }
    return 1;
}

static int safe_component(const char *name)
{
    return *name && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..");
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void meta_path(char *out, size_t cap, const char *folder, const char *fmt, ...)
{
    char tail[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tail, sizeof(tail), fmt, ap);
    va_end(ap);
    snprintf(out, cap, "%s/beer-steam/%s", folder, tail);
}

/* ---------------------------------------------------------------- install records */

typedef struct {
    uint32_t appid;
    char name[256], installdir[256], state[32], branch[64], language[32], buildid[32];
    uint64_t size;
    uint32_t depots[MAX_DEPOTS];
    uint64_t gids[MAX_DEPOTS];
    size_t depot_count;
    char executable[512], arguments[1024], workingdir[512];
} Record;

static void vdf_quote(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static void vdf_pair(FILE *f, int indent, const char *key, const char *value)
{
    for (int i = 0; i < indent; i++) fputc('\t', f);
    vdf_quote(f, key);
    fputs("\t\t", f);
    vdf_quote(f, value);
    fputc('\n', f);
}

static int record_save(const char *folder, const Record *r)
{
    char path[4096], tmp[4200], number[32];
    meta_path(path, sizeof(path), folder, "app_%u.vdf", r->appid);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!mkdirs_parent(path)) return 0;
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    fputs("\"BeerSteamApp\"\n{\n", f);
    snprintf(number, sizeof(number), "%u", r->appid);
    vdf_pair(f, 1, "appid", number);
    vdf_pair(f, 1, "name", r->name);
    vdf_pair(f, 1, "installdir", r->installdir);
    vdf_pair(f, 1, "state", r->state);
    vdf_pair(f, 1, "branch", r->branch);
    vdf_pair(f, 1, "language", r->language);
    vdf_pair(f, 1, "buildid", r->buildid);
    snprintf(number, sizeof(number), "%llu", (unsigned long long)r->size);
    vdf_pair(f, 1, "size", number);
    fputs("\t\"depots\"\n\t{\n", f);
    for (size_t i = 0; i < r->depot_count; i++) {
        char depot[16];
        snprintf(depot, sizeof(depot), "%u", r->depots[i]);
        snprintf(number, sizeof(number), "%llu", (unsigned long long)r->gids[i]);
        vdf_pair(f, 2, depot, number);
    }
    fputs("\t}\n\t\"launch\"\n\t{\n", f);
    vdf_pair(f, 2, "executable", r->executable);
    vdf_pair(f, 2, "arguments", r->arguments);
    vdf_pair(f, 2, "workingdir", r->workingdir);
    fputs("\t}\n}\n", f);
    int ok = !ferror(f);
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp, path) == 0;
}

static int record_load(const char *folder, uint32_t appid, Record *r)
{
    memset(r, 0, sizeof(*r));
    char path[4096];
    meta_path(path, sizeof(path), folder, "app_%u.vdf", appid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char *text = NULL;
    size_t cap = 0;
    ssize_t n = getdelim(&text, &cap, 0, f);
    fclose(f);
    VdfNode *root = n > 0 ? vdf_parse_text(text, (size_t)n) : NULL;
    free(text);
    VdfNode *app = root ? vdf_child(root, "BeerSteamApp") : NULL;
    if (!app) { vdf_free(root); return 0; }
    r->appid = (uint32_t)strtoul(vdf_string(app, "appid", "0"), NULL, 10);
    snprintf(r->name, sizeof(r->name), "%s", vdf_string(app, "name", ""));
    snprintf(r->installdir, sizeof(r->installdir), "%s", vdf_string(app, "installdir", ""));
    snprintf(r->state, sizeof(r->state), "%s", vdf_string(app, "state", ""));
    snprintf(r->branch, sizeof(r->branch), "%s", vdf_string(app, "branch", "public"));
    snprintf(r->language, sizeof(r->language), "%s", vdf_string(app, "language", "english"));
    snprintf(r->buildid, sizeof(r->buildid), "%s", vdf_string(app, "buildid", ""));
    r->size = strtoull(vdf_string(app, "size", "0"), NULL, 10);
    VdfNode *depots = vdf_child(app, "depots");
    for (VdfNode *d = depots ? depots->child : NULL; d && r->depot_count < MAX_DEPOTS; d = d->next) {
        if (!d->value) continue;
        r->depots[r->depot_count] = (uint32_t)strtoul(d->key, NULL, 10);
        r->gids[r->depot_count++] = strtoull(d->value, NULL, 10);
    }
    snprintf(r->executable, sizeof(r->executable), "%s", vdf_string(app, "launch/executable", ""));
    snprintf(r->arguments, sizeof(r->arguments), "%s", vdf_string(app, "launch/arguments", ""));
    snprintf(r->workingdir, sizeof(r->workingdir), "%s", vdf_string(app, "launch/workingdir", ""));
    vdf_free(root);
    return r->appid == appid && safe_component(r->installdir);
}

/* ---------------------------------------------------------------- app metadata */

typedef struct {
    uint32_t id;
    uint64_t gid;
    uint32_t dlc;                /* owning DLC app, or 0 */
} DepotChoice;

static int owned(const Job *job, uint32_t appid)
{
    for (size_t i = 0; i < job->req.owned_count; i++) if (job->owned[i] == appid) return 1;
    return 0;
}

static int windows_config(const VdfNode *depot, const char *key)
{
    const char *oslist = vdf_string(depot, key, "");
    return !*oslist || strcasestr(oslist, "windows") != NULL;
}

/* Picks the Windows, 64-bit (when offered), chosen-language depots of a branch. */
static size_t choose_depots(const Job *job, const VdfNode *depots, const char *branch, DepotChoice *out, size_t cap)
{
    int has64 = 0;
    for (const VdfNode *d = depots ? depots->child : NULL; d; d = d->next)
        if (d->child && windows_config(d, "config/oslist") && !strcmp(vdf_string(d, "config/osarch", ""), "64"))
            has64 = 1;
    size_t count = 0;
    for (const VdfNode *d = depots ? depots->child : NULL; d && count < cap; d = d->next) {
        char *end;
        unsigned long id = strtoul(d->key, &end, 10);
        if (!d->child || *end || !id) continue;                  /* "branches", "baselanguages", ... */
        if (vdf_child(d, "depotfromapp") || !strcmp(vdf_string(d, "sharedinstall", "0"), "1")) continue;
        if (!windows_config(d, "config/oslist")) continue;
        const char *arch = vdf_string(d, "config/osarch", "");
        if (has64 && !strcmp(arch, "32")) continue;
        const char *language = vdf_string(d, "config/language", "");
        if (*language && strcasecmp(language, job->req.language)) continue;
        if (!strcmp(vdf_string(d, "config/lowviolence", "0"), "1")) continue;
        uint32_t dlc = (uint32_t)strtoul(vdf_string(d, "dlcappid", "0"), NULL, 10);
        if (dlc && !owned(job, dlc)) continue;
        char path[160];
        snprintf(path, sizeof(path), "manifests/%s/gid", branch);
        const char *gid = vdf_string(d, path, NULL);
        if (!gid) { snprintf(path, sizeof(path), "manifests/%s", branch); gid = vdf_string(d, path, NULL); }
        uint64_t manifest = gid ? strtoull(gid, NULL, 10) : 0;
        if (!gid && job->req.beta_key[0]) {
            /* Password-protected branch: the manifest ID is AES-encrypted with the branch key. */
            snprintf(path, sizeof(path), "encryptedmanifests/%s/encrypted_gid_2", branch);
            const char *hex = vdf_string(d, path, NULL);
            uint8_t key[32], cipher[64], plain[64];
            size_t n = 0, plain_len = 0;
            for (int k = 0; k < 32; k++) {
                unsigned v = 0;
                sscanf(job->req.beta_key + 2 * k, "%2x", &v);
                key[k] = (uint8_t)v;
            }
            for (; hex && hex[0] && hex[1] && n < sizeof(cipher); hex += 2) {
                unsigned v;
                if (sscanf(hex, "%2x", &v) != 1) break;
                cipher[n++] = (uint8_t)v;
            }
            if (n && steam_ecb_decrypt(cipher, n, key, plain, &plain_len) && plain_len >= 8)
                for (int b = 7; b >= 0; b--) manifest = manifest << 8 | plain[b];
        }
        if (!manifest) continue;
        out[count].id = (uint32_t)id;
        out[count].gid = manifest;
        out[count].dlc = dlc;
        if (out[count].gid) count++;
    }
    return count;
}

static void choose_launch(const VdfNode *appinfo, const char *branch, Record *r)
{
    const VdfNode *launch = vdf_path(appinfo, "config/launch");
    const VdfNode *best = NULL;
    int best_score = -1;
    for (const VdfNode *l = launch ? launch->child : NULL; l; l = l->next) {
        const char *exe = vdf_string(l, "executable", "");
        if (!*exe || !windows_config(l, "config/oslist")) continue;
        const char *beta = vdf_string(l, "config/betakey", "");
        if (*beta && strcmp(beta, branch)) continue;
        int score = 1;
        const char *type = vdf_string(l, "type", "default");
        if (!strcasecmp(type, "default") || !*type) score += 4;
        if (!strcmp(vdf_string(l, "config/osarch", ""), "64")) score += 2;
        else if (!strcmp(vdf_string(l, "config/osarch", ""), "32")) score -= 1;
        if (strcasestr(exe, ".exe")) score += 1;
        if (score > best_score) { best = l; best_score = score; }
    }
    if (!best) return;
    snprintf(r->executable, sizeof(r->executable), "%s", vdf_string(best, "executable", ""));
    snprintf(r->arguments, sizeof(r->arguments), "%s", vdf_string(best, "arguments", ""));
    snprintf(r->workingdir, sizeof(r->workingdir), "%s", vdf_string(best, "workingdir", ""));
    for (char *p = r->executable; *p; p++) if (*p == '\\') *p = '/';
    for (char *p = r->workingdir; *p; p++) if (*p == '\\') *p = '/';
}

/* ---------------------------------------------------------------- events */

static void emit_job(Installer *in, Job *job)
{
    char name[600], error[1100];
    pthread_mutex_lock(&in->lock);
    json_escape(name, sizeof(name), job->req.name);
    json_escape(error, sizeof(error), job->error);
    char line[2400];
    snprintf(line, sizeof(line),
             "{\"event\":\"download\",\"appid\":%u,\"name\":\"%s\",\"action\":\"%s\",\"state\":\"%s\","
             "\"done\":%llu,\"total\":%llu,\"rate\":%.0f,\"error\":\"%s\"}",
             job->req.appid, name, ACTIONS[job->req.action], JOB_STATES[job->state],
             (unsigned long long)job->done, (unsigned long long)job->total, job->rate, error);
    pthread_mutex_unlock(&in->lock);
    in->event(in->context, line);
}

static void set_job(Installer *in, Job *job, int state, uint64_t done, uint64_t total)
{
    pthread_mutex_lock(&in->lock);
    job->state = state;
    job->done = done;
    job->total = total;
    job->rate = 0;
    pthread_mutex_unlock(&in->lock);
    emit_job(in, job);
}

static int job_fail(Installer *in, Job *job, const char *fmt, ...)
{
    pthread_mutex_lock(&in->lock);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(job->error, sizeof(job->error), fmt, ap);
    va_end(ap);
    job->state = JOB_ERROR;
    pthread_mutex_unlock(&in->lock);
    fprintf(stderr, "[STEAM] app %u: %s\n", job->req.appid, job->error);
    emit_job(in, job);
    return 0;
}

static int interrupted(Installer *in, Job *job)
{
    pthread_mutex_lock(&in->lock);
    int stop = job->pause_requested || job->cancel_requested;
    pthread_mutex_unlock(&in->lock);
    return stop;
}

/* ---------------------------------------------------------------- planning */

typedef struct {
    uint8_t sha[20];
    uint32_t depot_index;
    uint32_t size, compressed;
    uint32_t file;               /* index into Plan.files */
    uint64_t offset;
} Placement;

typedef struct {
    char *staging;               /* path being built */
    char *final;                 /* installed path */
    uint64_t size;
} BuildFile;

typedef struct {
    uint8_t sha[20];
    uint32_t depot_index;
    uint32_t file;               /* index into Plan.sources */
    uint64_t offset;
    uint32_t size;
} Source;

typedef struct {
    BuildFile *files;
    size_t file_count, file_cap;
    Placement *places;
    size_t place_count, place_cap;
    char **sources_paths;        /* installed files that may supply chunks */
    size_t source_path_count, source_path_cap;
    Source *sources;
    size_t source_count, source_cap;
} Plan;

#define GROW(array, count, cap, initial) \
    do { if ((count) == (cap)) { size_t c_ = (cap) ? (cap) * 2 : (initial); \
         void *g_ = realloc((array), c_ * sizeof(*(array))); if (!g_) return 0; (array) = g_; (cap) = c_; } } while (0)

static int plan_add_file(Plan *p, const char *staging, const char *final, uint64_t size)
{
    GROW(p->files, p->file_count, p->file_cap, 64);
    p->files[p->file_count].staging = strdup(staging);
    p->files[p->file_count].final = strdup(final);
    p->files[p->file_count].size = size;
    return p->files[p->file_count++].staging != NULL;
}

static int plan_add_place(Plan *p, const ManifestChunk *c, uint32_t depot_index, uint32_t file)
{
    GROW(p->places, p->place_count, p->place_cap, 1024);
    Placement *pl = &p->places[p->place_count++];
    memcpy(pl->sha, c->sha, 20);
    pl->depot_index = depot_index;
    pl->size = c->size;
    pl->compressed = c->compressed;
    pl->file = file;
    pl->offset = c->offset;
    return 1;
}

static int plan_add_sources(Plan *p, const ManifestFile *f, uint32_t depot_index, const char *final)
{
    GROW(p->sources_paths, p->source_path_count, p->source_path_cap, 64);
    p->sources_paths[p->source_path_count] = strdup(final);
    uint32_t index = (uint32_t)p->source_path_count++;
    for (size_t i = 0; i < f->chunk_count; i++) {
        GROW(p->sources, p->source_count, p->source_cap, 1024);
        Source *s = &p->sources[p->source_count++];
        memcpy(s->sha, f->chunks[i].sha, 20);
        s->depot_index = depot_index;
        s->file = index;
        s->offset = f->chunks[i].offset;
        s->size = f->chunks[i].size;
    }
    return 1;
}

static void plan_free(Plan *p)
{
    for (size_t i = 0; i < p->file_count; i++) { free(p->files[i].staging); free(p->files[i].final); }
    for (size_t i = 0; i < p->source_path_count; i++) free(p->sources_paths[i]);
    free(p->files);
    free(p->places);
    free(p->sources_paths);
    free(p->sources);
    memset(p, 0, sizeof(*p));
}

static int cmp_place(const void *a, const void *b)
{
    const Placement *x = a, *y = b;
    if (x->depot_index != y->depot_index) return x->depot_index < y->depot_index ? -1 : 1;
    return memcmp(x->sha, y->sha, 20);
}

static int cmp_source(const void *a, const void *b)
{
    const Source *x = a, *y = b;
    return memcmp(x->sha, y->sha, 20);
}

static int cmp_sha(const void *a, const void *b) { return memcmp(a, b, 20); }

static const ManifestFile *find_file(const Manifest *m, const char *name)
{
    for (size_t i = 0; m && i < m->count; i++)
        if (!strcmp(m->files[i].name, name)) return &m->files[i];
    return NULL;
}

/* SHA-1 of each chunk range of an installed file; 1 when every chunk matches. */
static int file_matches(const char *path, const ManifestFile *f, Installer *in, Job *job)
{
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != f->size) return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int ok = 1;
    uint8_t *buf = NULL;
    size_t cap = 0;
    for (size_t i = 0; ok && i < f->chunk_count; i++) {
        const ManifestChunk *c = &f->chunks[i];
        if (c->size > cap) { free(buf); cap = c->size; buf = malloc(cap); if (!buf) { ok = 0; break; } }
        uint8_t digest[20];
        ok = pread(fd, buf, c->size, (off_t)c->offset) == (ssize_t)c->size;
        if (ok) { SHA1(buf, c->size, digest); ok = !memcmp(digest, c->sha, 20); }
        pthread_mutex_lock(&in->lock);
        job->done += c->size;
        pthread_mutex_unlock(&in->lock);
    }
    free(buf);
    close(fd);
    return ok;
}

/* ---------------------------------------------------------------- download */

typedef struct {
    Installer *in;
    Job *job;
    Plan *plan;
    const DepotChoice *depots;
    const uint8_t (*keys)[32];
    CdnServer *servers;
    int server_count;
    size_t *starts;              /* first placement of each unique chunk */
    size_t unique;
    size_t next;                 /* next unique chunk to take */
    uint8_t *done_shas;          /* sorted SHA-1s finished in an earlier run */
    size_t done_count;
    FILE *progress;
    pthread_mutex_t lock;
    int server_seed;
    int exited;                  /* workers that have finished */
    int failed;
    char error[512];
    double limit_start;
    uint64_t limit_bytes;
} Download;

static int chunk_finished(const Download *d, const uint8_t *sha)
{
    return d->done_count && bsearch(sha, d->done_shas, d->done_count, 20, cmp_sha) != NULL;
}

static int write_placements(Download *d, size_t first, const uint8_t *data)
{
    const Placement *p0 = &d->plan->places[first];
    for (size_t i = first; i < d->plan->place_count && !cmp_place(&d->plan->places[i], p0); i++) {
        const Placement *p = &d->plan->places[i];
        int fd = open(d->plan->files[p->file].staging, O_WRONLY | O_CLOEXEC);
        if (fd < 0) return 0;
        int ok = pwrite(fd, data, p->size, (off_t)p->offset) == (ssize_t)p->size;
        close(fd);
        if (!ok) return 0;
    }
    return 1;
}

/* Copies a chunk from an installed file when one has it (verified by SHA-1). */
static int copy_local(Download *d, const Placement *p, uint8_t *out)
{
    Source key;
    memcpy(key.sha, p->sha, 20);
    Source *s = d->plan->source_count ? bsearch(&key, d->plan->sources, d->plan->source_count, sizeof(Source),
                                                cmp_source) : NULL;
    if (!s) return 0;
    while (s > d->plan->sources && !memcmp((s - 1)->sha, p->sha, 20)) s--;
    for (; s < d->plan->sources + d->plan->source_count && !memcmp(s->sha, p->sha, 20); s++) {
        if (s->size != p->size) continue;
        int fd = open(d->plan->sources_paths[s->file], O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        int ok = pread(fd, out, p->size, (off_t)s->offset) == (ssize_t)p->size;
        close(fd);
        uint8_t digest[20];
        if (ok) { SHA1(out, p->size, digest); if (!memcmp(digest, p->sha, 20)) return 1; }
    }
    return 0;
}

static void throttle(Download *d, uint64_t bytes)
{
    pthread_mutex_lock(&d->in->lock);
    uint64_t limit = d->in->bandwidth;
    pthread_mutex_unlock(&d->in->lock);
    if (!limit) return;
    pthread_mutex_lock(&d->lock);
    double now = now_seconds();
    if (now - d->limit_start > 2.0) { d->limit_start = now; d->limit_bytes = 0; }
    d->limit_bytes += bytes;
    double wait = (double)d->limit_bytes / (double)limit - (now - d->limit_start);
    pthread_mutex_unlock(&d->lock);
    if (wait > 0) usleep((useconds_t)(wait > 2.0 ? 2e6 : wait * 1e6));
}

static void *download_worker(void *arg)
{
    Download *d = arg;
    HttpConn conn;
    pthread_mutex_lock(&d->lock);
    int server = d->server_seed++ % d->server_count;
    pthread_mutex_unlock(&d->lock);
    http_init(&conn, d->servers[server].host, d->servers[server].https);
    uint8_t *buf = NULL;
    size_t cap = 0;
    for (;;) {
        pthread_mutex_lock(&d->lock);
        size_t index = d->failed ? d->unique : d->next++;
        pthread_mutex_unlock(&d->lock);
        if (index >= d->unique || interrupted(d->in, d->job)) break;
        const Placement *p = &d->plan->places[d->starts[index]];
        if (chunk_finished(d, p->sha)) {
            pthread_mutex_lock(&d->in->lock);
            d->job->done += p->compressed;
            pthread_mutex_unlock(&d->in->lock);
            continue;
        }
        if (p->size > cap) { free(buf); cap = p->size; buf = malloc(cap); if (!buf) break; }
        int ok = copy_local(d, p, buf);
        char err[512] = "";
        for (int attempt = 0; !ok && attempt < CHUNK_ATTEMPTS && !interrupted(d->in, d->job); attempt++) {
            ManifestChunk chunk = { .offset = 0, .size = p->size, .compressed = p->compressed };
            memcpy(chunk.sha, p->sha, 20);
            int status = chunk_download(&conn, d->depots[p->depot_index].id, &chunk, d->keys[p->depot_index],
                                        buf, err, sizeof(err));
            if (status == 1) { ok = 1; throttle(d, p->compressed); break; }
            /* Try the next server after a failure. */
            http_close(&conn);
            server = (server + 1) % d->server_count;
            http_init(&conn, d->servers[server].host, d->servers[server].https);
            if (attempt) usleep(250000u << (attempt > 4 ? 4 : attempt));
        }
        if (ok && !write_placements(d, d->starts[index], buf)) {
            ok = 0;
            snprintf(err, sizeof(err), "could not write to the library folder: %s", strerror(errno));
        }
        if (!ok) {
            if (interrupted(d->in, d->job)) break;
            pthread_mutex_lock(&d->lock);
            if (!d->failed) { d->failed = 1; snprintf(d->error, sizeof(d->error), "%s", err[0] ? err : "download failed"); }
            pthread_mutex_unlock(&d->lock);
            break;
        }
        pthread_mutex_lock(&d->lock);
        fwrite(p->sha, 1, 20, d->progress);
        pthread_mutex_unlock(&d->lock);
        pthread_mutex_lock(&d->in->lock);
        d->job->done += p->compressed;
        pthread_mutex_unlock(&d->in->lock);
    }
    free(buf);
    http_close(&conn);
    free(conn.buf);
    pthread_mutex_lock(&d->lock);
    d->exited++;
    pthread_mutex_unlock(&d->lock);
    return NULL;
}

static uint8_t *load_done(const char *path, size_t *count)
{
    *count = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t n = size > 0 ? (size_t)size / 20 : 0;
    uint8_t *shas = n ? malloc(n * 20) : NULL;
    if (shas && fread(shas, 20, n, f) == n) {
        qsort(shas, n, 20, cmp_sha);
        *count = n;
    } else {
        free(shas);
        shas = NULL;
    }
    fclose(f);
    return shas;
}

/* Removes now-empty directories from path up to (not including) stop. */
static void prune_dirs(const char *path, const char *stop)
{
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    size_t stop_len = strlen(stop);
    for (char *slash; (slash = strrchr(dir, '/')) && (size_t)(slash - dir) > stop_len;) {
        *slash = 0;
        if (rmdir(dir)) break;
    }
}

static void remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[4096];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat st;
            if (!lstat(child, &st) && S_ISDIR(st.st_mode)) remove_tree(child);
            else unlink(child);
        }
        closedir(dir);
    }
    rmdir(path);
}

/* ---------------------------------------------------------------- moving an install */

/* Copies a tree (files, directories, symlinks); counts copied bytes into job->done. */
static int copy_tree(Installer *in, Job *job, const char *from, const char *to)
{
    struct stat st;
    if (lstat(from, &st)) return 0;
    if (S_ISLNK(st.st_mode)) {
        char target[4096];
        ssize_t n = readlink(from, target, sizeof(target) - 1);
        if (n < 0) return 0;
        target[n] = 0;
        return !symlink(target, to);
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(to, 0755) && errno != EEXIST) return 0;
        DIR *dir = opendir(from);
        if (!dir) return 0;
        struct dirent *e;
        int ok = 1;
        while (ok && (e = readdir(dir))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char a[4096], b[4096];
            snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
            snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
            ok = copy_tree(in, job, a, b) && !interrupted(in, job);
        }
        closedir(dir);
        return ok;
    }
    int src = open(from, O_RDONLY | O_CLOEXEC);
    int dst = src >= 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777) : -1;
    int ok = src >= 0 && dst >= 0;
    static __thread uint8_t buf[1 << 20];
    while (ok) {
        ssize_t n = read(src, buf, sizeof(buf));
        if (n <= 0) { ok = n == 0; break; }
        ok = write(dst, buf, (size_t)n) == n;
        pthread_mutex_lock(&in->lock);
        job->done += (uint64_t)n;
        pthread_mutex_unlock(&in->lock);
    }
    if (src >= 0) close(src);
    if (dst >= 0 && close(dst)) ok = 0;
    return ok;
}

static int run_move(Installer *in, Job *job)
{
    const InstallRequest *req = &job->req;
    Record r;
    if (!record_load(req->folder, req->appid, &r)) return job_fail(in, job, "the app is not installed in %s", req->folder);
    if (!strcmp(req->folder, req->target)) return job_fail(in, job, "the app is already in that folder");
    Record existing;
    if (record_load(req->target, req->appid, &existing)) return job_fail(in, job, "the app is already installed in %s", req->target);
    char from[4096], to[4096], common[4096];
    snprintf(from, sizeof(from), "%s/common/%s", req->folder, r.installdir);
    snprintf(common, sizeof(common), "%s/common", req->target);
    snprintf(to, sizeof(to), "%s/%s", common, r.installdir);
    struct stat st;
    if (!lstat(to, &st)) return job_fail(in, job, "%s already exists", to);
    if (!mkdirs(common, 0755)) return job_fail(in, job, "could not create %s", common);
    struct statvfs fs;
    if (!statvfs(common, &fs) && (uint64_t)fs.f_bavail * fs.f_frsize < r.size)
        return job_fail(in, job, "not enough free space in %s", req->target);
    set_job(in, job, JOB_DOWNLOADING, 0, r.size);
    if (rename(from, to)) {
        if (errno != EXDEV) return job_fail(in, job, "could not move %s: %s", from, strerror(errno));
        /* Another filesystem: copy everything first, delete the source only after it all arrived. */
        if (!copy_tree(in, job, from, to)) {
            remove_tree(to);
            if (interrupted(in, job)) return 0;
            return job_fail(in, job, "copying to %s failed: %s", req->target, strerror(errno));
        }
        remove_tree(from);
    }
    set_job(in, job, JOB_COMMITTING, 0, 1);
    for (size_t i = 0; i < r.depot_count; i++) {
        char a[4096], b[4096];
        meta_path(a, sizeof(a), req->folder, "depots/%u_%llu.manifest", r.depots[i], (unsigned long long)r.gids[i]);
        meta_path(b, sizeof(b), req->target, "depots/%u_%llu.manifest", r.depots[i], (unsigned long long)r.gids[i]);
        mkdirs_parent(b);
        Manifest m;
        if (manifest_load(a, &m)) { manifest_save(&m, b); manifest_free(&m); }
        unlink(a);
    }
    if (!record_save(req->target, &r)) return job_fail(in, job, "could not write the install record in %s", req->target);
    char old[4096];
    meta_path(old, sizeof(old), req->folder, "app_%u.vdf", req->appid);
    unlink(old);
    set_job(in, job, JOB_DONE, 1, 1);
    return 1;
}

/* ---------------------------------------------------------------- the job */

static int run_job(Installer *in, Job *job)
{
    if (job->req.action == INSTALL_ACTION_MOVE) return run_move(in, job);
    const InstallRequest *req = &job->req;
    set_job(in, job, JOB_PREPARING, 0, 0);
    if (!cm_logged_on(in->cm)) return job_fail(in, job, "not signed in to Steam");

    VdfNode *root = library_app_info(in->cm, req->appid);
    VdfNode *appinfo = root ? vdf_child(root, "appinfo") : NULL;
    if (!appinfo) { vdf_free(root); return job_fail(in, job, "Steam did not return this app's information"); }

    Record old;
    int have_old = record_load(req->folder, req->appid, &old);
    Record rec;
    memset(&rec, 0, sizeof(rec));
    rec.appid = req->appid;
    snprintf(rec.name, sizeof(rec.name), "%s", vdf_string(appinfo, "common/name", req->name));
    snprintf(rec.installdir, sizeof(rec.installdir), "%s",
             have_old ? old.installdir : vdf_string(appinfo, "config/installdir", ""));
    if (!safe_component(rec.installdir)) snprintf(rec.installdir, sizeof(rec.installdir), "app_%u", req->appid);
    snprintf(rec.branch, sizeof(rec.branch), "%s", req->branch[0] ? req->branch : have_old ? old.branch : "public");
    snprintf(rec.language, sizeof(rec.language), "%s", req->language[0] ? req->language : "english");
    char build_path[160];
    snprintf(build_path, sizeof(build_path), "depots/branches/%s/buildid", rec.branch);
    snprintf(rec.buildid, sizeof(rec.buildid), "%s", vdf_string(appinfo, build_path, ""));
    choose_launch(appinfo, rec.branch, &rec);

    DepotChoice depots[MAX_DEPOTS];
    size_t depot_count = choose_depots(job, vdf_child(appinfo, "depots"), rec.branch, depots, MAX_DEPOTS);
    vdf_free(root);
    if (req->action == INSTALL_ACTION_VERIFY && have_old) {
        /* Verify what is installed, not what is newest. */
        depot_count = old.depot_count < MAX_DEPOTS ? old.depot_count : MAX_DEPOTS;
        for (size_t i = 0; i < depot_count; i++) { depots[i].id = old.depots[i]; depots[i].gid = old.gids[i]; depots[i].dlc = 0; }
        snprintf(rec.buildid, sizeof(rec.buildid), "%s", old.buildid);
        snprintf(rec.branch, sizeof(rec.branch), "%s", old.branch);
    }
    if (!depot_count) return job_fail(in, job, "no Windows content is available for this app on branch %s", rec.branch);

    char install_dir[4096], staging_dir[4096], done_path[4096];
    snprintf(install_dir, sizeof(install_dir), "%s/common/%s", req->folder, rec.installdir);
    meta_path(staging_dir, sizeof(staging_dir), req->folder, "staging/%u", req->appid);
    meta_path(done_path, sizeof(done_path), req->folder, "staging/%u.done", req->appid);

    CdnServer servers[MAX_SERVERS];
    int server_count = content_servers(in->cm, servers, MAX_SERVERS);
    if (!server_count) return job_fail(in, job, "Steam did not list any content servers");

    /* Keys and manifests. A depot whose key is refused is not owned (for example unowned DLC). */
    uint8_t (*keys)[32] = calloc(MAX_DEPOTS, 32);
    Manifest *manifests = calloc(MAX_DEPOTS, sizeof(Manifest));
    Manifest *olds = calloc(MAX_DEPOTS, sizeof(Manifest));
    Plan plan;
    memset(&plan, 0, sizeof(plan));
    int ok = 0;
    if (!keys || !manifests || !olds) { job_fail(in, job, "out of memory"); goto out; }
    size_t kept = 0;
    for (size_t i = 0; i < depot_count; i++) {
        if (interrupted(in, job)) goto out;
        int eresult = content_depot_key(in->cm, req->appid, depots[i].id, keys[kept]);
        if (eresult != ERESULT_OK) {
            fprintf(stderr, "[STEAM] app %u: depot %u skipped (%s)\n", req->appid, depots[i].id, cm_eresult_name(eresult));
            continue;
        }
        char manifest_path[4096];
        meta_path(manifest_path, sizeof(manifest_path), req->folder, "depots/%u_%llu.manifest", depots[i].id,
                  (unsigned long long)depots[i].gid);
        if (!manifest_load(manifest_path, &manifests[kept])) {
            uint64_t code = 0;
            content_manifest_code(in->cm, req->appid, depots[i].id, depots[i].gid, rec.branch, &code);
            char err[512] = "could not download the depot manifest";
            int got = 0;
            for (int s = 0; s < server_count && !got; s++) {
                HttpConn conn;
                http_init(&conn, servers[s].host, servers[s].https);
                got = manifest_download(&conn, depots[i].id, depots[i].gid, code, keys[kept], &manifests[kept],
                                        err, sizeof(err));
                http_close(&conn);
                free(conn.buf);
            }
            if (!got) { job_fail(in, job, "%s", err); goto out; }
        }
        for (size_t o = 0; have_old && o < old.depot_count; o++) {
            if (old.depots[o] != depots[i].id) continue;
            char old_path[4096];
            meta_path(old_path, sizeof(old_path), req->folder, "depots/%u_%llu.manifest", old.depots[o],
                      (unsigned long long)old.gids[o]);
            manifest_load(old_path, &olds[kept]);
        }
        if (kept != i) depots[kept] = depots[i];
        kept++;
    }
    depot_count = kept;
    if (!depot_count) { job_fail(in, job, "Steam refused the content keys: this account may not own the app"); goto out; }

    /* Plan: which files must be built, and from which chunks. */
    uint64_t hash_total = 0, build_bytes = 0;
    for (size_t i = 0; i < depot_count; i++) hash_total += manifests[i].size;
    if (req->action == INSTALL_ACTION_VERIFY) set_job(in, job, JOB_VERIFYING, 0, hash_total);
    for (size_t i = 0; i < depot_count; i++) {
        for (size_t f = 0; f < manifests[i].count; f++) {
            const ManifestFile *file = &manifests[i].files[f];
            if (!safe_relative(file->name)) { job_fail(in, job, "manifest has an unsafe path: %s", file->name); goto out; }
            if (file->flags & (DEPOT_FILE_DIRECTORY | DEPOT_FILE_SYMLINK)) continue;
            char final[4096], staging[4096];
            snprintf(final, sizeof(final), "%s/%s", install_dir, file->name);
            snprintf(staging, sizeof(staging), "%s/%s", staging_dir, file->name);
            const ManifestFile *previous = find_file(&olds[i], file->name);
            if (previous && !plan_add_sources(&plan, previous, (uint32_t)i, final)) { job_fail(in, job, "out of memory"); goto out; }
            int unchanged;
            if (req->action == INSTALL_ACTION_VERIFY) {
                unchanged = file_matches(final, file, in, job);
                if (!unchanged && !plan_add_sources(&plan, file, (uint32_t)i, final)) { job_fail(in, job, "out of memory"); goto out; }
            } else {
                struct stat st;
                unchanged = previous && !memcmp(previous->sha, file->sha, 20) && previous->size == file->size &&
                            !stat(final, &st) && (uint64_t)st.st_size == file->size;
            }
            if (interrupted(in, job)) goto out;
            if (unchanged) continue;
            if (!plan_add_file(&plan, staging, final, file->size)) { job_fail(in, job, "out of memory"); goto out; }
            build_bytes += file->size;
            for (size_t c = 0; c < file->chunk_count; c++)
                if (!plan_add_place(&plan, &file->chunks[c], (uint32_t)i, (uint32_t)(plan.file_count - 1))) {
                    job_fail(in, job, "out of memory");
                    goto out;
                }
        }
    }

    /* Space: the staged files are complete copies until they are renamed into place. */
    mkdirs(staging_dir, 0755);
    struct statvfs fs;
    if (!statvfs(staging_dir, &fs) && (uint64_t)fs.f_bavail * fs.f_frsize < build_bytes) {
        job_fail(in, job, "not enough free space: %.1f GB needed, %.1f GB free", build_bytes / 1e9,
                 (double)fs.f_bavail * fs.f_frsize / 1e9);
        goto out;
    }
    if (!have_old || req->action == INSTALL_ACTION_INSTALL) {
        Record pending = rec;
        snprintf(pending.state, sizeof(pending.state), "%s", have_old ? old.state : "downloading");
        if (have_old) {
            pending.depot_count = old.depot_count;
            memcpy(pending.depots, old.depots, sizeof(old.depots));
            memcpy(pending.gids, old.gids, sizeof(old.gids));
            snprintf(pending.buildid, sizeof(pending.buildid), "%s", old.buildid);
        }
        record_save(req->folder, &pending);
    }

    /* Staging files at their final sizes. */
    for (size_t i = 0; i < plan.file_count; i++) {
        if (!mkdirs_parent(plan.files[i].staging)) { job_fail(in, job, "could not create %s", plan.files[i].staging); goto out; }
        int fd = open(plan.files[i].staging, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0 || ftruncate(fd, (off_t)plan.files[i].size)) {
            if (fd >= 0) close(fd);
            job_fail(in, job, "could not create %s: %s", plan.files[i].staging, strerror(errno));
            goto out;
        }
        close(fd);
    }

    /* Unique chunks (a chunk may be placed in several files) and the download. */
    qsort(plan.places, plan.place_count, sizeof(Placement), cmp_place);
    qsort(plan.sources, plan.source_count, sizeof(Source), cmp_source);
    size_t *starts = malloc((plan.place_count ? plan.place_count : 1) * sizeof(size_t));
    if (!starts) { job_fail(in, job, "out of memory"); goto out; }
    size_t unique = 0;
    uint64_t download_total = 0;
    for (size_t i = 0; i < plan.place_count; i++)
        if (!i || cmp_place(&plan.places[i], &plan.places[i - 1])) {
            starts[unique++] = i;
            download_total += plan.places[i].compressed;
        }
    Download d;
    memset(&d, 0, sizeof(d));
    d.in = in;
    d.job = job;
    d.plan = &plan;
    d.depots = depots;
    d.keys = (const uint8_t (*)[32])keys;
    d.servers = servers;
    d.server_count = server_count;
    d.starts = starts;
    d.unique = unique;
    d.done_shas = load_done(done_path, &d.done_count);
    d.progress = fopen(done_path, "ab");
    pthread_mutex_init(&d.lock, NULL);
    d.limit_start = now_seconds();
    if (!d.progress) { free(starts); free(d.done_shas); job_fail(in, job, "could not write %s", done_path); goto out; }
    set_job(in, job, JOB_DOWNLOADING, 0, download_total);
    fprintf(stderr, "[STEAM] app %u: %zu depots, %zu files to build, %zu chunks (%.1f MB)\n", req->appid,
            depot_count, plan.file_count, unique, download_total / 1e6);
    pthread_t threads[WORKERS];
    int started = 0;
    for (int t = 0; t < WORKERS && (size_t)t < (unique ? unique : 1); t++)
        if (!pthread_create(&threads[t], NULL, download_worker, &d)) started++;
    double last = now_seconds();
    uint64_t last_done = 0;
    for (;;) {
        pthread_mutex_lock(&d.lock);
        int finished = d.exited == started;
        pthread_mutex_unlock(&d.lock);
        if (finished || interrupted(in, job)) break;
        usleep(500000);
        double now = now_seconds();
        pthread_mutex_lock(&in->lock);
        double instant = (double)(job->done - last_done) / (now - last);
        job->rate = job->rate ? job->rate * 0.7 + instant * 0.3 : instant;
        last_done = job->done;
        pthread_mutex_unlock(&in->lock);
        last = now;
        emit_job(in, job);
    }
    for (int t = 0; t < started; t++) pthread_join(threads[t], NULL);
    fclose(d.progress);
    free(d.done_shas);
    free(starts);
    pthread_mutex_destroy(&d.lock);
    if (interrupted(in, job)) goto out;
    if (d.failed) { job_fail(in, job, "%s", d.error); goto out; }

    /* Commit: directories, staged files, symlinks, removals, manifests, record. */
    set_job(in, job, JOB_COMMITTING, 0, plan.file_count);
    mkdirs(install_dir, 0755);
    for (size_t i = 0; i < depot_count; i++)
        for (size_t f = 0; f < manifests[i].count; f++)
            if (manifests[i].files[f].flags & DEPOT_FILE_DIRECTORY) {
                char path[4096];
                snprintf(path, sizeof(path), "%s/%s", install_dir, manifests[i].files[f].name);
                mkdirs(path, 0755);
            }
    for (size_t i = 0; i < plan.file_count; i++) {
        if (!mkdirs_parent(plan.files[i].final) || rename(plan.files[i].staging, plan.files[i].final)) {
            job_fail(in, job, "could not move %s into place: %s", plan.files[i].final, strerror(errno));
            goto out;
        }
    }
    for (size_t i = 0; i < depot_count; i++)
        for (size_t f = 0; f < manifests[i].count; f++) {
            const ManifestFile *file = &manifests[i].files[f];
            if (!(file->flags & DEPOT_FILE_SYMLINK) || !file->link) continue;
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", install_dir, file->name);
            mkdirs_parent(path);
            unlink(path);
            if (symlink(file->link, path)) fprintf(stderr, "[STEAM] symlink %s: %s\n", path, strerror(errno));
        }
    for (size_t i = 0; i < depot_count; i++)
        for (size_t f = 0; f < olds[i].count; f++) {
            const ManifestFile *file = &olds[i].files[f];
            if ((file->flags & DEPOT_FILE_DIRECTORY) || find_file(&manifests[i], file->name) || !safe_relative(file->name))
                continue;
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", install_dir, file->name);
            if (!unlink(path)) prune_dirs(path, install_dir);
        }
    /* Launch paths come from app config written for Windows, where case does not matter:
     * use the spelling the depot actually installed. */
    for (size_t i = 0; i < depot_count; i++)
        for (size_t f = 0; f < manifests[i].count; f++) {
            if (!strcasecmp(manifests[i].files[f].name, rec.executable))
                snprintf(rec.executable, sizeof(rec.executable), "%s", manifests[i].files[f].name);
            if (rec.workingdir[0] && !strcasecmp(manifests[i].files[f].name, rec.workingdir))
                snprintf(rec.workingdir, sizeof(rec.workingdir), "%s", manifests[i].files[f].name);
        }
    rec.depot_count = depot_count;
    rec.size = 0;
    for (size_t i = 0; i < depot_count; i++) {
        char path[4096];
        meta_path(path, sizeof(path), req->folder, "depots/%u_%llu.manifest", depots[i].id,
                  (unsigned long long)depots[i].gid);
        mkdirs_parent(path);
        manifest_save(&manifests[i], path);
        rec.depots[i] = depots[i].id;
        rec.gids[i] = depots[i].gid;
        rec.size += manifests[i].size;
    }
    for (size_t o = 0; have_old && o < old.depot_count; o++) {
        int still = 0;
        for (size_t i = 0; i < depot_count; i++) still |= rec.depots[i] == old.depots[o] && rec.gids[i] == old.gids[o];
        if (still) continue;
        char path[4096];
        meta_path(path, sizeof(path), req->folder, "depots/%u_%llu.manifest", old.depots[o], (unsigned long long)old.gids[o]);
        unlink(path);
    }
    snprintf(rec.state, sizeof(rec.state), "installed");
    if (!record_save(req->folder, &rec)) { job_fail(in, job, "could not write the install record"); goto out; }
    unlink(done_path);
    remove_tree(staging_dir);
    ok = 1;
    set_job(in, job, JOB_DONE, plan.file_count, plan.file_count);

out:
    for (size_t i = 0; manifests && i < MAX_DEPOTS; i++) manifest_free(&manifests[i]);
    for (size_t i = 0; olds && i < MAX_DEPOTS; i++) manifest_free(&olds[i]);
    free(manifests);
    free(olds);
    free(keys);
    plan_free(&plan);
    return ok;
}

/* ---------------------------------------------------------------- queue */

static void *queue_thread(void *arg)
{
    Installer *in = arg;
    for (;;) {
        pthread_mutex_lock(&in->lock);
        Job *job = NULL;
        for (;;) {
            for (job = in->queue; job; job = job->next)
                if (job->state == JOB_QUEUED && !job->pause_requested) break;
            if (job) break;
            pthread_cond_wait(&in->changed, &in->lock);
        }
        pthread_mutex_unlock(&in->lock);

        run_job(in, job);

        pthread_mutex_lock(&in->lock);
        int cancelled = job->cancel_requested, paused = job->pause_requested && !cancelled;
        if (paused) { job->state = JOB_PAUSED; job->rate = 0; }
        else if (cancelled) job->state = JOB_CANCELLED;
        pthread_mutex_unlock(&in->lock);
        if (paused) emit_job(in, job);
        if (cancelled) {
            char path[4096];
            meta_path(path, sizeof(path), job->req.folder, "staging/%u", job->req.appid);
            remove_tree(path);
            meta_path(path, sizeof(path), job->req.folder, "staging/%u.done", job->req.appid);
            unlink(path);
            Record r;
            if (record_load(job->req.folder, job->req.appid, &r) && !strcmp(r.state, "downloading")) {
                meta_path(path, sizeof(path), job->req.folder, "app_%u.vdf", job->req.appid);
                unlink(path);
            }
            emit_job(in, job);                   /* after cleanup, so listings already match */
        }
        if (!paused) {
            pthread_mutex_lock(&in->lock);
            for (Job **link = &in->queue; *link; link = &(*link)->next)
                if (*link == job) { *link = job->next; break; }
            pthread_mutex_unlock(&in->lock);
            free(job->owned);
            free(job);
        }
    }
    return NULL;
}

Installer *installer_new(Cm *cm, InstallEventFn event, void *context)
{
    Installer *in = calloc(1, sizeof(*in));
    if (!in) return NULL;
    in->cm = cm;
    in->event = event;
    in->context = context;
    pthread_mutex_init(&in->lock, NULL);
    pthread_cond_init(&in->changed, NULL);
    pthread_t thread;
    if (pthread_create(&thread, NULL, queue_thread, in)) { free(in); return NULL; }
    pthread_detach(thread);
    return in;
}

int installer_enqueue(Installer *in, const InstallRequest *request, char *err, size_t errlen)
{
    if (!request->folder[0] || !mkdirs(request->folder, 0755) || access(request->folder, W_OK)) {
        snprintf(err, errlen, "the library folder %s is not writable", request->folder);
        return 0;
    }
    Job *job = calloc(1, sizeof(*job));
    if (!job) { snprintf(err, errlen, "out of memory"); return 0; }
    job->req = *request;
    job->owned = request->owned_count ? malloc(request->owned_count * sizeof(uint32_t)) : NULL;
    if (job->owned) memcpy(job->owned, request->owned, request->owned_count * sizeof(uint32_t));
    else job->req.owned_count = 0;
    job->req.owned = NULL;
    pthread_mutex_lock(&in->lock);
    for (Job *j = in->queue; j; j = j->next)
        if (j->req.appid == request->appid) {
            pthread_mutex_unlock(&in->lock);
            free(job->owned);
            free(job);
            snprintf(err, errlen, "this app is already in the download queue");
            return 0;
        }
    Job **tail = &in->queue;
    while (*tail) tail = &(*tail)->next;
    *tail = job;
    pthread_cond_broadcast(&in->changed);
    pthread_mutex_unlock(&in->lock);
    emit_job(in, job);
    return 1;
}

static Job *find_job(Installer *in, uint32_t appid)
{
    for (Job *j = in->queue; j; j = j->next) if (j->req.appid == appid) return j;
    return NULL;
}

void installer_pause(Installer *in, uint32_t appid)
{
    pthread_mutex_lock(&in->lock);
    Job *job = find_job(in, appid);
    int queued = job && job->state == JOB_QUEUED;
    if (job) job->pause_requested = 1;
    if (queued) job->state = JOB_PAUSED;
    pthread_mutex_unlock(&in->lock);
    if (queued) emit_job(in, job);
}

void installer_resume(Installer *in, uint32_t appid)
{
    pthread_mutex_lock(&in->lock);
    Job *job = find_job(in, appid);
    int resumed = job && job->state == JOB_PAUSED;
    if (resumed) { job->pause_requested = 0; job->state = JOB_QUEUED; job->error[0] = 0; }
    pthread_cond_broadcast(&in->changed);
    pthread_mutex_unlock(&in->lock);
    if (resumed) emit_job(in, job);
}

void installer_cancel(Installer *in, uint32_t appid)
{
    pthread_mutex_lock(&in->lock);
    Job *job = find_job(in, appid);
    int idle = job && (job->state == JOB_QUEUED || job->state == JOB_PAUSED);
    if (job) job->cancel_requested = 1;
    if (idle) {
        /* Not running: drop it here (the queue thread only sees running jobs finish). */
        for (Job **link = &in->queue; *link; link = &(*link)->next)
            if (*link == job) { *link = job->next; break; }
        job->state = JOB_CANCELLED;
    }
    pthread_mutex_unlock(&in->lock);
    if (idle) {
        emit_job(in, job);
        char path[4096];
        meta_path(path, sizeof(path), job->req.folder, "staging/%u", appid);
        remove_tree(path);
        meta_path(path, sizeof(path), job->req.folder, "staging/%u.done", appid);
        unlink(path);
        Record r;
        if (record_load(job->req.folder, appid, &r) && !strcmp(r.state, "downloading")) {
            meta_path(path, sizeof(path), job->req.folder, "app_%u.vdf", appid);
            unlink(path);
        }
        free(job->owned);
        free(job);
    }
}

void installer_set_bandwidth(Installer *in, uint64_t bytes_per_second)
{
    pthread_mutex_lock(&in->lock);
    in->bandwidth = bytes_per_second;
    pthread_mutex_unlock(&in->lock);
}

int installer_busy_with(Installer *in, uint32_t appid)
{
    pthread_mutex_lock(&in->lock);
    int busy = find_job(in, appid) != NULL;
    pthread_mutex_unlock(&in->lock);
    return busy;
}

char *installer_queue_json(Installer *in)
{
    size_t cap = 256, len = 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    strcpy(out, "[");
    pthread_mutex_lock(&in->lock);
    for (Job *j = in->queue; j; j = j->next) {
        char name[600], item[1024];
        json_escape(name, sizeof(name), j->req.name);
        int n = snprintf(item, sizeof(item), "%s{\"appid\":%u,\"name\":\"%s\",\"action\":\"%s\",\"state\":\"%s\","
                         "\"done\":%llu,\"total\":%llu}", len > 1 ? "," : "", j->req.appid, name,
                         ACTIONS[j->req.action], JOB_STATES[j->state], (unsigned long long)j->done,
                         (unsigned long long)j->total);
        if (len + (size_t)n + 2 > cap) {
            while (len + (size_t)n + 2 > cap) cap *= 2;
            char *grown = realloc(out, cap);
            if (!grown) break;
            out = grown;
        }
        memcpy(out + len, item, (size_t)n);
        len += (size_t)n;
    }
    pthread_mutex_unlock(&in->lock);
    out[len++] = ']';
    out[len] = 0;
    return out;
}

/* ---------------------------------------------------------------- records on disk */

char *installs_json(const char *const *folders, size_t count)
{
    size_t cap = 1024, len = 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    strcpy(out, "[");
    for (size_t i = 0; i < count; i++) {
        char dir_path[4096];
        snprintf(dir_path, sizeof(dir_path), "%s/beer-steam", folders[i]);
        DIR *dir = opendir(dir_path);
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir))) {
            unsigned appid;
            char expected[64];
            if (sscanf(e->d_name, "app_%u.vdf", &appid) != 1) continue;
            snprintf(expected, sizeof(expected), "app_%u.vdf", appid);
            if (strcmp(e->d_name, expected)) continue;
            Record r;
            if (!record_load(folders[i], appid, &r)) continue;
            char name[600], folder[2100], installdir[600], state[64], branch[160], buildid[64], exe[1100],
                 args[2100], cwd[1100], item[9000];
            json_escape(name, sizeof(name), r.name);
            json_escape(folder, sizeof(folder), folders[i]);
            json_escape(installdir, sizeof(installdir), r.installdir);
            json_escape(state, sizeof(state), r.state);
            json_escape(branch, sizeof(branch), r.branch);
            json_escape(buildid, sizeof(buildid), r.buildid);
            json_escape(exe, sizeof(exe), r.executable);
            json_escape(args, sizeof(args), r.arguments);
            json_escape(cwd, sizeof(cwd), r.workingdir);
            int n = snprintf(item, sizeof(item),
                             "%s{\"appid\":%u,\"name\":\"%s\",\"folder\":\"%s\",\"installdir\":\"%s\",\"state\":\"%s\","
                             "\"branch\":\"%s\",\"buildid\":\"%s\",\"size\":%llu,\"executable\":\"%s\","
                             "\"arguments\":\"%s\",\"workingdir\":\"%s\"}",
                             len > 1 ? "," : "", appid, name, folder, installdir, state, branch, buildid,
                             (unsigned long long)r.size, exe, args, cwd);
            if (n < 0 || (size_t)n >= sizeof(item)) continue;
            if (len + (size_t)n + 2 > cap) {
                while (len + (size_t)n + 2 > cap) cap *= 2;
                char *grown = realloc(out, cap);
                if (!grown) { closedir(dir); free(out); return NULL; }
                out = grown;
            }
            memcpy(out + len, item, (size_t)n);
            len += (size_t)n;
        }
        closedir(dir);
    }
    out[len++] = ']';
    out[len] = 0;
    return out;
}

int install_remove(const char *folder, uint32_t appid, char *err, size_t errlen)
{
    Record r;
    if (!record_load(folder, appid, &r)) { snprintf(err, errlen, "app %u is not installed in %s", appid, folder); return 0; }
    char install_dir[4096];
    snprintf(install_dir, sizeof(install_dir), "%s/common/%s", folder, r.installdir);
    for (size_t i = 0; i < r.depot_count; i++) {
        char path[4096];
        meta_path(path, sizeof(path), folder, "depots/%u_%llu.manifest", r.depots[i], (unsigned long long)r.gids[i]);
        Manifest m;
        if (manifest_load(path, &m)) {
            for (size_t f = 0; f < m.count; f++) {
                if ((m.files[f].flags & DEPOT_FILE_DIRECTORY) || !safe_relative(m.files[f].name)) continue;
                char file[4096];
                snprintf(file, sizeof(file), "%s/%s", install_dir, m.files[f].name);
                if (!unlink(file)) prune_dirs(file, install_dir);
            }
            manifest_free(&m);
        }
        unlink(path);
    }
    rmdir(install_dir);                  /* only when nothing else (saves, mods) remains */
    char path[4096];
    meta_path(path, sizeof(path), folder, "staging/%u", appid);
    remove_tree(path);
    meta_path(path, sizeof(path), folder, "staging/%u.done", appid);
    unlink(path);
    meta_path(path, sizeof(path), folder, "app_%u.vdf", appid);
    unlink(path);
    return 1;
}
