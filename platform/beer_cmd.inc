/* Beer's cmd.exe: a command interpreter run in a child loader (CreateProcess of cmd.exe starts the loader with
 * --beer-cmd=<command line>) instead of a Windows binary. It covers what installers and redistributable scripts
 * use: /c and /k, command chains (&, &&, ||), redirection (>, >>, 2>, 2>&1, <, nul), %VAR% expansion, IF [NOT]
 * EXIST / ERRORLEVEL / string compare, the file built-ins (DEL, RD, MD, COPY, MOVE, REN, CD, TYPE), ECHO, SET,
 * EXIT, START, CALL, batch files, TIMEOUT and the "ping -n N 127.0.0.1" delay. Other programs run through Beer as
 * child loaders and are waited for; the exit code of the last command is the process exit code.
 * Contract: Microsoft Learn "cmd" and "Windows commands" reference pages. */

static int g_cmd_errorlevel;
static int g_cmd_exit_requested;

static int cmd_ieq(const char *a, const char *b) { return !strcasecmp(a, b); }

/* A Windows path (absolute, drive-relative or relative to the current directory) as a host path. */
static int cmd_host_path(const char *windows, char *host, size_t size)
{
    if (!windows || !*windows) return 0;
    if (cmd_ieq(windows, "nul")) { snprintf(host, size, "/dev/null"); return 1; }
    return resolve_windows_host_path(windows, host, size);
}

static void cmd_strip_quotes(char *s)
{
    char *w = s;
    for (char *r = s; *r; ++r) if (*r != '"') *w++ = *r;
    *w = 0;
}

/* %VAR% expansion; unknown variables expand to nothing, %% to %. Windows-only variables get prefix defaults. */
static const char *cmd_getenv(const char *name, char *buf, size_t size)
{
    if (cmd_ieq(name, "errorlevel")) { snprintf(buf, size, "%d", g_cmd_errorlevel); return buf; }
    if (cmd_ieq(name, "cd")) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return "";
        snprintf(buf, size, "Z:%s", cwd);
        for (char *p = buf; *p; ++p) if (*p == '/') *p = '\\';
        return buf;
    }
    char upper[128];
    size_t n = 0;
    for (; name[n] && n + 1 < sizeof(upper); ++n) upper[n] = (char)toupper((unsigned char)name[n]);
    upper[n] = 0;
    const char *v = getenv(upper);
    if (!v) v = getenv(name);
    if (v) return v;
    if (!strcmp(upper, "SYSTEMROOT") || !strcmp(upper, "WINDIR")) return "C:\\Windows";
    if (!strcmp(upper, "SYSTEMDRIVE")) return "C:";
    if (!strcmp(upper, "COMSPEC")) return "C:\\Windows\\System32\\cmd.exe";
    if (!strcmp(upper, "TEMP") || !strcmp(upper, "TMP")) return "C:\\Windows\\Temp";
    if (!strcmp(upper, "PROGRAMFILES")) return "C:\\Program Files";
    if (!strcmp(upper, "PROGRAMFILES(X86)")) return "C:\\Program Files (x86)";
    if (!strcmp(upper, "USERPROFILE")) return "C:\\Users\\Player";
    if (!strcmp(upper, "APPDATA")) return "C:\\Users\\Player\\AppData\\Roaming";
    if (!strcmp(upper, "LOCALAPPDATA")) return "C:\\Users\\Player\\AppData\\Local";
    return NULL;
}

