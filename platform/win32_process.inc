/* Child processes, Toolhelp process snapshots and named pipes.
 *
 * Every guest process is its own Linux process running its own loader, so a Win32 process is a host process:
 * pids are host pids, CreateProcess starts another loader on the target exe, and named pipes are unix sockets in
 * the runtime directory.  Each loader publishes <runtime>/<pid>.proc (parent pid, exe name, exe path) so that
 * CreateToolhelp32Snapshot can list the guest processes that are alive.
 *
 * Included from loader.c after the file-handle table; the hooks used by the wait, close, read and open paths
 * are forward-declared near the top of loader.c. */

extern char **environ;

/* ---- Runtime directory and process registry ---- */
static const char *beer_runtime_dir(void)
{
    static char dir[256];
    if (dir[0]) return dir;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime) snprintf(dir, sizeof(dir), "%s/beer", runtime);
    else snprintf(dir, sizeof(dir), "/tmp/beer-%d", (int)getuid());
    mkdir(dir, 0700);
    return dir;
}

static char g_proc_file[320];
static void beer_proc_unregister(void) { if (g_proc_file[0]) unlink(g_proc_file); }

static const char *beer_path_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

static void beer_proc_register(void)
{
    snprintf(g_proc_file, sizeof(g_proc_file), "%s/%d.proc", beer_runtime_dir(), (int)getpid());
    FILE *f = fopen(g_proc_file, "w");
    if (!f) { g_proc_file[0] = 0; return; }
    fprintf(f, "%d\n%s\n%s\n%llx\n", (int)getppid(), beer_path_basename(g_exe_host_path ? g_exe_host_path : ""),
            g_exe_path ? g_exe_path : "", (unsigned long long)(uintptr_t)g_img);
    fclose(f);
    atexit(beer_proc_unregister);
    signal(SIGPIPE, SIG_IGN);      /* a vanished pipe peer is an error code, not a signal */
}

/* Is <pid> a live loader process (same executable as this one)? */
static int beer_pid_is_loader(pid_t pid)
{
    char link[64], mine[PATH_MAX], theirs[PATH_MAX];
    snprintf(link, sizeof(link), "/proc/%d/exe", (int)pid);
    ssize_t a = readlink("/proc/self/exe", mine, sizeof(mine) - 1);
    ssize_t b = readlink(link, theirs, sizeof(theirs) - 1);
    if (a <= 0 || b <= 0) return 0;
    mine[a] = 0; theirs[b] = 0;
    return !strcmp(mine, theirs);
}

/* ---- Process handles ---- */
#define BEER_PROC_HANDLE_BASE 0xD1000000u
#define BEER_SNAP_HANDLE_BASE 0xD2000000u
#define BEER_MAX_PROCS 128
#define BEER_MAX_SNAPS 32
#define BEER_SNAP_MAX_ENTRIES 128

typedef struct { int used; pid_t pid; int child; int exited; u32 exit_code; } BeerProc;
static BeerProc g_procs[BEER_MAX_PROCS];
static pthread_mutex_t g_proc_mtx = PTHREAD_MUTEX_INITIALIZER;

typedef struct { u32 pid, ppid; char name[260]; } BeerProcEntry;
#define BEER_SNAP_MAX_THREADS 512
typedef struct { int used, count, next; BeerProcEntry entries[BEER_SNAP_MAX_ENTRIES]; u32 tids[BEER_SNAP_MAX_THREADS]; int tcount, tnext; } BeerSnapshot;
static BeerSnapshot g_snaps[BEER_MAX_SNAPS];

static BeerProc *beer_proc_from_handle(u64 h)
{
    u32 hv = (u32)h;
    if (h >> 32 || (hv & 0xFF000000u) != BEER_PROC_HANDLE_BASE) return NULL;
    u32 id = hv - BEER_PROC_HANDLE_BASE;
    return id < BEER_MAX_PROCS && g_procs[id].used ? &g_procs[id] : NULL;
}

static BeerSnapshot *beer_snap_from_handle(u64 h)
{
    u32 hv = (u32)h;
    if (h >> 32 || (hv & 0xFF000000u) != BEER_SNAP_HANDLE_BASE) return NULL;
    u32 id = hv - BEER_SNAP_HANDLE_BASE;
    return id < BEER_MAX_SNAPS && g_snaps[id].used ? &g_snaps[id] : NULL;
}

static u64 beer_proc_alloc(pid_t pid, int child)
{
    pthread_mutex_lock(&g_proc_mtx);
    for (u32 id = 0; id < BEER_MAX_PROCS; ++id) {
        if (g_procs[id].used) continue;
        g_procs[id] = (BeerProc){ .used = 1, .pid = pid, .child = child };
        pthread_mutex_unlock(&g_proc_mtx);
        return (u64)(BEER_PROC_HANDLE_BASE + id);
    }
    pthread_mutex_unlock(&g_proc_mtx);
    g_last_error = 4;   /* ERROR_TOO_MANY_OPEN_FILES */
    return 0;
}