static void cmd_expand(const char *in, char *out, size_t size)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < size;) {
        if (*p == '%') {
            if (p[1] == '%') { out[o++] = '%'; p += 2; continue; }
            const char *end = strchr(p + 1, '%');
            if (end && end > p + 1 && end - p < 128) {
                char name[128], buf[PATH_MAX];
                memcpy(name, p + 1, (size_t)(end - p - 1));
                name[end - p - 1] = 0;
                const char *v = cmd_getenv(name, buf, sizeof(buf));
                if (v) {
                    for (; *v && o + 1 < size; ++v) out[o++] = *v;
                    p = end + 1;
                    continue;
                }
                if (!strchr(name, ' ')) { p = end + 1; continue; }   /* an undefined variable expands to nothing */
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

/* Splits one simple command into words (quotes kept off, spaces inside quotes kept). Returns the word count. */
static int cmd_words(const char *s, char words[][PATH_MAX], int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') ++s;
        if (!*s) break;
        size_t k = 0;
        int quoted = 0;
        while (*s && (quoted || (*s != ' ' && *s != '\t'))) {
            if (*s == '"') { quoted = !quoted; ++s; continue; }
            if (k + 1 < PATH_MAX) words[n][k++] = *s;
            ++s;
        }
        words[n][k] = 0;
        ++n;
    }
    return n;
}

/* Files matching a pattern (wildcards allowed in the last component, matched case-insensitively). */
typedef void (*CmdFileFn)(const char *host, const char *name, void *ctx);
static int cmd_for_each_match(const char *pattern, int dirs_too, CmdFileFn fn, void *ctx)
{
    char host[PATH_MAX];
    if (!strpbrk(pattern, "*?")) {
        struct stat st;
        if (!cmd_host_path(pattern, host, sizeof(host)) || stat(host, &st) != 0) return 0;
        if (S_ISDIR(st.st_mode) && !dirs_too) return 0;
        const char *base = strrchr(host, '/');
        fn(host, base ? base + 1 : host, ctx);
        return 1;
    }
    char dir_win[PATH_MAX];
    snprintf(dir_win, sizeof(dir_win), "%s", pattern);
    char *slash = strrchr(dir_win, '\\');
    if (!slash) slash = strrchr(dir_win, '/');
    const char *mask = pattern;
    char dir_host[PATH_MAX];
    if (slash) {
        mask = pattern + (slash - dir_win) + 1;
        slash[1] = 0;
        if (!cmd_host_path(dir_win, dir_host, sizeof(dir_host))) return 0;
    } else snprintf(dir_host, sizeof(dir_host), ".");
    DIR *d = opendir(dir_host);
    if (!d) return 0;
    char m[260];
    snprintf(m, sizeof(m), "%s", mask);
    if (!strcmp(m, "*.*")) snprintf(m, sizeof(m), "*");
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (fnmatch(m, e->d_name, FNM_CASEFOLD)) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir_host, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || (S_ISDIR(st.st_mode) && !dirs_too)) continue;
        fn(path, e->d_name, ctx);
        ++count;
    }
    closedir(d);
    return count;
}

static int cmd_remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char child[PATH_MAX];
                snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
                cmd_remove_tree(child);
            }
            closedir(d);
        }
        return rmdir(path);
    }
    return unlink(path);
}

static int cmd_copy_file(const char *from, const char *to, int append)
{
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0666);
    if (out < 0) { close(in); return -1; }
    char buf[1 << 16];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)n) != n) { rc = -1; break; }
    if (n < 0) rc = -1;
    close(in);
    close(out);
    return rc;
}

static void cmd_del_one(const char *host, const char *name, void *ctx)
{
    (void)name;
    struct stat st;
    if (stat(host, &st) == 0 && S_ISDIR(st.st_mode)) {           /* DEL dir deletes the files inside it */
        DIR *d = opendir(host);
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d))) {
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", host, e->d_name);
            if (stat(child, &st) == 0 && S_ISREG(st.st_mode)) unlink(child);
        }
        closedir(d);
        return;
    }
    if (unlink(host) == 0) ++*(int *)ctx;
}

typedef struct { char dest[PATH_MAX]; int dest_is_dir, move, append, failed, done; } CmdCopyCtx;
static void cmd_copy_one(const char *host, const char *name, void *opaque)
{
    CmdCopyCtx *c = opaque;
    char target[PATH_MAX];
    if (c->dest_is_dir) snprintf(target, sizeof(target), "%s/%s", c->dest, name);
    else snprintf(target, sizeof(target), "%s", c->dest);
    int rc;
    if (c->move) {
        rc = rename(host, target);
        if (rc != 0 && errno == EXDEV) { rc = cmd_copy_file(host, target, 0); if (!rc) unlink(host); }
    } else {
        rc = cmd_copy_file(host, target, c->append);
        if (!c->dest_is_dir) c->append = 1;                     /* COPY a+b dest concatenates */
    }
    if (rc) ++c->failed; else ++c->done;
}