/* Refreshes p->exited / p->exit_code; returns 1 when the process has ended. */
static int beer_proc_exited(BeerProc *p)
{
    pthread_mutex_lock(&g_proc_mtx);
    if (!p->exited) {
        int status = 0;
        pid_t r = p->child ? waitpid(p->pid, &status, WNOHANG) : 0;
        if (r == p->pid) {
            p->exited = 1;
            p->exit_code = WIFEXITED(status) ? (u32)WEXITSTATUS(status) : 0xC0000005u;
        } else if (r < 0 || (!p->child && kill(p->pid, 0) != 0)) {
            p->exited = 1;
            p->exit_code = 0;
        } else if (!p->child) {
            /* A zombie that is not ours still answers kill(0); /proc tells them apart. */
            char path[64], line[256];
            snprintf(path, sizeof(path), "/proc/%d/stat", (int)p->pid);
            FILE *f = fopen(path, "r");
            if (f) {
                if (fgets(line, sizeof(line), f)) {
                    char *close = strrchr(line, ')');
                    if (close && close[1] == ' ' && close[2] == 'Z') p->exited = 1;
                }
                fclose(f);
            }
        }
    }
    int exited = p->exited;
    pthread_mutex_unlock(&g_proc_mtx);
    return exited;
}

/* Wait hooks: (u64)-2 / -2 mean "not a process handle". */
static u64 beer_proc_wait(u64 h, u32 ms)
{
    BeerProc *p = beer_proc_from_handle(h);
    if (!p) return (u64)-2;
    u64 waited = 0;
    while (!beer_proc_exited(p)) {
        if (ms != 0xFFFFFFFFu && waited >= ms) return 0x102u;      /* WAIT_TIMEOUT */
        struct timespec delay = {0, 2000000L};
        nanosleep(&delay, NULL);
        waited += 2;
    }
    return 0;
}

static int beer_proc_poll(u64 h)
{
    BeerProc *p = beer_proc_from_handle(h);
    if (!p) return -2;
    return beer_proc_exited(p);
}

/* Returns 1 when <h> was another process (killed) or a handle this module owns. */
static int beer_proc_terminate(u64 h, u32 code)
{
    BeerProc *p = beer_proc_from_handle(h);
    if (!p) return 0;
    (void)code;
    if (p->pid != getpid()) kill(p->pid, SIGKILL);
    return 1;
}

static int beer_proc_close(u64 h)
{
    BeerProc *p = beer_proc_from_handle(h);
    if (p) {
        pthread_mutex_lock(&g_proc_mtx);
        if (p->child && !p->exited) {
            /* The child keeps running; reap it later through a detached waiter-less zombie at worst. */
        }
        p->used = 0;
        pthread_mutex_unlock(&g_proc_mtx);
        return 1;
    }
    BeerSnapshot *s = beer_snap_from_handle(h);
    if (s) { s->used = 0; return 1; }
    return 0;
}

/* ---- CreateProcess ---- */
static char **g_loader_argv;
static int g_loader_argc;
static void beer_proc_save_args(int argc, char **argv) { g_loader_argc = argc; g_loader_argv = argv; }