/* Finds a program: as given, with .exe/.com/.bat/.cmd, then in C:\Windows\System32 and C:\Windows. */
static int cmd_find_program(const char *name, char *host, size_t size)
{
    static const char *const exts[] = { "", ".exe", ".com", ".bat", ".cmd" };
    static const char *const dirs[] = { "", "C:\\Windows\\System32\\", "C:\\Windows\\" };
    const int has_dir = strchr(name, '\\') || strchr(name, '/') || strchr(name, ':');
    for (int d = 0; d < (has_dir ? 1 : 3); ++d)
        for (int e = 0; e < 5; ++e) {
            char candidate[PATH_MAX];
            snprintf(candidate, sizeof(candidate), "%s%s%s", dirs[d], name, exts[e]);
            struct stat st;
            if (cmd_host_path(candidate, host, size) && stat(host, &st) == 0 && S_ISREG(st.st_mode)) return 1;
        }
    return 0;
}

static int cmd_run_line(const char *line);
static int cmd_run_batch(const char *host, int argc, char args[][PATH_MAX]);

/* Runs a Windows program in a child loader (or a batch file in this interpreter); waits unless `nowait`. */
static int cmd_run_program(const char *raw_command, int argc, char words[][PATH_MAX], int nowait)
{
    char host[PATH_MAX];
    if (!cmd_find_program(words[0], host, sizeof(host))) {
        fprintf(stderr, "'%s' is not recognized as an internal or external command,\noperable program or batch file.\n",
                words[0]);
        return 9009;
    }
    const char *dot = strrchr(host, '.');
    if (dot && (cmd_ieq(dot, ".bat") || cmd_ieq(dot, ".cmd"))) return cmd_run_batch(host, argc, words);
    {
        unsigned char magic[2] = {0, 0};
        FILE *f = fopen(host, "rb");
        if (f) { if (fread(magic, 1, 2, f) != 2) magic[0] = 0; fclose(f); }
        if (magic[0] != 'M' || magic[1] != 'Z') { fprintf(stderr, "Access is denied.\n"); return 1; }
    }
    char self[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) return 1;
    self[n] = 0;
    char *argv[64];
    int k = 0;
    argv[k++] = self;
    for (int i = 1; i < g_loader_argc && k < 56; ++i) {
        const char *a = g_loader_argv[i];
        if (strncmp(a, "--", 2)) break;
        if (strncmp(a, "--cmdline=", 10) && strncmp(a, "--beer-cmd=", 11)) argv[k++] = g_loader_argv[i];
    }
    char cmdline_option[8192 + 16];
    snprintf(cmdline_option, sizeof(cmdline_option), "--cmdline=%s", raw_command);
    argv[k++] = cmdline_option;
    argv[k++] = host;
    argv[k] = NULL;
    pid_t pid;
    extern char **environ;
    if (posix_spawn(&pid, self, NULL, NULL, argv, environ) != 0) return 1;
    fprintf(stderr, "[CMD] started pid %d: %s\n", (int)pid, raw_command);
    if (nowait) return 0;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int cmd_builtin(const char *raw, int argc, char w[][PATH_MAX])
{
    const char *verb = w[0];
    /* The text after the verb, for ECHO / SET / IF bodies. */
    const char *rest = raw;
    while (*rest == ' ' || *rest == '\t' || *rest == '@') ++rest;
    rest += strcspn(rest, " \t");
    if (*rest) ++rest;

    if (cmd_ieq(verb, "rem") || cmd_ieq(verb, "::") || cmd_ieq(verb, "cls") || cmd_ieq(verb, "pause") ||
        cmd_ieq(verb, "title") || cmd_ieq(verb, "color") || cmd_ieq(verb, "chcp") || cmd_ieq(verb, "setlocal") ||
        cmd_ieq(verb, "endlocal") || cmd_ieq(verb, "attrib") || cmd_ieq(verb, "mode") || cmd_ieq(verb, "verify"))
        return 0;
    if (!strncasecmp(verb, "echo", 4) && (verb[4] == 0 || verb[4] == '.')) {
        if (verb[4] == '.') { printf("%s%s%s\n", verb + 5, *rest ? " " : "", rest); fflush(stdout); return 0; }
        if (cmd_ieq(rest, "off") || cmd_ieq(rest, "on")) return 0;
        printf("%s\n", *rest ? rest : "ECHO is on.");
        fflush(stdout);
        return 0;
    }
    if (cmd_ieq(verb, "exit")) {
        int i = 1;
        if (argc > i && cmd_ieq(w[i], "/b")) ++i;
        g_cmd_exit_requested = 1;
        return argc > i ? atoi(w[i]) : g_cmd_errorlevel;
    }
    if (cmd_ieq(verb, "set")) {
        char text[PATH_MAX];
        snprintf(text, sizeof(text), "%s", rest);
        if (!strncasecmp(text, "/a ", 3) || !strncasecmp(text, "/p ", 3)) return 0;
        cmd_strip_quotes(text);
        char *eq = strchr(text, '=');
        if (!eq) return 0;
        *eq = 0;
        if (eq[1]) setenv(text, eq + 1, 1); else unsetenv(text);
        return 0;
    }
    if (cmd_ieq(verb, "cd") || cmd_ieq(verb, "chdir") || cmd_ieq(verb, "pushd") || cmd_ieq(verb, "popd")) {
        if (cmd_ieq(verb, "popd")) return 0;
        int i = 1;
        if (argc > i && cmd_ieq(w[i], "/d")) ++i;
        if (argc <= i) { char b[PATH_MAX]; printf("%s\n", cmd_getenv("cd", b, sizeof(b))); return 0; }
        char host[PATH_MAX];
        if (!cmd_host_path(w[i], host, sizeof(host)) || chdir(host) != 0) {
            fprintf(stderr, "The system cannot find the path specified.\n");
            return 1;
        }
        return 0;
    }
    if (cmd_ieq(verb, "md") || cmd_ieq(verb, "mkdir")) {
        int rc = 0;
        for (int i = 1; i < argc; ++i) {
            char host[PATH_MAX];
            if (!cmd_host_path(w[i], host, sizeof(host))) { rc = 1; continue; }
            for (char *p = host + 1; *p; ++p)                    /* MD creates intermediate directories */
                if (*p == '/') { *p = 0; mkdir(host, 0777); *p = '/'; }
            if (mkdir(host, 0777) != 0) { fprintf(stderr, "A subdirectory or file %s already exists.\n", w[i]); rc = 1; }
        }
        return rc;
    }
    if (cmd_ieq(verb, "rd") || cmd_ieq(verb, "rmdir")) {
        int tree = 0, rc = 0;
        for (int i = 1; i < argc; ++i) {
            if (cmd_ieq(w[i], "/s")) { tree = 1; continue; }
            if (w[i][0] == '/') continue;
            char host[PATH_MAX];
            if (!cmd_host_path(w[i], host, sizeof(host)) || (tree ? cmd_remove_tree(host) : rmdir(host)) != 0) {
                fprintf(stderr, "The system cannot find the file specified.\n");
                rc = 1;
            }
        }
        return rc;
    }
    if (cmd_ieq(verb, "del") || cmd_ieq(verb, "erase")) {
        int any = 0;
        for (int i = 1; i < argc; ++i) {
            if (w[i][0] == '/') continue;
            int deleted = 0;
            if (!cmd_for_each_match(w[i], 1, cmd_del_one, &deleted)) fprintf(stderr, "Could Not Find %s\n", w[i]);
            any = 1;
        }
        return any ? 0 : 1;
    }
    if (cmd_ieq(verb, "copy") || cmd_ieq(verb, "move") || cmd_ieq(verb, "xcopy") || cmd_ieq(verb, "robocopy")) {
        if (cmd_ieq(verb, "robocopy")) return 1;               /* not built in */
        char *paths[16];
        int np = 0;
        for (int i = 1; i < argc && np < 16; ++i) if (w[i][0] != '/') paths[np++] = w[i];
        if (np < 1) { fprintf(stderr, "The syntax of the command is incorrect.\n"); return 1; }
        CmdCopyCtx c;
        memset(&c, 0, sizeof(c));
        c.move = cmd_ieq(verb, "move");
        if (np < 2) snprintf(c.dest, sizeof(c.dest), ".");
        else if (!cmd_host_path(paths[np - 1], c.dest, sizeof(c.dest))) return 1;
        struct stat st;
        c.dest_is_dir = stat(c.dest, &st) == 0 && S_ISDIR(st.st_mode);
        const int sources = np < 2 ? 1 : np - 1;
        for (int s = 0; s < sources; ++s) {
            char list[PATH_MAX];                                  /* a+b+c */
            snprintf(list, sizeof(list), "%s", paths[s]);
            for (char *part = strtok(list, "+"); part; part = strtok(NULL, "+")) {
                if (c.move) {                                     /* MOVE of a directory renames it */
                    char host[PATH_MAX];
                    if (!strpbrk(part, "*?") && cmd_host_path(part, host, sizeof(host)) && stat(host, &st) == 0 &&
                        S_ISDIR(st.st_mode)) {
                        const char *base = strrchr(host, '/');
                        cmd_copy_one(host, base ? base + 1 : host, &c);
                        continue;
                    }
                }
                if (!cmd_for_each_match(part, 0, cmd_copy_one, &c)) ++c.failed;
            }
        }
        if (!cmd_ieq(verb, "xcopy")) printf("%8d file(s) %s.\n", c.done, c.move ? "moved" : "copied");
        fflush(stdout);
        return c.failed ? 1 : 0;
    }
    if (cmd_ieq(verb, "ren") || cmd_ieq(verb, "rename")) {
        if (argc < 3) return 1;
        char from[PATH_MAX], to[PATH_MAX];
        if (!cmd_host_path(w[1], from, sizeof(from))) return 1;
        snprintf(to, sizeof(to), "%s", from);
        char *slash = strrchr(to, '/');
        snprintf(slash ? slash + 1 : to, sizeof(to) - (size_t)(slash ? slash + 1 - to : 0), "%s", w[2]);
        return rename(from, to) == 0 ? 0 : 1;
    }
    if (cmd_ieq(verb, "type")) {
        int rc = 0;
        for (int i = 1; i < argc; ++i) {
            char host[PATH_MAX];
            int fd = cmd_host_path(w[i], host, sizeof(host)) ? open(host, O_RDONLY | O_CLOEXEC) : -1;
            if (fd < 0) { rc = 1; continue; }
            char buf[1 << 14];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0) if (write(1, buf, (size_t)n) < 0) break;
            close(fd);
        }
        return rc;
    }
    if (cmd_ieq(verb, "timeout")) {
        for (int i = 1; i + 1 < argc; ++i)
            if (cmd_ieq(w[i], "/t")) { const int s = atoi(w[i + 1]); if (s > 0) sleep((unsigned)s); }
        return 0;
    }
    if (cmd_ieq(verb, "ping") || cmd_ieq(verb, "ping.exe")) {   /* "ping -n N 127.0.0.1": an N-1 second delay */
        int count = 4;
        for (int i = 1; i + 1 < argc; ++i) if (cmd_ieq(w[i], "-n") || cmd_ieq(w[i], "/n")) count = atoi(w[i + 1]);
        if (count > 1) sleep((unsigned)(count - 1));
        return 0;
    }
    if (cmd_ieq(verb, "ver")) { printf("\nMicrosoft Windows [Version 10.0.19045]\n"); return 0; }
    if (cmd_ieq(verb, "start")) {
        int i = 1, wait = 0;
        if (argc > i && raw[0]) {
            /* START "title" ...: the first quoted argument is the window title. */
            const char *after = rest;
            while (*after == ' ') ++after;
            if (*after == '"') ++i;
        }
        for (; i < argc && w[i][0] == '/'; ++i) {
            if (cmd_ieq(w[i], "/wait")) wait = 1;
            else if (cmd_ieq(w[i], "/d") && i + 1 < argc) { char h[PATH_MAX]; if (cmd_host_path(w[++i], h, sizeof(h))) { if (chdir(h)) {} } }
        }
        if (i >= argc) return 0;
        char command[8192] = "";
        size_t used = 0;
        for (int k = i; k < argc && used + 4 < sizeof(command); ++k)
            used += (size_t)snprintf(command + used, sizeof(command) - used, strchr(w[k], ' ') ? "%s\"%s\"" : "%s%s",
                                     k > i ? " " : "", w[k]);
        return cmd_run_program(command, argc - i, w + i, !wait);
    }
    if (cmd_ieq(verb, "call")) return argc > 1 ? cmd_run_line(rest) : 0;
    return -1;                                                   /* not a built-in */
}

/* IF [/I] [NOT] EXIST x cmd | ERRORLEVEL n cmd | a==b cmd. Returns -1 when `line` is not an IF. */
static int cmd_run_if(const char *line)
{
    const char *p = line;
    while (*p == ' ') ++p;
    if (strncasecmp(p, "if ", 3)) return -1;
    p += 3;
    int negate = 0, nocase = 0;
    for (;;) {
        while (*p == ' ') ++p;
        if (!strncasecmp(p, "/i ", 3)) { nocase = 1; p += 3; continue; }
        if (!strncasecmp(p, "not ", 4)) { negate = !negate; p += 4; continue; }
        break;
    }
    char a[PATH_MAX];
    const char *body;
    int truth;
    if (!strncasecmp(p, "exist ", 6) || !strncasecmp(p, "errorlevel ", 11)) {
        const int exist = !strncasecmp(p, "exist ", 6);
        p += exist ? 6 : 11;
        while (*p == ' ') ++p;
        size_t k = 0;
        int quoted = 0;
        while (*p && (quoted || *p != ' ')) { if (*p == '"') quoted = !quoted; else if (k + 1 < sizeof(a)) a[k++] = *p; ++p; }
        a[k] = 0;
        body = p;
        if (exist) {
            char host[PATH_MAX];
            struct stat st;
            if (!strpbrk(a, "*?")) truth = cmd_host_path(a, host, sizeof(host)) && stat(host, &st) == 0;
            else {
                /* a wildcard exists when anything matches */
                char dir_win[PATH_MAX];
                snprintf(dir_win, sizeof(dir_win), "%s", a);
                truth = 0;
                char *slash = strrchr(dir_win, '\\');
                const char *mask = slash ? slash + 1 : dir_win;
                char dir_host[PATH_MAX];
                if (slash) { *slash = 0; if (!cmd_host_path(dir_win, dir_host, sizeof(dir_host))) dir_host[0] = 0; }
                else snprintf(dir_host, sizeof(dir_host), ".");
                DIR *d = dir_host[0] ? opendir(dir_host) : NULL;
                struct dirent *e;
                while (d && (e = readdir(d))) if (!fnmatch(mask, e->d_name, FNM_CASEFOLD)) { truth = 1; break; }
                if (d) closedir(d);
            }
        } else truth = g_cmd_errorlevel >= atoi(a);
    } else {
        const char *eq = strstr(p, "==");
        if (!eq) return 1;
        char b[PATH_MAX];
        snprintf(a, sizeof(a), "%.*s", (int)(eq - p), p);
        const char *q = eq + 2;
        while (*q == ' ') ++q;
        size_t k = 0;
        int quoted = 0;
        while (*q && (quoted || *q != ' ')) { if (*q == '"') quoted = !quoted; if (k + 1 < sizeof(b)) b[k++] = *q; ++q; }
        b[k] = 0;
        body = q;
        char *end = a + strlen(a);
        while (end > a && end[-1] == ' ') *--end = 0;
        truth = nocase ? !strcasecmp(a, b) : !strcmp(a, b);
    }
    if (negate) truth = !truth;
    while (*body == ' ') ++body;
    char text[8192];
    snprintf(text, sizeof(text), "%s", body);
    if (text[0] == '(') {                                        /* IF ... (cmd) ELSE (cmd) on one line */
        char *close_paren = strrchr(text, ')');
        char *else_part = NULL;
        char *mid = strstr(text, ") else (");
        if (!mid) mid = strstr(text, ") ELSE (");
        if (mid) { *mid = 0; else_part = mid + 8; char *e = strrchr(else_part, ')'); if (e) *e = 0; }
        else if (close_paren) *close_paren = 0;
        if (truth) return cmd_run_line(text + 1);
        return else_part ? cmd_run_line(else_part) : 0;
    }
    char *else_word = strstr(text, " else ");
    if (!else_word) else_word = strstr(text, " ELSE ");
    if (else_word) { *else_word = 0; if (!truth) return cmd_run_line(else_word + 6); }
    return truth ? cmd_run_line(text) : 0;
}