static void beer_proc_utf16_to_utf8(const u16 *w, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; w && w[i] && o + 4 < cap; ++i) {
        u32 c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else { out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
}

static u64 __attribute__((ms_abi)) impl_WriteFile(u64 h, const void *buf, u32 n, u32 *written, u64 ov);

/* Console utilities games shell out to for machine/user fingerprints (whoami, hostname). There is no Windows system directory
 * to run them from, so they are answered in-process: the text goes to the redirected stdout handle (STARTF_USESTDHANDLES) and
 * the "process" is born exited with code 0. Returns 0 when <name> is not one of them, -1 on failure, 1 when answered. */
static int beer_builtin_console_tool(const char *name, const u8 *startup, u8 *info)
{
    const char *base = name;
    for (const char *c = name; *c; ++c) if (*c == '\\' || *c == '/') base = c + 1;
    char tool[32];
    size_t n = 0;
    for (; base[n] && n < sizeof(tool) - 1; ++n) tool[n] = (char)((base[n] >= 'A' && base[n] <= 'Z') ? base[n] + 32 : base[n]);
    tool[n] = 0;
    if (n > 4 && !strcmp(tool + n - 4, ".exe")) tool[n - 4] = 0;

    char host[256] = "beer";
    if (gethostname(host, sizeof(host) - 1) != 0) snprintf(host, sizeof(host), "beer");
    host[sizeof(host) - 1] = 0;
    for (char *c = host; *c; ++c) if (*c == '.') { *c = 0; break; }

    char text[600];
    if (!strcmp(tool, "hostname")) snprintf(text, sizeof(text), "%s\r\n", host);
    else if (!strcmp(tool, "whoami")) {
        char lower[256];
        size_t i = 0;
        for (; host[i] && i < sizeof(lower) - 1; ++i) lower[i] = (char)((host[i] >= 'A' && host[i] <= 'Z') ? host[i] + 32 : host[i]);
        lower[i] = 0;
        snprintf(text, sizeof(text), "%s\\player\r\n", lower);
    } else return 0;

    const u32 flags = startup ? *(const u32 *)(startup + 60) : 0;
    if ((flags & 0x100u) && startup) {
        u64 out = *(const u64 *)(startup + 88);
        u32 written = 0;
        if (out && out != (u64)-1) impl_WriteFile(out, text, (u32)strlen(text), &written, 0);
    }
    u64 process = beer_proc_alloc(getpid(), 0);
    u64 thread = event_alloc(1, 1);
    if (!process) return -1;
    g_procs[process - BEER_PROC_HANDLE_BASE].exited = 1;
    if (info) {
        memcpy(info + 0, &process, 8);
        memcpy(info + 8, &thread, 8);
        u32 id = (u32)getpid() + 1;
        memcpy(info + 16, &id, 4);
        memcpy(info + 20, &id, 4);
    }
    fprintf(stderr, "[PROCESS] %s answered in-process\n", tool);
    g_last_error = 0;
    return 1;
}

static u64 beer_create_process(const char *application, const char *command_line, u32 flags, u8 *info, const u8 *startup)
{
    char token[PATH_MAX];
    token[0] = 0;
    if (application && *application) {
        snprintf(token, sizeof(token), "%s", application);
    } else if (command_line) {
        const char *p = command_line;
        while (*p == ' ' || *p == '\t') ++p;
        size_t n = 0;
        if (*p == '"') {
            for (++p; *p && *p != '"' && n + 1 < sizeof(token); ++p) token[n++] = *p;
        } else {
            for (; *p && *p != ' ' && *p != '\t' && n + 1 < sizeof(token); ++p) token[n++] = *p;
        }
        token[n] = 0;
    }
    if (!token[0]) { g_last_error = 87; return 0; }
    {
        int tool = beer_builtin_console_tool(token, startup, info);
        if (tool) return tool > 0;
    }

    char host[PATH_MAX];
    struct stat st;
    int found = 0;
    for (int attempt = 0; attempt < 2 && !found; ++attempt) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), attempt ? "%s.exe" : "%s", token);
        if (resolve_windows_host_path(candidate, host, sizeof(host)) && stat(host, &st) == 0 && S_ISREG(st.st_mode))
            found = 1;
    }
    if (!found) { g_last_error = 2; return 0; }        /* ERROR_FILE_NOT_FOUND */
    {
        unsigned char magic[2] = {0, 0};
        FILE *f = fopen(host, "rb");
        if (f) { if (fread(magic, 1, 2, f) != 2) magic[0] = 0; fclose(f); }
        if (magic[0] != 'M' || magic[1] != 'Z') { g_last_error = 193; return 0; }   /* ERROR_BAD_EXE_FORMAT */
    }

    char command[8192];
    if (command_line && *command_line) snprintf(command, sizeof(command), "%s", command_line);
    else snprintf(command, sizeof(command), "\"%s\"", token);

    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) { g_last_error = 5; return 0; }
    self[n] = 0;

    char *argv[64];
    int argc = 0;
    argv[argc++] = self;
    for (int i = 1; i < g_loader_argc && argc < 56; ++i) {
        const char *a = g_loader_argv[i];
        if (!strncmp(a, "--", 2) && strncmp(a, "--cmdline=", 10)) argv[argc++] = g_loader_argv[i];
    }
    char cmdline_option[8192 + 16];
    snprintf(cmdline_option, sizeof(cmdline_option), "--cmdline=%s", command);
    argv[argc++] = cmdline_option;
    argv[argc++] = host;
    argv[argc] = NULL;

    if (flags & 0x4u)
        fprintf(stderr, "[PROCESS] CREATE_SUSPENDED is not supported; starting %s immediately\n", host);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, self, NULL, NULL, argv, environ);
    if (rc != 0) { g_last_error = 5; return 0; }       /* ERROR_ACCESS_DENIED */
    fprintf(stderr, "[PROCESS] CreateProcess pid=%d: %s\n", (int)pid, command);

    u64 process = beer_proc_alloc(pid, 1);
    u64 thread = event_alloc(1, 1);       /* closable stand-in for the primary thread handle */
    if (!process) return 0;
    if (info) {
        memcpy(info + 0, &process, 8);
        memcpy(info + 8, &thread, 8);
        u32 id = (u32)pid;
        memcpy(info + 16, &id, 4);
        memcpy(info + 20, &id, 4);
    }
    g_last_error = 0;
    return 1;
}

static u64 __attribute__((ms_abi))
beer_CreateProcessA(const char *application, const char *command_line, void *process_sa, void *thread_sa,
                    u32 inherit, u32 flags, void *environment, const char *directory, void *startup, u8 *info)
{
    (void)process_sa; (void)thread_sa;
    if (getenv("BEER_PROCESS_DIAGNOSTICS")) {
        fprintf(stderr, "[PROCESS] CreateProcessA inherit=%u flags=0x%x directory=%s environment=%p\n", inherit, flags, directory ? directory : "(null)", environment);
        if (environment) {
            const char *e = (const char *)environment;
            for (int k = 0; k < 64 && *e; ++k) { fprintf(stderr, "[PROCESS]   env %s\n", e); e += strlen(e) + 1; }
        }
    }
    return beer_create_process(application, command_line, flags, info, startup);
}

static u64 __attribute__((ms_abi))
beer_CreateProcessW(const u16 *application, const u16 *command_line, void *process_sa, void *thread_sa,
                    u32 inherit, u32 flags, void *environment, const u16 *directory, void *startup, u8 *info)
{
    (void)process_sa; (void)thread_sa; (void)inherit; (void)environment; (void)directory;
    char app[PATH_MAX], cmd[8192];
    beer_proc_utf16_to_utf8(application, app, sizeof(app));
    beer_proc_utf16_to_utf8(command_line, cmd, sizeof(cmd));
    return beer_create_process(application ? app : NULL, command_line ? cmd : NULL, flags, info, startup);
}

static u64 __attribute__((ms_abi)) beer_GetProcessId(u64 process)
{
    BeerProc *p = beer_proc_from_handle(process);
    if (p) return (u64)(u32)p->pid;
    if (process == 0x7FF0ull || process == (u64)-1) return (u64)(u32)getpid();
    g_last_error = 6;
    return 0;
}

/* ---- Toolhelp snapshots ---- */
static void beer_snapshot_add(BeerSnapshot *s, pid_t pid)
{
    if (s->count >= BEER_SNAP_MAX_ENTRIES) return;
    char path[320];
    snprintf(path, sizeof(path), "%s/%d.proc", beer_runtime_dir(), (int)pid);
    if (!beer_pid_is_loader(pid)) { unlink(path); return; }      /* the process died without cleaning up */
    FILE *f = fopen(path, "r");
    if (!f) return;
    char ppid_line[32], name[260];
    BeerProcEntry *e = &s->entries[s->count];
    memset(e, 0, sizeof(*e));
    if (fgets(ppid_line, sizeof(ppid_line), f) && fgets(name, sizeof(name), f)) {
        name[strcspn(name, "\r\n")] = 0;
        e->pid = (u32)pid;
        e->ppid = (u32)atoi(ppid_line);
        snprintf(e->name, sizeof(e->name), "%s", name);
        s->count++;
    }
    fclose(f);
}

static u64 __attribute__((ms_abi)) beer_CreateToolhelp32Snapshot(u32 flags, u32 pid)
{
    (void)pid;
    for (u32 id = 0; id < BEER_MAX_SNAPS; ++id) {
        if (g_snaps[id].used) continue;
        BeerSnapshot *s = &g_snaps[id];
        memset(s, 0, sizeof(*s));
        s->used = 1;
        if (flags & 0x2u) {                      /* TH32CS_SNAPPROCESS */
            DIR *dir = opendir(beer_runtime_dir());
            struct dirent *entry;
            while (dir && (entry = readdir(dir)) != NULL) {
                size_t len = strlen(entry->d_name);
                if (len > 5 && !strcmp(entry->d_name + len - 5, ".proc") && atoi(entry->d_name) > 0)
                    beer_snapshot_add(s, (pid_t)atoi(entry->d_name));
            }
            if (dir) closedir(dir);
        }
        if (flags & 0x4u)                        /* TH32CS_SNAPTHREAD: this process's guest threads */
            s->tcount = (int)beer_guest_thread_ids(s->tids, BEER_SNAP_MAX_THREADS);
        g_last_error = 0;
        return (u64)(BEER_SNAP_HANDLE_BASE + id);
    }
    g_last_error = 4;
    return INVALID_HANDLE_VALUE64;
}

static u64 beer_process32_fill(u64 snapshot, u8 *out, int wide, int first)
{
    BeerSnapshot *s = beer_snap_from_handle(snapshot);
    if (!s || !out) { g_last_error = s ? 87 : 6; return 0; }
    if (first) s->next = 0;
    if (s->next >= s->count) { g_last_error = 18; return 0; }     /* ERROR_NO_MORE_FILES */
    const BeerProcEntry *e = &s->entries[s->next++];
    u32 size = *(u32 *)out;
    u32 expect = wide ? 568u : 304u;
    if (size < expect) { g_last_error = 24; return 0; }           /* ERROR_BAD_LENGTH */
    memset(out + 4, 0, expect - 4);
    *(u32 *)(out + 8) = e->pid;
    *(u32 *)(out + 28) = 1;                                       /* cntThreads */
    *(u32 *)(out + 32) = e->ppid;
    *(s32 *)(out + 36) = 8;                                       /* NORMAL_PRIORITY_CLASS base */
    for (size_t i = 0; e->name[i] && i < 259; ++i) {
        if (wide) ((u16 *)(out + 44))[i] = (u8)e->name[i];
        else out[44 + i] = (u8)e->name[i];
    }
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) beer_Process32First(u64 s, u8 *e)  { return beer_process32_fill(s, e, 0, 1); }
static u64 __attribute__((ms_abi)) beer_Process32Next(u64 s, u8 *e)   { return beer_process32_fill(s, e, 0, 0); }
static u64 __attribute__((ms_abi)) beer_Process32FirstW(u64 s, u8 *e) { return beer_process32_fill(s, e, 1, 1); }
static u64 __attribute__((ms_abi)) beer_Process32NextW(u64 s, u8 *e)  { return beer_process32_fill(s, e, 1, 0); }

/* THREADENTRY32: dwSize, cntUsage, th32ThreadID, th32OwnerProcessID, tpBasePri, tpDeltaPri, dwFlags (28 bytes). */
static u64 beer_thread32_fill(u64 snapshot, u8 *out, int first)
{
    BeerSnapshot *s = beer_snap_from_handle(snapshot);
    if (!s || !out) { g_last_error = s ? 87 : 6; return 0; }
    if (*(u32 *)out < 28) { g_last_error = 24; return 0; }
    if (first) s->tnext = 0;
    if (s->tnext >= s->tcount) { g_last_error = 18; return 0; }   /* ERROR_NO_MORE_FILES */
    memset(out + 4, 0, 24);
    *(u32 *)(out + 8) = s->tids[s->tnext++];
    *(u32 *)(out + 12) = (u32)getpid();
    *(s32 *)(out + 16) = 8;
    g_last_error = 0;
    return 1;
}
static u64 __attribute__((ms_abi)) beer_Thread32First(u64 s, u8 *e) { return beer_thread32_fill(s, e, 1); }
static u64 __attribute__((ms_abi)) beer_Thread32Next(u64 s, u8 *e)  { return beer_thread32_fill(s, e, 0); }