/* One simple command with its redirections. */
static int cmd_run_simple(const char *input)
{
    char line[8192];
    size_t o = 0;
    int saved[3] = { -1, -1, -1 };
    int quoted = 0;
    /* Pull redirections out of the command text. */
    for (const char *p = input; *p && o + 1 < sizeof(line);) {
        if (*p == '"') quoted = !quoted;
        int fd = -1;
        if (!quoted && (*p == '>' || *p == '<')) fd = *p == '<' ? 0 : 1;
        if (!quoted && (p[0] == '1' || p[0] == '2') && p[1] == '>' && (o == 0 || line[o - 1] == ' ')) { fd = p[0] - '0'; ++p; }
        if (fd < 0) { line[o++] = *p++; continue; }
        const int input_redirect = *p == '<';
        int append = 0;
        ++p;
        if (*p == '>') { append = 1; ++p; }
        if (*p == '&' && (p[1] == '1' || p[1] == '2')) {         /* 2>&1 */
            const int target = p[1] - '0';
            p += 2;
            if (saved[fd] < 0) saved[fd] = dup(fd);
            dup2(target, fd);
            continue;
        }
        while (*p == ' ') ++p;
        char name[PATH_MAX];
        size_t k = 0;
        int q = 0;
        while (*p && (q || (*p != ' ' && *p != '>' && *p != '<' && *p != '&' && *p != '|'))) {
            if (*p == '"') q = !q; else if (k + 1 < sizeof(name)) name[k++] = *p;
            ++p;
        }
        name[k] = 0;
        char host[PATH_MAX];
        int file = -1;
        if (cmd_host_path(name, host, sizeof(host)))
            file = input_redirect ? open(host, O_RDONLY | O_CLOEXEC)
                                  : open(host, O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0666);
        if (file >= 0) {
            if (saved[fd] < 0) saved[fd] = dup(fd);
            dup2(file, fd);
            close(file);
        }
    }
    line[o] = 0;
    int rc = cmd_run_if(line);
    if (rc < 0) {
        static char words[64][PATH_MAX];
        const char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '@') ++p;
        const int argc = cmd_words(p, words, 64);
        if (!argc) rc = g_cmd_errorlevel;
        else if (words[0][0] == ':') rc = 0;                      /* a label */
        else {
            rc = cmd_builtin(p, argc, words);
            if (rc < 0) rc = cmd_run_program(p, argc, words, 0);
        }
    }
    fflush(stdout);
    for (int fd = 0; fd < 3; ++fd) if (saved[fd] >= 0) { dup2(saved[fd], fd); close(saved[fd]); }
    return rc;
}