/* ---- Named pipes (unix stream sockets) ---- */
typedef struct { int used, fd, refs; char name[128]; char path[320]; } BeerPipeListener;
typedef struct { u8 is_pipe, server, connected, nowait; int listener; } BeerPipeInfo;
static BeerPipeListener g_pipe_listeners[32];
static BeerPipeInfo g_pipe_info[MAX_FHANDLES];
static pthread_mutex_t g_pipe_mtx = PTHREAD_MUTEX_INITIALIZER;

/* "\\.\pipe\Some\Name" -> "some_name"; returns 0 when <name> is not in the pipe namespace. */
static int beer_pipe_name(const char *name, char *out, size_t cap)
{
    if (!name || name[0] != '\\' || name[1] != '\\' || (name[2] != '.' && name[2] != '?') || name[3] != '\\' ||
        strncasecmp(name + 4, "pipe\\", 5) != 0)
        return 0;
    size_t o = 0;
    for (const char *p = name + 9; *p && o + 1 < cap; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[o++] = (c == '\\' || c == '/' || c == ' ') ? '_' : c;
    }
    out[o] = 0;
    return o > 0;
}

static int beer_pipe_connect_path(const char *path)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (snprintf(address.sun_path, sizeof(address.sun_path), "%s", path) >= (int)sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}

/* Finds or creates the listening socket for <name>; returns its index or -1 (g_last_error set). */
static int beer_pipe_listener_acquire(const char *name)
{
    pthread_mutex_lock(&g_pipe_mtx);
    for (int i = 0; i < 32; ++i)
        if (g_pipe_listeners[i].used && !strcmp(g_pipe_listeners[i].name, name)) {
            g_pipe_listeners[i].refs++;
            pthread_mutex_unlock(&g_pipe_mtx);
            return i;
        }
    int slot = -1;
    for (int i = 0; i < 32 && slot < 0; ++i) if (!g_pipe_listeners[i].used) slot = i;
    if (slot < 0) { pthread_mutex_unlock(&g_pipe_mtx); g_last_error = 4; return -1; }
    BeerPipeListener *l = &g_pipe_listeners[slot];
    memset(l, 0, sizeof(*l));
    snprintf(l->name, sizeof(l->name), "%s", name);
    snprintf(l->path, sizeof(l->path), "%s/pipe-%s.sock", beer_runtime_dir(), name);

    int probe = beer_pipe_connect_path(l->path);
    if (probe >= 0) {                       /* another process already serves this pipe */
        close(probe);
        pthread_mutex_unlock(&g_pipe_mtx);
        g_last_error = 231;                 /* ERROR_PIPE_BUSY */
        return -1;
    }
    unlink(l->path);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int fd = -1;
    if (snprintf(address.sun_path, sizeof(address.sun_path), "%s", l->path) < (int)sizeof(address.sun_path))
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 16) != 0) {
        if (fd >= 0) close(fd);
        pthread_mutex_unlock(&g_pipe_mtx);
        g_last_error = 5;
        return -1;
    }
    chmod(l->path, 0600);
    l->used = 1; l->fd = fd; l->refs = 1;
    pthread_mutex_unlock(&g_pipe_mtx);
    return slot;
}

/* Called from fh_close for every released file handle. */
static void beer_pipe_released(u32 id)
{
    if (id >= MAX_FHANDLES || !g_pipe_info[id].is_pipe) return;
    int listener = g_pipe_info[id].server ? g_pipe_info[id].listener : -1;
    memset(&g_pipe_info[id], 0, sizeof(g_pipe_info[id]));
    if (listener < 0) return;
    pthread_mutex_lock(&g_pipe_mtx);
    BeerPipeListener *l = &g_pipe_listeners[listener];
    if (l->used && --l->refs <= 0) {
        close(l->fd);
        unlink(l->path);
        l->used = 0;
    }
    pthread_mutex_unlock(&g_pipe_mtx);
}

static int beer_pipe_is_pipe(u64 h)
{
    if (h < 0xF0000001ULL || h >= 0xF0000000ULL + MAX_FHANDLES) return 0;
    return g_pipe_info[(u32)(h - 0xF0000000ULL)].is_pipe;
}

static u64 beer_create_named_pipe(const char *name, u32 open_mode, u32 pipe_mode)
{
    (void)open_mode;
    char key[128];
    if (!beer_pipe_name(name, key, sizeof(key))) { g_last_error = 123; return INVALID_HANDLE_VALUE64; }   /* ERROR_INVALID_NAME */
    int listener = beer_pipe_listener_acquire(key);
    if (listener < 0) return INVALID_HANDLE_VALUE64;
    int fd = fcntl(g_pipe_listeners[listener].fd, F_DUPFD_CLOEXEC, 3);
    u64 handle = fd >= 0 ? fh_alloc(fd, name, g_pipe_listeners[listener].path) : INVALID_HANDLE_VALUE64;
    if (handle == INVALID_HANDLE_VALUE64) {
        if (fd >= 0) close(fd);
        pthread_mutex_lock(&g_pipe_mtx);
        g_pipe_listeners[listener].refs--;
        pthread_mutex_unlock(&g_pipe_mtx);
        return handle;
    }
    BeerPipeInfo *info = &g_pipe_info[(u32)(handle - 0xF0000000ULL)];
    info->is_pipe = 1; info->server = 1; info->connected = 0;
    info->nowait = (pipe_mode & 1u) ? 1 : 0;      /* PIPE_NOWAIT */
    info->listener = listener;
    signal(SIGPIPE, SIG_IGN);
    g_last_error = 0;
    return handle;
}