/* A line: simple commands joined by &, &&, || (and | run left to right without a pipe). */
static int cmd_run_line(const char *raw)
{
    char line[8192];
    cmd_expand(raw, line, sizeof(line));
    int rc = g_cmd_errorlevel;
    char op = '&';
    const char *p = line;
    while (*p && !g_cmd_exit_requested) {
        const char *start = p;
        int quoted = 0, depth = 0;
        while (*p) {
            if (*p == '"') quoted = !quoted;
            else if (!quoted && *p == '(') ++depth;
            else if (!quoted && *p == ')' && depth) --depth;
            else if (!quoted && !depth && (*p == '&' || *p == '|') && !(p > start && p[-1] == '>')) break;
            ++p;
        }
        char part[8192];
        snprintf(part, sizeof(part), "%.*s", (int)(p - start), start);
        char *s = part;
        while (*s == ' ') ++s;
        if (*s == '(') { ++s; char *e = strrchr(s, ')'); if (e) *e = 0; }
        const int run = op == '&' || (op == 'A' && rc == 0) || (op == 'O' && rc != 0) || op == '|';
        if (run && *s) { rc = cmd_run_simple(s); g_cmd_errorlevel = rc; }
        if (!*p) break;
        if (p[0] == '&' && p[1] == '&') { op = 'A'; p += 2; }
        else if (p[0] == '|' && p[1] == '|') { op = 'O'; p += 2; }
        else { op = *p == '|' ? '|' : '&'; ++p; }
    }
    return rc;
}

/* A batch file: lines in order, GOTO label / :EOF, %0..%9 arguments. Parenthesised blocks over several lines are
 * joined into one line. */
static int cmd_run_batch(const char *host, int argc, char args[][PATH_MAX])
{
    FILE *f = fopen(host, "rb");
    if (!f) return 1;
    char *lines[4096];
    int count = 0;
    char buf[8192], block[8192];
    int depth = 0;
    block[0] = 0;
    while (count < 4096 && fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        for (const char *c = buf; *c; ++c) { if (*c == '(') ++depth; else if (*c == ')' && depth) --depth; }
        if (block[0]) strncat(block, " & ", sizeof(block) - strlen(block) - 1);
        strncat(block, buf, sizeof(block) - strlen(block) - 1);
        if (depth) continue;
        lines[count++] = strdup(block);
        block[0] = 0;
    }
    fclose(f);
    int rc = 0;
    for (int i = 0; i < count && !g_cmd_exit_requested; ++i) {
        char text[8192];
        size_t o = 0;
        for (const char *p = lines[i]; *p && o + 1 < sizeof(text);) {       /* %0..%9 */
            if (p[0] == '%' && p[1] >= '0' && p[1] <= '9') {
                const int n = p[1] - '0';
                if (n < argc) for (const char *a = args[n]; *a && o + 1 < sizeof(text); ++a) text[o++] = *a;
                p += 2;
                continue;
            }
            if (p[0] == '%' && p[1] == '~' && p[2] == 'd' && p[3] == 'p' && p[4] == '0') {   /* %~dp0 */
                char dir[PATH_MAX];
                snprintf(dir, sizeof(dir), "Z:%s", host);
                for (char *c = dir; *c; ++c) if (*c == '/') *c = '\\';
                char *slash = strrchr(dir, '\\');
                if (slash) slash[1] = 0;
                for (const char *a = dir; *a && o + 1 < sizeof(text); ++a) text[o++] = *a;
                p += 5;
                continue;
            }
            text[o++] = *p++;
        }
        text[o] = 0;
        const char *t = text;
        while (*t == ' ' || *t == '@') ++t;
        if (!strncasecmp(t, "goto ", 5)) {
            const char *label = t + 5;
            while (*label == ' ' || *label == ':') ++label;
            if (cmd_ieq(label, "eof")) break;
            int found = -1;
            for (int k = 0; k < count && found < 0; ++k) {
                const char *l = lines[k];
                while (*l == ' ') ++l;
                if (*l == ':' && !strncasecmp(l + 1, label, strlen(label)) &&
                    (l[1 + strlen(label)] == 0 || l[1 + strlen(label)] == ' ')) found = k;
            }
            if (found < 0) { rc = 1; break; }
            i = found;
            continue;
        }
        rc = cmd_run_line(t);
        g_cmd_errorlevel = rc;
    }
    for (int i = 0; i < count; ++i) free(lines[i]);
    g_cmd_exit_requested = 0;
    return rc;
}