static u64 __attribute__((ms_abi))
beer_CreateNamedPipeA(const char *name, u32 open_mode, u32 pipe_mode, u32 max_instances, u32 out_buffer,
                      u32 in_buffer, u32 timeout, void *security)
{
    (void)max_instances; (void)out_buffer; (void)in_buffer; (void)timeout; (void)security;
    return beer_create_named_pipe(name, open_mode, pipe_mode);
}
static u64 __attribute__((ms_abi))
beer_CreateNamedPipeW(const u16 *name, u32 open_mode, u32 pipe_mode, u32 max_instances, u32 out_buffer,
                      u32 in_buffer, u32 timeout, void *security)
{
    (void)max_instances; (void)out_buffer; (void)in_buffer; (void)timeout; (void)security;
    char narrow[512];
    beer_proc_utf16_to_utf8(name, narrow, sizeof(narrow));
    return beer_create_named_pipe(narrow, open_mode, pipe_mode);
}

typedef struct { u64 handle; WIN_OVERLAPPED *overlapped; } BeerPipeAccept;

static int beer_pipe_attach_connection(u64 handle, int connection)
{
    u32 id = (u32)(handle - 0xF0000000ULL);
    pthread_mutex_lock(&g_fh_mtx);
    int old = g_fh_used[id] ? g_fh_fd[id] : -1;
    if (old >= 0) g_fh_fd[id] = connection;
    pthread_mutex_unlock(&g_fh_mtx);
    if (old < 0) return 0;
    close(old);
    g_pipe_info[id].connected = 1;
    return 1;
}

/* A server instance is born in the listening state and a client's CreateFile connects it without any
 * ConnectNamedPipe call, so I/O on an unconnected instance first adopts a pending client. Returns 0 with
 * ERROR_PIPE_LISTENING while nobody has connected yet. */
static int beer_pipe_ensure_connected(u64 handle)
{
    BeerPipeInfo *info = &g_pipe_info[(u32)(handle - 0xF0000000ULL)];
    if (!info->server || info->connected) return 1;
    int lfd = g_pipe_listeners[info->listener].fd;
    struct pollfd p = { .fd = lfd, .events = POLLIN };
    int c = -1;
    if (poll(&p, 1, 0) > 0) c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    if (c < 0) { g_last_error = 536; return 0; }       /* ERROR_PIPE_LISTENING */
    if (!beer_pipe_attach_connection(handle, c)) { close(c); g_last_error = 6; return 0; }
    return 1;
}

static void *beer_pipe_accept_thread(void *arg)
{
    BeerPipeAccept job = *(BeerPipeAccept *)arg;
    free(arg);
    BeerPipeInfo *info = &g_pipe_info[(u32)(job.handle - 0xF0000000ULL)];
    int lfd = g_pipe_listeners[info->listener].fd, c;
    do { c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC); } while (c < 0 && errno == EINTR);
    if (c >= 0 && !beer_pipe_attach_connection(job.handle, c)) { close(c); c = -1; }
    if (job.overlapped) {
        job.overlapped->Internal = c >= 0 ? 0 : (u64)0xC0000120u;     /* STATUS_CANCELLED */
        job.overlapped->InternalHigh = 0;
        if (job.overlapped->hEvent) impl_SetEvent(job.overlapped->hEvent);
    }
    return NULL;
}

static u64 __attribute__((ms_abi)) beer_ConnectNamedPipe(u64 handle, WIN_OVERLAPPED *overlapped)
{
    if (!beer_pipe_is_pipe(handle) || !g_pipe_info[(u32)(handle - 0xF0000000ULL)].server) { g_last_error = 6; return 0; }
    BeerPipeInfo *info = &g_pipe_info[(u32)(handle - 0xF0000000ULL)];
    if (info->connected) { g_last_error = 535; return 0; }            /* ERROR_PIPE_CONNECTED */
    int lfd = g_pipe_listeners[info->listener].fd;
    if (info->nowait) {
        struct pollfd p = { .fd = lfd, .events = POLLIN };
        if (poll(&p, 1, 0) <= 0) { g_last_error = 536; return 0; }    /* ERROR_PIPE_LISTENING */
    } else if (overlapped) {
        BeerPipeAccept *job = malloc(sizeof(*job));
        pthread_t thread;
        if (!job) { g_last_error = 8; return 0; }
        job->handle = handle; job->overlapped = overlapped;
        overlapped->Internal = 0x103;                                  /* STATUS_PENDING */
        if (pthread_create(&thread, NULL, beer_pipe_accept_thread, job) != 0) { free(job); g_last_error = 8; return 0; }
        pthread_detach(thread);
        g_last_error = 997;                                            /* ERROR_IO_PENDING */
        return 0;
    }
    int c;
    do { c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC); } while (c < 0 && errno == EINTR);
    if (c < 0 || !beer_pipe_attach_connection(handle, c)) { if (c >= 0) close(c); g_last_error = 109; return 0; }
    g_last_error = 0;
    return 1;
}

static u64 __attribute__((ms_abi)) beer_DisconnectNamedPipe(u64 handle)
{
    if (!beer_pipe_is_pipe(handle) || !g_pipe_info[(u32)(handle - 0xF0000000ULL)].server) { g_last_error = 6; return 0; }
    u32 id = (u32)(handle - 0xF0000000ULL);
    int fresh = fcntl(g_pipe_listeners[g_pipe_info[id].listener].fd, F_DUPFD_CLOEXEC, 3);
    if (fresh < 0) { g_last_error = 8; return 0; }
    pthread_mutex_lock(&g_fh_mtx);
    int old = g_fh_used[id] ? g_fh_fd[id] : -1;
    if (old >= 0) g_fh_fd[id] = fresh;
    pthread_mutex_unlock(&g_fh_mtx);
    if (old < 0) { close(fresh); g_last_error = 6; return 0; }
    close(old);
    g_pipe_info[id].connected = 0;
    return 1;
}

/* CreateFile("\\.\pipe\name"): connects a client; INVALID_HANDLE_VALUE64 with the Win32 error otherwise. */
static u64 beer_pipe_open_client(const char *name)
{
    char key[128], path[320];
    if (!beer_pipe_name(name, key, sizeof(key))) { g_last_error = 123; return INVALID_HANDLE_VALUE64; }
    snprintf(path, sizeof(path), "%s/pipe-%s.sock", beer_runtime_dir(), key);
    int fd = beer_pipe_connect_path(path);
    if (fd < 0) {
        g_last_error = (errno == ENOENT || errno == ECONNREFUSED) ? 2 : 231;   /* FILE_NOT_FOUND / PIPE_BUSY */
        return INVALID_HANDLE_VALUE64;
    }
    u64 handle = fh_alloc(fd, name, path);
    if (handle == INVALID_HANDLE_VALUE64) { close(fd); return handle; }
    BeerPipeInfo *info = &g_pipe_info[(u32)(handle - 0xF0000000ULL)];
    memset(info, 0, sizeof(*info));
    info->is_pipe = 1; info->connected = 1;
    signal(SIGPIPE, SIG_IGN);
    g_last_error = 0;
    return handle;
}

static u64 __attribute__((ms_abi)) beer_WaitNamedPipeA(const char *name, u32 timeout)
{
    char key[128], path[320];
    if (!beer_pipe_name(name, key, sizeof(key))) { g_last_error = 123; return 0; }
    snprintf(path, sizeof(path), "%s/pipe-%s.sock", beer_runtime_dir(), key);
    u32 waited = 0;
    for (;;) {
        if (access(path, F_OK) == 0) return 1;
        if (timeout == 0 || (timeout != 0xFFFFFFFFu && waited >= timeout)) { g_last_error = 121; return 0; }  /* ERROR_SEM_TIMEOUT */
        struct timespec delay = {0, 10000000L};
        nanosleep(&delay, NULL);
        waited += 10;
    }
}
static u64 __attribute__((ms_abi)) beer_WaitNamedPipeW(const u16 *name, u32 timeout)
{
    char narrow[512];
    beer_proc_utf16_to_utf8(name, narrow, sizeof(narrow));
    return beer_WaitNamedPipeA(narrow, timeout);
}

static u64 __attribute__((ms_abi))
beer_PeekNamedPipe(u64 handle, void *buffer, u32 size, u32 *read, u32 *available, u32 *left)
{
    if (!beer_pipe_is_pipe(handle)) { g_last_error = 6; return 0; }
    if (!beer_pipe_ensure_connected(handle)) return 0;
    int fd = fh_get(handle);
    if (fd < 0) { g_last_error = 6; return 0; }
    int pending = 0;
    ioctl(fd, FIONREAD, &pending);
    if (buffer && size) {
        ssize_t n = recv(fd, buffer, size, MSG_PEEK | MSG_DONTWAIT);
        if (read) *read = n > 0 ? (u32)n : 0;
    } else if (read) {
        *read = 0;
    }
    if (available) *available = pending > 0 ? (u32)pending : 0;
    if (left) *left = 0;
    return 1;
}

static u64 __attribute__((ms_abi))
beer_SetNamedPipeHandleState(u64 handle, u32 *mode, u32 *max_collection, u32 *timeout)
{
    (void)max_collection; (void)timeout;
    if (!beer_pipe_is_pipe(handle)) { g_last_error = 6; return 0; }
    if (mode) g_pipe_info[(u32)(handle - 0xF0000000ULL)].nowait = (*mode & 1u) ? 1 : 0;
    return 1;
}

/* ---- Inspecting other guest processes (psapi / kernel32 K32* family) ---- */
typedef struct { char name[260]; char path[600]; u64 base; } BeerProcInfo;