/* The process: `command_line` is cmd's full command line ("cmd.exe /c ..."). */
static int beer_cmd_main(const char *command_line)
{
    char cwd[PATH_MAX];
    fprintf(stderr, "[CMD] %s (in %s)\n", command_line, getcwd(cwd, sizeof(cwd)) ? cwd : "?");
    const char *p = command_line;
    while (*p == ' ') ++p;
    if (*p == '"') { p = strchr(p + 1, '"'); p = p ? p + 1 : command_line + strlen(command_line); }
    else p += strcspn(p, " \t");
    /* Switches up to /C or /K; the rest is the command. */
    const char *command = NULL;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '/') break;
        if ((p[1] == 'c' || p[1] == 'C' || p[1] == 'k' || p[1] == 'K') && (p[2] == ' ' || p[2] == '\t' || p[2] == '"' || !p[2])) {
            command = p + 2;
            break;
        }
        p += strcspn(p, " \t");
    }
    if (!command) return 0;                                       /* an interactive shell: nothing to read */
    while (*command == ' ' || *command == '\t') ++command;
    char text[8192];
    snprintf(text, sizeof(text), "%s", command);
    /* cmd /c "..." keeps the quotes only for exactly two quotes around an existing program name; otherwise the first
     * and the last quote are removed. */
    size_t len = strlen(text);
    if (text[0] == '"') {
        int quotes = 0;
        for (size_t i = 0; i < len; ++i) quotes += text[i] == '"';
        int keep = 0;
        if (quotes == 2 && text[len - 1] == '"' && !strpbrk(text, "&<>()@^|")) {
            char name[PATH_MAX], host[PATH_MAX];
            snprintf(name, sizeof(name), "%.*s", (int)(len - 2), text + 1);
            keep = strchr(name, ' ') && cmd_find_program(name, host, sizeof(host));
        }
        if (!keep) {
            char *last = strrchr(text, '"');
            if (last) memmove(last, last + 1, strlen(last));
            memmove(text, text + 1, strlen(text));
        }
    }
    const int rc = cmd_run_line(text);
    fflush(stdout);
    fprintf(stderr, "[CMD] exit %d\n", rc);
    return rc;
}