static int beer_proc_info(pid_t pid, BeerProcInfo *out)
{
    memset(out, 0, sizeof(*out));
    if (pid == getpid()) {
        snprintf(out->name, sizeof(out->name), "%s", beer_path_basename(g_exe_host_path ? g_exe_host_path : ""));
        snprintf(out->path, sizeof(out->path), "%s", g_exe_path ? g_exe_path : "");
        out->base = (u64)(uintptr_t)g_img;
        return 1;
    }
    if (!beer_pid_is_loader(pid)) return 0;
    char file[320], line[700];
    snprintf(file, sizeof(file), "%s/%d.proc", beer_runtime_dir(), (int)pid);
    FILE *f = fopen(file, "r");
    if (!f) return 0;
    int ok = 0;
    if (fgets(line, sizeof(line), f)) {                         /* parent pid */
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(out->name, sizeof(out->name), "%s", line);
            if (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\r\n")] = 0;
                snprintf(out->path, sizeof(out->path), "%s", line);
                if (fgets(line, sizeof(line), f)) out->base = strtoull(line, NULL, 16);
                ok = 1;
            }
        }
    }
    fclose(f);
    return ok;
}

static pid_t beer_handle_pid(u64 h)
{
    BeerProc *p = beer_proc_from_handle(h);
    if (p) return p->pid;
    if (h == 0x7FF0ull || h == (u64)-1) return getpid();
    return -1;
}

static int beer_handle_info(u64 h, BeerProcInfo *info)
{
    pid_t pid = beer_handle_pid(h);
    if (pid < 0) { g_last_error = 6; return 0; }
    if (!beer_proc_info(pid, info)) { g_last_error = 299; return 0; }     /* ERROR_PARTIAL_COPY: not a guest process */
    return 1;
}

static u64 beer_enum_process_modules(u64 process, u64 *modules, u32 cb, u32 *needed)
{
    BeerProcInfo info;
    if (!beer_handle_info(process, &info)) return 0;
    if (needed) *needed = 8;
    if (modules && cb >= 8) modules[0] = info.base;
    return 1;
}
static u64 __attribute__((ms_abi)) beer_K32EnumProcessModules(u64 process, u64 *modules, u32 cb, u32 *needed)
{ return beer_enum_process_modules(process, modules, cb, needed); }
static u64 __attribute__((ms_abi)) beer_K32EnumProcessModulesEx(u64 process, u64 *modules, u32 cb, u32 *needed, u32 filter)
{ (void)filter; return beer_enum_process_modules(process, modules, cb, needed); }

static u32 beer_copy_text_out(const char *text, void *out, u32 size, int wide)
{
    u32 n = (u32)strlen(text);
    if (!out || !size) return 0;
    if (n >= size) n = size - 1;
    for (u32 i = 0; i < n; ++i) {
        if (wide) ((u16 *)out)[i] = (u8)text[i];
        else ((char *)out)[i] = text[i];
    }
    if (wide) ((u16 *)out)[n] = 0; else ((char *)out)[n] = 0;
    return n;
}

static u64 beer_module_file_name_ex(u64 process, u64 module, void *out, u32 size, int wide, int base_name)
{
    BeerProcInfo info;
    if (!beer_handle_info(process, &info)) return 0;
    (void)module;                                   /* only the main module of a guest process is known */
    return beer_copy_text_out(base_name ? info.name : info.path, out, size, wide);
}
static u64 __attribute__((ms_abi)) beer_K32GetModuleFileNameExA(u64 p, u64 m, char *b, u32 n) { return beer_module_file_name_ex(p, m, b, n, 0, 0); }
static u64 __attribute__((ms_abi)) beer_K32GetModuleFileNameExW(u64 p, u64 m, u16 *b, u32 n)  { return beer_module_file_name_ex(p, m, b, n, 1, 0); }
static u64 __attribute__((ms_abi)) beer_K32GetModuleBaseNameA(u64 p, u64 m, char *b, u32 n)   { return beer_module_file_name_ex(p, m, b, n, 0, 1); }
static u64 __attribute__((ms_abi)) beer_K32GetModuleBaseNameW(u64 p, u64 m, u16 *b, u32 n)    { return beer_module_file_name_ex(p, m, b, n, 1, 1); }

static u64 beer_query_full_process_image_name(u64 process, void *out, u32 *size, int wide)
{
    BeerProcInfo info;
    if (!size || !beer_handle_info(process, &info)) return 0;
    u32 need = (u32)strlen(info.path);
    if (!out || *size <= need) { *size = need + 1; g_last_error = 122; return 0; }   /* ERROR_INSUFFICIENT_BUFFER */
    *size = beer_copy_text_out(info.path, out, *size, wide);
    return 1;
}
static u64 __attribute__((ms_abi)) beer_QueryFullProcessImageNameA(u64 p, u32 flags, char *b, u32 *n)
{ (void)flags; return beer_query_full_process_image_name(p, b, n, 0); }
static u64 __attribute__((ms_abi)) beer_QueryFullProcessImageNameW(u64 p, u32 flags, u16 *b, u32 *n)
{ (void)flags; return beer_query_full_process_image_name(p, b, n, 1); }
static u64 __attribute__((ms_abi)) beer_K32GetProcessImageFileNameA(u64 p, char *b, u32 n)
{ BeerProcInfo i; return beer_handle_info(p, &i) ? beer_copy_text_out(i.path, b, n, 0) : 0; }
static u64 __attribute__((ms_abi)) beer_K32GetProcessImageFileNameW(u64 p, u16 *b, u32 n)
{ BeerProcInfo i; return beer_handle_info(p, &i) ? beer_copy_text_out(i.path, b, n, 1) : 0; }
