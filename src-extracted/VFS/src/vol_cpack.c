/* vol_cpack.c — WP11 tool plumbing, codecpack exec hooks (WP13),
 * WP16a containerpack sweep + WP16b seekable-container map read path.
 * Split from volume.c (pure code motion). */

#include "volume_internal.h"


/*
/*
 * JXL helpers: transcode JPEG -> JXL (lossless) and back via subprocesses.
 * Returns 0 on success. Out buffers are malloc'd.
 */
#ifndef _WIN32

/* ---- WP11: POSIX twin of the Windows tool plumbing (CreateProcessW) ----
 *
 * One exec layer serves every external codec (cjxl/djxl, MAC, packMP3,
 * ffmpeg): fixed argv arrays (no shell), a fresh mkdtemp scratch dir per
 * transcode (a leftover output from an earlier run can never be misread as
 * this run's), and a hard timeout so a wedged helper cannot hang a sweep.
 * Scratch lives in /dev/shm (tmpfs) first -- the intermediate WAV/JPEG can
 * be tens of MB and should not wear the flash the volume itself lives on --
 * with /tmp as fallback. */

/* read a whole file into a fresh buffer; returns 0 on success */
static int slurp_file(const char *path, uint8_t **out, size_t *out_len);


/* Tool search order: $INVFS_TOOLS/<name> -> /usr/lib/invfs/tools/<name> ->
 * the bare name (execvp's PATH search). No other absolute paths. */
static const char *tool_resolve(const char *name, char *buf, size_t cap)
{
    const char *dir = getenv("INVFS_TOOLS");
    int n;

    if (dir && *dir) {
        n = snprintf(buf, cap, "%s/%s", dir, name);
        if (n > 0 && (size_t)n < cap && access(buf, X_OK) == 0)
            return buf;
    }
    n = snprintf(buf, cap, "/usr/lib/invfs/tools/%s", name);
    if (n > 0 && (size_t)n < cap && access(buf, X_OK) == 0)
        return buf;
    return name;
}


static uint64_t tool_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}


#define TOOL_TIMEOUT_MS (120ull * 1000ull)


/* WP12(d): every tool child runs under an RLIMIT_AS address-space ceiling,
 * set between fork and execvp. WP10 §10: "RLIMIT_AS=dec_mem_limit gives
 * real runtime memory enforcement (exceed -> killed -> guard -> generic
 * fallback)". The ceiling comes from the codec being executed:
 *  - a codecpack whose manifest declares dec_mem > 0: max(2*dec_mem, 256MB)
 *    (2x headroom over the admitted decode working set; the floor keeps a
 *    small-dec_mem pack's interpreter/linker comfortably inside);
 *  - everything else (builtin tool callers -- cjxl/djxl direct, ffmpeg,
 *    mac, packMP3 -- and packs without a dec_mem): the 2GB default cap.
 * A helper that exceeds its ceiling fails its allocations and dies; the
 * non-zero/killed exit surfaces through tool_exec as a tool failure, which
 * the sweep treats as a guard refusal (or a no-stamp fallthrough), never
 * as an FS error. Best effort: a setrlimit failure still execs. */
#define TOOL_MEM_CAP_DEFAULT (2ull << 30)    /* 2 GiB */

#define TOOL_MEM_CAP_FLOOR   (256ull << 20)  /* 256 MiB */


static uint64_t tool_mem_cap_for(const invfs_codec *c)
{
    if (c && c->dec_mem_bytes) {
        uint64_t cap = c->dec_mem_bytes > UINT64_MAX / 2
                     ? UINT64_MAX : 2 * c->dec_mem_bytes;
        return cap > TOOL_MEM_CAP_FLOOR ? cap : TOOL_MEM_CAP_FLOOR;
    }
    return TOOL_MEM_CAP_DEFAULT;
}


static void tool_child_memlimit(uint64_t mem_cap)
{
    struct rlimit rl;
    if (!mem_cap) return;
    rl.rlim_cur = (rlim_t)mem_cap;
    rl.rlim_max = (rlim_t)mem_cap;
    setrlimit(RLIMIT_AS, &rl);
}


/* fork/execvp, wait with a timeout. The child is muted (stdin/out/err to
 * /dev/null), matching the CREATE_NO_WINDOW processes on the Windows side.
 * Returns the child's exit code, or -1 on fork failure, a kill, or expiry. */
static int tool_exec_lim(char *const argv[], uint64_t mem_cap)
{
    pid_t pid;
    int st = 0;
    uint64_t t0;

    if (!argv || !argv[0]) return -1;
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, STDIN_FILENO);
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
        }
        tool_child_memlimit(mem_cap);
        execvp(argv[0], argv);
        _exit(127);
    }
    t0 = tool_now_ms();
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (tool_now_ms() - t0 > TOOL_TIMEOUT_MS) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
                ;
            fprintf(stderr, "tool_exec: %s killed after %llus\n", argv[0],
                    (unsigned long long)(TOOL_TIMEOUT_MS / 1000));
            return -1;
        }
        usleep(5000);
    }
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}


static int tool_exec(char *const argv[])
{
    return tool_exec_lim(argv, TOOL_MEM_CAP_DEFAULT);
}

int tool_tmpdir(char *dir, size_t cap)
{
    static const char *roots[] = { "/dev/shm", "/tmp" };
    size_t r;

    for (r = 0; r < sizeof roots / sizeof roots[0]; r++) {
        int n = snprintf(dir, cap, "%s/invfs-tool-XXXXXX", roots[r]);
        if (n > 0 && (size_t)n < cap && mkdtemp(dir) != NULL)
            return 0;
    }
    return -1;
}

void tool_rm(const char *dir, const char *name)
{
    char p[320];
    int n = snprintf(p, sizeof p, "%s/%s", dir, name);
    if (n > 0 && (size_t)n < sizeof p) unlink(p);
}


/* write a whole buffer, creating/truncating; 0 on success */
int tool_write(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    return fclose(f);
}


/* slurp a tool's output file; 0 only if it exists and is non-empty (an
 * empty output is how several of these tools say "refused") */
static int tool_slurp_out(const char *path, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (slurp_file(path, out, out_len) != 0 || *out_len == 0) {
        free(*out);
        *out = NULL;
        *out_len = 0;
        return -1;
    }
    return 0;
}


/* like tool_exec_lim, but the child's stdout lands in buf (NUL-terminated,
 * truncated at cap-1, overflow drained and discarded so the child never
 * blocks on a full pipe). Used by the codecpack estimate hook. */
static int tool_exec_out_lim(char *const argv[], char *buf, size_t cap,
                             uint64_t mem_cap)
{
    int pfd[2], st = 0, exited = 0;
    pid_t pid;
    uint64_t t0;
    size_t got = 0;

    if (cap) buf[0] = '\0';
    if (pipe(pfd) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, STDIN_FILENO);
            dup2(dn, STDERR_FILENO);
        }
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        tool_child_memlimit(mem_cap);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL, 0) | O_NONBLOCK);
    t0 = tool_now_ms();
    for (;;) {
        pid_t w;
        for (;;) {   /* drain what there is; discard past cap */
            char junk[256];
            char *dst = got + 1 < cap ? buf + got : junk;
            size_t room = dst == junk ? sizeof junk : cap - 1 - got;
            ssize_t r = read(pfd[0], dst, room);
            if (r <= 0) break;
            if (dst != junk) got += (size_t)r;
        }
        if (exited) break;      /* reaped and drained */
        w = waitpid(pid, &st, WNOHANG);
        if (w == pid) { exited = 1; continue; }
        if (w < 0 && errno != EINTR) { close(pfd[0]); return -1; }
        if (tool_now_ms() - t0 > TOOL_TIMEOUT_MS) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
                ;
            close(pfd[0]);
            return -1;
        }
        usleep(5000);
    }
    close(pfd[0]);
    if (cap) buf[got < cap ? got : cap - 1] = '\0';
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

#endif /* !_WIN32 */


/* ---- codecpack execution hooks (WP13; declared in codec.h, called by the
 * codec.c trampolines and the sweep's pack branch) ---- */

/* Substitute {in} {out} {pack} {idx} {dir} {recipe} in one argv token.
 * Returns 0 on overflow. */
static size_t pack_subst(char *dst, size_t cap, const char *tok,
                         const char *packdir, const char *in, const char *out,
                         const char *idx, const char *dir, const char *recipe)
{
    size_t w = 0;

    while (*tok) {
        const char *rep;
        size_t rl;
        if (strncmp(tok, "{in}", 4) == 0)         { rep = in;      tok += 4; }
        else if (strncmp(tok, "{out}", 5) == 0)   { rep = out;     tok += 5; }
        else if (strncmp(tok, "{pack}", 6) == 0)  { rep = packdir; tok += 6; }
        else if (strncmp(tok, "{idx}", 5) == 0)   { rep = idx;     tok += 5; }
        else if (strncmp(tok, "{dir}", 5) == 0)   { rep = dir;     tok += 5; }
        else if (strncmp(tok, "{recipe}", 8) == 0){ rep = recipe;  tok += 8; }
        else { rep = tok++; rl = 1; goto emit; }
        rl = rep ? strlen(rep) : 0;
    emit:
        if (w + rl + 1 > cap) return 0;
        memcpy(dst + w, rep, rl);
        w += rl;
    }
    dst[w] = '\0';
    return w;
}


/* Fixed argv from the manifest template: split on whitespace, substitute
 * placeholders per token (no shell — WP10 §10). A bare argv[0] stays bare
 * (execvp does the PATH search); a relative path containing '/' is taken
 * relative to the pack dir (mirrors manifest_tool_ok). */
static int pack_argv_build(const invfs_pack_def *def, const char *tmpl,
                           const char *in, const char *out,
                           const char *idx, const char *dir,
                           const char *recipe,
                           char *argv[], size_t maxa,
                           char *arena, size_t acap)
{
    size_t used = 0, argc = 0;

    while (*tmpl) {
        char tok[1024];
        size_t tl = 0, w;

        while (*tmpl == ' ' || *tmpl == '\t') tmpl++;
        if (!*tmpl) break;
        while (tmpl[tl] && tmpl[tl] != ' ' && tmpl[tl] != '\t') {
            if (tl + 1 >= sizeof tok) return -1;
            tok[tl] = tmpl[tl];
            tl++;
        }
        tok[tl] = '\0';
        tmpl += tl;
        if (argc + 1 >= maxa) return -1;
        w = pack_subst(arena + used, acap - used, tok,
                       def->dir, in ? in : "", out ? out : "",
                       idx ? idx : "", dir ? dir : "", recipe ? recipe : "");
        if (!w && tok[0]) return -1;    /* arena overflow */
        if (argc == 0 && !strchr(arena + used, '/')) {
            /* WP16e: a bare argv[0] resolves exactly the way the pack
             * probe (codec.c pack_tool_resolvable) and the builtin tool
             * layer do: $INVFS_TOOLS/<name> -> /usr/lib/invfs/tools/<name>
             * -> bare name (execvp's PATH search). Without this the probe
             * could pass on INVFS_TOOLS while execvp still ran the PATH
             * tool of the same name. */
            char rb[4096];
            const char *rp = tool_resolve(arena + used, rb, sizeof rb);
            size_t rl = strlen(rp);
            if (rl + 1 > acap - used) return -1;
            if (rp != arena + used) memcpy(arena + used, rp, rl + 1);
            w = rl;
        } else if (argc == 0 && strchr(arena + used, '/') && arena[used] != '/') {
            /* relative path: resolve against the pack dir */
            char joined[4096];
            int n = snprintf(joined, sizeof joined, "%s/%s",
                             def->dir, arena + used);
            if (n <= 0 || (size_t)n >= sizeof joined ||
                (size_t)n + 1 > acap - used) return -1;
            memcpy(arena + used, joined, (size_t)n + 1);
            w = (size_t)n;
        }
        argv[argc++] = arena + used;
        used += w + 1;
    }
    argv[argc] = NULL;
    return argc ? 0 : -1;
}


int invfs_codec_pack_exec(const invfs_codec *c, int is_encode,
                          const char *in_path, const char *out_path)
{
#ifdef _WIN32
    (void)c; (void)is_encode; (void)in_path; (void)out_path;
    return -1;   /* the POSIX tool layer does not exist on Windows */
#else
    const invfs_pack_def *def = invfs_codec_pack_def(c);
    const char *tmpl;
    char *argv[24];
    char arena[4096];

    if (!def) return -1;
    tmpl = is_encode ? def->encode : def->decode;
    if (!tmpl || !in_path || !out_path) return -1;
    if (pack_argv_build(def, tmpl, in_path, out_path, NULL, NULL, NULL,
                        argv, 24, arena, sizeof arena) != 0)
        return -1;
    /* WP12(d): the child runs under the pack's RLIMIT_AS ceiling
     * (max(2*dec_mem, 256MB) when the manifest declares dec_mem, else the
     * 2GB default) */
    return tool_exec_lim(argv, tool_mem_cap_for(c));
#endif
}


/* WP16a: run one of a CONTAINER pack's four commands. See codec.h for the
 * command set and the placeholder contract. */
int invfs_codec_pack_cmd(const invfs_codec *c, int cmd,
                         const char *in, const char *idx,
                         const char *dir, const char *recipe,
                         const char *out)
{
#ifdef _WIN32
    (void)c; (void)cmd; (void)in; (void)idx; (void)dir; (void)recipe;
    (void)out;
    return -1;   /* the POSIX tool layer does not exist on Windows */
#else
    const invfs_pack_def *def = invfs_codec_pack_def(c);
    const char *tmpl;
    char *argv[24];
    char arena[4096];

    if (!def || !def->is_container) return -1;
    switch (cmd) {
    case INVFS_PACK_CMD_ENUMERATE: tmpl = def->enumerate; break;
    case INVFS_PACK_CMD_EXTRACT:   tmpl = def->extract;   break;
    case INVFS_PACK_CMD_STRIP:     tmpl = def->strip;     break;
    case INVFS_PACK_CMD_REBUILD:   tmpl = def->rebuild;   break;
    case INVFS_PACK_CMD_MAP:       tmpl = def->map;       break;
    default: return -1;
    }
    if (!tmpl) return -1;
    if (pack_argv_build(def, tmpl, in, out, idx, dir, recipe,
                        argv, 24, arena, sizeof arena) != 0)
        return -1;
    return tool_exec_lim(argv, tool_mem_cap_for(c));   /* WP12(d) */
#endif
}


int invfs_codec_pack_estimate(const invfs_codec *c, const char *in_path,
                              uint64_t *out_bytes)
{
#ifdef _WIN32
    (void)c; (void)in_path; (void)out_bytes;
    return -1;
#else
    const invfs_pack_def *def = invfs_codec_pack_def(c);
    char *argv[24];
    char arena[4096];
    char out[256];
    char *endp = NULL;
    unsigned long long v;

    if (!def || !def->estimate || !in_path) return -1;
    if (pack_argv_build(def, def->estimate, in_path, NULL, NULL, NULL, NULL,
                        argv, 24, arena, sizeof arena) != 0)
        return -1;
    if (tool_exec_out_lim(argv, out, sizeof out, tool_mem_cap_for(c)) != 0)
        return -1;   /* WP12(d): the estimate child is capped too */
    errno = 0;
    v = strtoull(out, &endp, 10);
    if (errno || endp == out) return -1;
    while (*endp == ' ' || *endp == '\t' || *endp == '\n' || *endp == '\r')
        endp++;
    if (*endp) return -1;   /* trailing garbage: not a bare byte count */
    *out_bytes = (uint64_t)v;
    return 0;
#endif
}

int run_tool(const char *exe, const char *a1, const char *a2, const char *opts)
{
#ifdef _WIN32
    WCHAR wexe[512], wcmd[4096];
    WCHAR wa1[512], wa2[512], wopts[256];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    MultiByteToWideChar(CP_UTF8, 0, exe, -1, wexe, 512);
    MultiByteToWideChar(CP_UTF8, 0, a1, -1, wa1, 512);
    MultiByteToWideChar(CP_UTF8, 0, a2, -1, wa2, 512);
    MultiByteToWideChar(CP_UTF8, 0, opts, -1, wopts, 256);
    swprintf_s(wcmd, 4096, L"\"%s\" \"%s\" \"%s\" %s", wexe, wa1, wa2, wopts);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(wexe, wcmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        fprintf(stderr, "run_tool: CreateProcess failed (%lu): %ls\n",
                (unsigned long)GetLastError(), wcmd);
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
#else
    /* "<tool> <in> <out> <opts...>" — opts is a compile-time literal at
     * every call site ("--lossless_jpeg=1", "-d 0 -e 7", ...), split on
     * whitespace into a fixed argv; no shell, no quoting. */
    char exeb[512], obuf[256];
    char *argv[16], *save = NULL, *tok;
    int ac = 0;

    argv[ac++] = (char *)tool_resolve(exe, exeb, sizeof exeb);
    argv[ac++] = (char *)a1;
    argv[ac++] = (char *)a2;
    snprintf(obuf, sizeof obuf, "%s", opts ? opts : "");
    for (tok = strtok_r(obuf, " \t", &save); tok && ac < 15;
         tok = strtok_r(NULL, " \t", &save))
        argv[ac++] = tok;
    argv[ac] = NULL;
    return tool_exec(argv);
#endif
}


/* ffmpeg needs -i before the input */
static int run_ffmpeg(const char *a1, const char *a2, const char *opts)
{
#ifdef _WIN32
    WCHAR wcmd[4096];
    WCHAR wa1[512], wa2[512], wopts[256];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    MultiByteToWideChar(CP_UTF8, 0, a1, -1, wa1, 512);
    MultiByteToWideChar(CP_UTF8, 0, a2, -1, wa2, 512);
    MultiByteToWideChar(CP_UTF8, 0, opts, -1, wopts, 256);
    /* options MUST come before the output file: ffmpeg 8.x ignores
       -c:a/-sample_fmt placed after the output (silently emits 16-bit) */
    swprintf_s(wcmd, 4096, L"ffmpeg -loglevel error -i \"%s\" %s \"%s\"", wa1, wopts, wa2);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        fprintf(stderr, "run_ffmpeg: CreateProcess failed (%lu)\n",
                (unsigned long)GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0)
        fprintf(stderr, "run_ffmpeg: exit code %lu: %ls\n", (unsigned long)code, wcmd);
    return (int)code;
#else
    /* ffmpeg -loglevel error -i <a1> <opts...> <a2> — options MUST come
     * before the output file: ffmpeg 8.x ignores -c:a/-sample_fmt placed
     * after the output (silently emits 16-bit) */
    char exeb[512], obuf[256];
    char *argv[20], *save = NULL, *tok;
    int ac = 0, rc;

    argv[ac++] = (char *)tool_resolve("ffmpeg", exeb, sizeof exeb);
    argv[ac++] = (char *)"-loglevel";
    argv[ac++] = (char *)"error";
    argv[ac++] = (char *)"-i";
    argv[ac++] = (char *)a1;
    snprintf(obuf, sizeof obuf, "%s", opts ? opts : "");
    for (tok = strtok_r(obuf, " \t", &save); tok && ac < 18;
         tok = strtok_r(NULL, " \t", &save))
        argv[ac++] = tok;
    argv[ac++] = (char *)a2;
    argv[ac] = NULL;
    rc = tool_exec(argv);
    if (rc != 0)
        fprintf(stderr, "run_ffmpeg: exit code %d\n", rc);
    return rc;
#endif
}


int invfs_jxl_compress(const uint8_t *jpeg, size_t jpeg_len,
                       uint8_t **jxl_out, size_t *jxl_len)
{
#ifdef _WIN32
    char jpg_tmp[256], jxl_tmp[256];
    static const char *cjxl = "D:\\VFS\\tools\\jxl\\x64-windows-static\\bin\\cjxl.exe";
    FILE *f;
    long sz;

    _snprintf_s(jpg_tmp, sizeof jpg_tmp, _TRUNCATE, "%s\\%d_tmp.jpg",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(jxl_tmp, sizeof jxl_tmp, _TRUNCATE, "%s\\%d_tmp.jxl",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    f = fopen(jpg_tmp, "wb");
    if (!f) return -1;
    fwrite(jpeg, 1, jpeg_len, f);
    fclose(f);

    /* lossless JPEG transcode: JXL stores the original JPEG bitstream
     * (boxes), djxl reconstructs the exact same JPEG bytes (1:1 invariant) */
    if (run_tool(cjxl, jpg_tmp, jxl_tmp, "--lossless_jpeg=1") != 0) {
        remove(jpg_tmp); remove(jxl_tmp);
        return -1;
    }
    f = fopen(jxl_tmp, "rb");
    if (!f) { remove(jpg_tmp); remove(jxl_tmp); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    *jxl_out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*jxl_out) { fclose(f); remove(jpg_tmp); remove(jxl_tmp); return -1; }
    if (fread(*jxl_out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*jxl_out); fclose(f); remove(jpg_tmp); remove(jxl_tmp); return -1;
    }
    fclose(f);
    *jxl_len = (size_t)sz;
    remove(jpg_tmp); remove(jxl_tmp);
    return 0;
#else
    char dir[64], in[128], out[128];
    int rc;

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(in, sizeof in, "%s/in.jpg", dir);
    snprintf(out, sizeof out, "%s/out.jxl", dir);
    if (tool_write(in, jpeg, jpeg_len) != 0) { rmdir(dir); return -1; }

    /* lossless JPEG transcode: JXL stores the original JPEG bitstream
     * (boxes), djxl reconstructs the exact same JPEG bytes (1:1 invariant) */
    rc = run_tool("cjxl", in, out, "--lossless_jpeg=1");
    if (rc != 0 || tool_slurp_out(out, jxl_out, jxl_len) != 0) {
        tool_rm(dir, "in.jpg"); tool_rm(dir, "out.jxl"); rmdir(dir);
        return -1;
    }
    tool_rm(dir, "in.jpg"); tool_rm(dir, "out.jxl"); rmdir(dir);
    return 0;
#endif
}


int invfs_jxl_decompress(const uint8_t *jxl, size_t jxl_len,
                         uint8_t **jpg_out, size_t *jpg_len)
{
#ifdef _WIN32
    char jxl_tmp[256], jpg_tmp[256];
    static const char *djxl = "D:\\VFS\\tools\\jxl\\x64-windows-static\\bin\\djxl.exe";
    FILE *f;
    long sz;

    _snprintf_s(jxl_tmp, sizeof jxl_tmp, _TRUNCATE, "%s\\%d_tmp2.jxl",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(jpg_tmp, sizeof jpg_tmp, _TRUNCATE, "%s\\%d_tmp2.jpg",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    f = fopen(jxl_tmp, "wb");
    if (!f) return -1;
    fwrite(jxl, 1, jxl_len, f);
    fclose(f);

    if (run_tool(djxl, jxl_tmp, jpg_tmp, "") != 0) {
        remove(jxl_tmp); remove(jpg_tmp);
        return -1;
    }
    f = fopen(jpg_tmp, "rb");
    if (!f) { remove(jxl_tmp); remove(jpg_tmp); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    *jpg_out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*jpg_out) { fclose(f); remove(jxl_tmp); remove(jpg_tmp); return -1; }
    if (fread(*jpg_out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*jpg_out); fclose(f); remove(jxl_tmp); remove(jpg_tmp); return -1;
    }
    fclose(f);
    *jpg_len = (size_t)sz;
    remove(jxl_tmp); remove(jpg_tmp);
    return 0;
#else
    /* djxl picks the output format by extension: a JXL holding a JPEG
     * reconstruction written to "*.jpg" reproduces the original bytes */
    char dir[64], in[128], out[128];
    int rc;

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(in, sizeof in, "%s/in.jxl", dir);
    snprintf(out, sizeof out, "%s/out.jpg", dir);
    if (tool_write(in, jxl, jxl_len) != 0) { rmdir(dir); return -1; }

    rc = run_tool("djxl", in, out, "");
    if (rc != 0 || tool_slurp_out(out, jpg_out, jpg_len) != 0) {
        tool_rm(dir, "in.jxl"); tool_rm(dir, "out.jpg"); rmdir(dir);
        return -1;
    }
    tool_rm(dir, "in.jxl"); tool_rm(dir, "out.jpg"); rmdir(dir);
    return 0;
#endif
}


/* MP3 -> PMP (packMP3, lossless and bit-exact).
 *
 * packMP3 differs from cjxl/MAC in three ways that shape this code:
 *   - it takes ONE path and derives the output name itself (foo.mp3 ->
 *     foo.pmp, next to the input), so run_tool's "in out" form is unusable
 *     and we must know the output name in advance;
 *   - it decides compress-vs-decompress by CONTENT, not extension, so the
 *     same binary and the same argument shape serve both directions;
 *   - it exits 0 on a refused file (readme: "For unrecognized file types no
 *     action is taken"), and MPEG-2/2.5 Layer III really is refused even
 *     though the extension says .mp3. So the exit code proves nothing --
 *     only the presence of the output file does. Verified: a MPEG-2 file
 *     printed "fatal error: file is MPEG-2 LAYER III, not supported" and
 *     still exited 0.
 * Both halves therefore check for the produced file, not the status. */
static int run_packmp3(const char *path)
{
#ifdef _WIN32
    static const char *pmp =
        "D:\\VFS\\packMP3-v1.0g\\packMP3.exe";
    WCHAR wexe[512], wcmd[4096], wpath[512];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    MultiByteToWideChar(CP_UTF8, 0, pmp, -1, wexe, 512);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512);
    /* -np: never block on "press any key" -- this runs with no console.
       -o: overwrite, else packMP3 invents foo_.pmp and we'd read a stale file.
       No -p: warnings must abort. -p relaxes them but the readme is explicit
       that reconstruction is then not guaranteed bit-exact, which would break
       the 1:1 invariant this filesystem exists to hold. */
    swprintf_s(wcmd, 4096, L"\"%s\" -np -o \"%s\"", wexe, wpath);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(wexe, wcmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        fprintf(stderr, "run_packmp3: CreateProcess failed (%lu)\n",
                (unsigned long)GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
#else
    char exeb[512];
    char *argv[5];

    argv[0] = (char *)tool_resolve("packMP3", exeb, sizeof exeb);
    argv[1] = (char *)"-np";   /* never block on "press any key" */
    argv[2] = (char *)"-o";    /* overwrite, else foo_.pmp is invented */
    argv[3] = (char *)path;
    argv[4] = NULL;
    return tool_exec(argv);
#endif
}


/* read a whole file into a fresh buffer; returns 0 on success */
static int slurp_file(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    if (!f) return -1;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    *out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*out) { fclose(f); return -1; }
    if (fread(*out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*out); *out = NULL; fclose(f); return -1;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return 0;
}


int invfs_pmp_compress(const uint8_t *mp3, size_t mp3_len,
                       uint8_t **pmp_out, size_t *pmp_len)
{
#ifdef _WIN32
    char mp3_tmp[256], pmp_tmp[256];
    FILE *f;
    int rc;

    _snprintf_s(mp3_tmp, sizeof mp3_tmp, _TRUNCATE, "%s\\%d_tmp.mp3",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(pmp_tmp, sizeof pmp_tmp, _TRUNCATE, "%s\\%d_tmp.pmp",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    /* a leftover .pmp from an earlier file would be read back as this
       file's output, silently storing the wrong audio */
    remove(pmp_tmp);

    f = fopen(mp3_tmp, "wb");
    if (!f) return -1;
    fwrite(mp3, 1, mp3_len, f);
    fclose(f);

    run_packmp3(mp3_tmp);
    rc = slurp_file(pmp_tmp, pmp_out, pmp_len);   /* absence == refused */
    remove(mp3_tmp); remove(pmp_tmp);
    return rc;
#else
    /* packMP3 derives the output name from the input (in.mp3 -> in.pmp);
     * the scratch dir is fresh per call, so a stale blob cannot be misread */
    char dir[64], in[128], out[128];
    int rc;

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(in, sizeof in, "%s/in.mp3", dir);
    snprintf(out, sizeof out, "%s/in.pmp", dir);
    if (tool_write(in, mp3, mp3_len) != 0) { rmdir(dir); return -1; }

    run_packmp3(in);   /* exit 0 even on refusal: only the blob proves it */
    rc = tool_slurp_out(out, pmp_out, pmp_len);
    tool_rm(dir, "in.mp3"); tool_rm(dir, "in.pmp"); rmdir(dir);
    return rc;
#endif
}


int invfs_pmp_decompress(const uint8_t *pmp, size_t pmp_len,
                         uint8_t **mp3_out, size_t *mp3_len)
{
#ifdef _WIN32
    char pmp_tmp[256], mp3_tmp[256];
    FILE *f;
    int rc;

    /* separate names from the compress side: a sweep and a read can run in
       the same process, and _tmp.mp3 is live there */
    _snprintf_s(pmp_tmp, sizeof pmp_tmp, _TRUNCATE, "%s\\%d_tmp2.pmp",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(mp3_tmp, sizeof mp3_tmp, _TRUNCATE, "%s\\%d_tmp2.mp3",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    remove(mp3_tmp);

    f = fopen(pmp_tmp, "wb");
    if (!f) return -1;
    fwrite(pmp, 1, pmp_len, f);
    fclose(f);

    run_packmp3(pmp_tmp);
    rc = slurp_file(mp3_tmp, mp3_out, mp3_len);
    remove(pmp_tmp); remove(mp3_tmp);
    return rc;
#else
    char dir[64], in[128], out[128];
    int rc;

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(in, sizeof in, "%s/in.pmp", dir);
    snprintf(out, sizeof out, "%s/in.mp3", dir);
    if (tool_write(in, pmp, pmp_len) != 0) { rmdir(dir); return -1; }

    run_packmp3(in);
    rc = tool_slurp_out(out, mp3_out, mp3_len);
    tool_rm(dir, "in.pmp"); tool_rm(dir, "in.mp3"); rmdir(dir);
    return rc;
#endif
}


/* FLAC bits-per-sample from STREAMINFO (ffmpeg 8.x silently downconverts
   24-bit FLAC to 16-bit WAV unless an explicit pcm_s*le codec is given). */
static int flac_bits_per_sample(const uint8_t *flac, size_t n)
{
    if (!flac || n < 42 || memcmp(flac, "fLaC", 4)) return 16;
    uint32_t blen = ((uint32_t)flac[5] << 16) | ((uint32_t)flac[6] << 8) | flac[7];
    if ((flac[4] & 0x7F) != 0 || blen < 34 || n < 8u + blen) return 16;
    const uint8_t *b = flac + 8;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[10 + i];
    int bps = (int)((v >> 36) & 0x1F) + 1;
    return (bps >= 8 && bps <= 32) ? bps : 16;
}


int invfs_ape_compress(const uint8_t *flac, size_t flac_len,
                       uint8_t **ape_out, size_t *ape_len)
{
#ifdef _WIN32
    char flac_tmp[256], wav_tmp[256], ape_tmp[256], fopts[64];
    static const char *mac = "D:\\bin\\MAC.exe";
    FILE *f;
    long sz;

    _snprintf_s(flac_tmp, sizeof flac_tmp, _TRUNCATE, "%s\\%d_tmp.flac",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(wav_tmp, sizeof wav_tmp, _TRUNCATE, "%s\\%d_tmp.wav",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(ape_tmp, sizeof ape_tmp, _TRUNCATE, "%s\\%d_tmp.ape",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    f = fopen(flac_tmp, "wb");
    if (!f) return -1;
    fwrite(flac, 1, flac_len, f);
    fclose(f);

    /* FLAC -> WAV (ffmpeg, explicit codec matching the source bit depth —
       ffmpeg 8.x otherwise downconverts 24-bit FLAC to 16-bit WAV),
       then WAV -> APE (MAC.exe -c4000, density profile) */
    int bps = flac_bits_per_sample(flac, flac_len);
    if (bps >= 25)      _snprintf_s(fopts, sizeof fopts, _TRUNCATE, "-y -c:a pcm_s32le");
    else if (bps >= 17) _snprintf_s(fopts, sizeof fopts, _TRUNCATE, "-y -c:a pcm_s24le");
    else if (bps >= 9)  _snprintf_s(fopts, sizeof fopts, _TRUNCATE, "-y -c:a pcm_s16le");
    else                _snprintf_s(fopts, sizeof fopts, _TRUNCATE, "-y -c:a pcm_u8");
    if (run_ffmpeg(flac_tmp, wav_tmp, fopts) != 0 ||
        run_tool(mac, wav_tmp, ape_tmp, "-c4000") != 0) {
        remove(flac_tmp); remove(wav_tmp); remove(ape_tmp);
        return -1;
    }
    f = fopen(ape_tmp, "rb");
    if (!f) { remove(flac_tmp); remove(wav_tmp); remove(ape_tmp); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    *ape_out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*ape_out) { fclose(f); remove(flac_tmp); remove(wav_tmp); remove(ape_tmp); return -1; }
    if (fread(*ape_out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*ape_out); fclose(f); remove(flac_tmp); remove(wav_tmp); remove(ape_tmp); return -1;
    }
    fclose(f);
    *ape_len = (size_t)sz;
    remove(flac_tmp); remove(wav_tmp); remove(ape_tmp);
    return 0;
#else
    /* FLAC -> WAV (ffmpeg, explicit codec matching the source bit depth),
     * WAV -> APE (mac -c4000) */
    char dir[64], fin[128], wmid[128], aout[128], fopts[64];
    int bps = flac_bits_per_sample(flac, flac_len);

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(fin, sizeof fin, "%s/in.flac", dir);
    snprintf(wmid, sizeof wmid, "%s/mid.wav", dir);
    snprintf(aout, sizeof aout, "%s/out.ape", dir);
    if (tool_write(fin, flac, flac_len) != 0) { rmdir(dir); return -1; }

    if (bps >= 25)      snprintf(fopts, sizeof fopts, "-y -c:a pcm_s32le");
    else if (bps >= 17) snprintf(fopts, sizeof fopts, "-y -c:a pcm_s24le");
    else if (bps >= 9)  snprintf(fopts, sizeof fopts, "-y -c:a pcm_s16le");
    else                snprintf(fopts, sizeof fopts, "-y -c:a pcm_u8");
    if (run_ffmpeg(fin, wmid, fopts) != 0 ||
        run_tool("mac", wmid, aout, "-c4000") != 0 ||
        tool_slurp_out(aout, ape_out, ape_len) != 0) {
        tool_rm(dir, "in.flac"); tool_rm(dir, "mid.wav");
        tool_rm(dir, "out.ape"); rmdir(dir);
        return -1;
    }
    tool_rm(dir, "in.flac"); tool_rm(dir, "mid.wav");
    tool_rm(dir, "out.ape"); rmdir(dir);
    return 0;
#endif
}


int invfs_ape_decompress(const uint8_t *ape, size_t ape_len,
                         uint8_t **flac_out, size_t *flac_len)
{
#ifdef _WIN32
    char ape_tmp[256], wav_tmp[256], flac_tmp[256];
    static const char *mac = "D:\\bin\\MAC.exe";
    FILE *f;
    long sz;

    _snprintf_s(ape_tmp, sizeof ape_tmp, _TRUNCATE, "%s\\%d_tmp2.ape",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(wav_tmp, sizeof wav_tmp, _TRUNCATE, "%s\\%d_tmp2.wav",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(flac_tmp, sizeof flac_tmp, _TRUNCATE, "%s\\%d_tmp2.flac",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    f = fopen(ape_tmp, "wb");
    if (!f) return -1;
    fwrite(ape, 1, ape_len, f);
    fclose(f);

    /* APE -> WAV (MAC.exe -d), WAV -> FLAC (ffmpeg) */
    if (run_tool(mac, ape_tmp, wav_tmp, "-d") != 0 ||
        run_ffmpeg(wav_tmp, flac_tmp, "-y") != 0) {
        remove(ape_tmp); remove(wav_tmp); remove(flac_tmp);
        return -1;
    }
    f = fopen(flac_tmp, "rb");
    if (!f) { remove(ape_tmp); remove(wav_tmp); remove(flac_tmp); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    *flac_out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*flac_out) { fclose(f); remove(ape_tmp); remove(wav_tmp); remove(flac_tmp); return -1; }
    if (fread(*flac_out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*flac_out); fclose(f); remove(ape_tmp); remove(wav_tmp); remove(flac_tmp); return -1;
    }
    fclose(f);
    *flac_len = (size_t)sz;
    remove(ape_tmp); remove(wav_tmp); remove(flac_tmp);
    return 0;
#else
    /* APE -> WAV (mac -d), WAV -> FLAC (ffmpeg) */
    char dir[64], ain[128], wmid[128], fout[128];

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(ain, sizeof ain, "%s/in.ape", dir);
    snprintf(wmid, sizeof wmid, "%s/mid.wav", dir);
    snprintf(fout, sizeof fout, "%s/out.flac", dir);
    if (tool_write(ain, ape, ape_len) != 0) { rmdir(dir); return -1; }

    if (run_tool("mac", ain, wmid, "-d") != 0 ||
        run_ffmpeg(wmid, fout, "-y") != 0 ||
        tool_slurp_out(fout, flac_out, flac_len) != 0) {
        tool_rm(dir, "in.ape"); tool_rm(dir, "mid.wav");
        tool_rm(dir, "out.flac"); rmdir(dir);
        return -1;
    }
    tool_rm(dir, "in.ape"); tool_rm(dir, "mid.wav");
    tool_rm(dir, "out.flac"); rmdir(dir);
    return 0;
#endif
}


/* APE -> WAV in memory (MAC.exe -d only; no ffmpeg re-compress step).
   Used by the FLAC-recipe path: the WAV feeds flacx_rebuild which
   reproduces the ORIGINAL FLAC bytes bit-exactly from the recipe. */
int invfs_ape_to_wav(const uint8_t *ape, size_t ape_len,
                     uint8_t **wav_out, size_t *wav_len)
{
#ifdef _WIN32
    char ape_tmp[256], wav_tmp[256];
    static const char *mac = "D:\\bin\\MAC.exe";
    FILE *f;
    long sz;

    _snprintf_s(ape_tmp, sizeof ape_tmp, _TRUNCATE, "%s\\%d_tmp3.ape",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _snprintf_s(wav_tmp, sizeof wav_tmp, _TRUNCATE, "%s\\%d_tmp3.wav",
                getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());

    f = fopen(ape_tmp, "wb");
    if (!f) return -1;
    fwrite(ape, 1, ape_len, f);
    fclose(f);

    /* APE -> WAV (MAC.exe -d) */
    if (run_tool(mac, ape_tmp, wav_tmp, "-d") != 0) {
        remove(ape_tmp); remove(wav_tmp);
        return -1;
    }
    f = fopen(wav_tmp, "rb");
    if (!f) { remove(ape_tmp); remove(wav_tmp); return -1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    *wav_out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*wav_out) { fclose(f); remove(ape_tmp); remove(wav_tmp); return -1; }
    if (fread(*wav_out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*wav_out); fclose(f); remove(ape_tmp); remove(wav_tmp); return -1;
    }
    fclose(f);
    *wav_len = (size_t)sz;
    remove(ape_tmp); remove(wav_tmp);
    return 0;
#else
    /* APE -> WAV (mac -d only; the WAV feeds flacx_rebuild) */
    char dir[64], ain[128], wout[128];

    if (tool_tmpdir(dir, sizeof dir) != 0) return -1;
    snprintf(ain, sizeof ain, "%s/in.ape", dir);
    snprintf(wout, sizeof wout, "%s/out.wav", dir);
    if (tool_write(ain, ape, ape_len) != 0) { rmdir(dir); return -1; }

    if (run_tool("mac", ain, wout, "-d") != 0 ||
        tool_slurp_out(wout, wav_out, wav_len) != 0) {
        tool_rm(dir, "in.ape"); tool_rm(dir, "out.wav"); rmdir(dir);
        return -1;
    }
    tool_rm(dir, "in.ape"); tool_rm(dir, "out.wav"); rmdir(dir);
    return 0;
#endif
}


/* FLAC transcode (density profile): store the PCM as an APE blob in inode
   `name` (algo=FLACR, file_size = original FLAC size) and the frame recipe
   in sibling inode `name!recipe`. Reading `name` reproduces the original
   FLAC bytes bit-exactly: APE->WAV->flacx_rebuild(recipe). */
uint64_t vol_create_flac_file(invfs_volume *v, const char *name,
                              const uint8_t *flac, size_t flac_len)
{
#ifdef _WIN32
    uint8_t *recipe = NULL, *ape = NULL;
    size_t rlen = 0, ape_len = 0;
    flacx_cover *covers = NULL;
    uint32_t ncv = 0, i;
    uint64_t a;

    if (name_too_long_for_children(name)) return 0;
    if (flacx_extract(flac, flac_len, &recipe, &rlen, &covers, &ncv) != 0) {
        fprintf(stderr, "[vol] flacx_extract failed for %s\n", name);
        return 0;
    }
    if (invfs_ape_compress(flac, flac_len, &ape, &ape_len) != 0) {
        fprintf(stderr, "[vol] APE compress failed for %s\n", name);
        for (i = 0; i < ncv; i++) free(covers[i].data);
        free(covers); free(recipe);
        return 0;
    }
    /* invariant: transcode ONLY if it actually pays off — otherwise the
       original FLAC is kept (git-safe). On synthetic/24-bit material APE
       -c4000 often loses to FLAC -8; on real 16-bit CD music it wins by
       ~2-6% (B-series host benchmarks: APE = 91.1% of FLAC size). */
    {
        size_t cover_bytes = 0;
        for (i = 0; i < ncv; i++) cover_bytes += covers[i].len;
        if (!getenv("INVFS_FORCE_FLACR") && ape_len + rlen + cover_bytes >= flac_len) {
            if (getenv("INVFS_DEBUG"))
                fprintf(stderr, "[vol] %s: APE+recipe+covers %zu+%zu+%zu >= FLAC %zu — keep original\n",
                        name, ape_len, rlen, cover_bytes, flac_len);
            for (i = 0; i < ncv; i++) free(covers[i].data);
            free(covers); free(recipe); free(ape);
            return 0;
        }
    }
    /* Children first, the name-owning record last.
     *
     * This used to write the APE inode under `name` and only then the recipe.
     * That order is not merely leaky, it is destructive: the sweep tombstones
     * the original FLAC as soon as this returns non-zero, so a failure after
     * the APE landed left `name` pointing at APE-compressed PCM with no
     * recipe to rebuild the FLAC container from -- the file could no longer
     * be reconstructed at all. Writing the payload children first means the
     * name only ever flips to the transcoded form once everything needed to
     * decode it is already durable. tar/gz/png always did it this way. */
    char rname[272];
    _snprintf_s(rname, sizeof rname, _TRUNCATE, "%s!recipe", name);
    /* store the recipe ZSTD-compressed (repetitive frame headers shrink
       ~2x); fall back to raw if it does not compress */
    uint64_t b = 0;
    {
        size_t cbound = ZSTD_compressBound(rlen);
        uint8_t *rc = (uint8_t *)malloc(cbound ? cbound : 1);
        if (rc) {
            size_t clen = ZSTD_compress(rc, cbound, recipe, rlen, 19);
            if (!ZSTD_isError(clen) && clen < rlen) {
                b = vol_create_blob_file(v, rname, rc, clen,
                                         (uint64_t)rlen, INVFS_ALGO_ZSTD);
                free(rc);
            } else {
                free(rc);
            }
        }
        if (!b)
            b = vol_create_blob_file(v, rname, recipe, rlen,
                                     (uint64_t)rlen, INVFS_ALGO_NONE);
    }
    if (!b) {
        fprintf(stderr, "[vol] recipe inode failed for %s\n", name);
        for (i = 0; i < ncv; i++) free(covers[i].data);
        free(covers); free(recipe); free(ape);
        return vol_transcode_abort(v, name);
    }
    /* covers as separate inodes "name!coverN" — identical covers across
       tracks become identical segments and are block-deduped. Only kind=0
       slots carry payloads (kind=1 zero-PADDING has no data). */
    uint32_t di = 0;
    for (i = 0; i < ncv; i++) {
        if (!covers[i].data) continue;   /* kind=1 zero-slot */
        char cn[288];
        _snprintf_s(cn, sizeof cn, _TRUNCATE, "%s!cover%u", name, di++);
        uint64_t ci = vol_create_blob_file(v, cn, covers[i].data, covers[i].len,
                                           (uint64_t)covers[i].len, INVFS_ALGO_NONE);
        if (!ci) {
            /* the recipe addresses this slot by name, so a dropped cover is a
               FLAC that cannot be rebuilt -- a failed transcode, not a warning
               to carry forward */
            fprintf(stderr, "[vol] cover inode failed for %s!cover%u\n", name, di - 1);
            for (i = 0; i < ncv; i++) free(covers[i].data);
            free(covers); free(recipe); free(ape);
            return vol_transcode_abort(v, name);
        }
    }
    a = vol_create_blob_file(v, name, ape, ape_len,
                             (uint64_t)flac_len, INVFS_ALGO_FLACR);
    for (i = 0; i < ncv; i++) free(covers[i].data);
    free(covers);
    free(recipe);
    free(ape);
    if (!a) {
        fprintf(stderr, "[vol] APE inode failed for %s\n", name);
        return vol_transcode_abort(v, name);
    }
    return a;
#else
    /* POSIX twin of the Windows body above. Everything it needs is already
     * in-process or on the WP11 tool layer: flacx is embedded
     * (INVFS_EMBED_FLACX), ffmpeg/mac exec via run_tool/run_ffmpeg
     * (invfs_ape_compress, invfs_ape_to_wav -- $INVFS_TOOLS ->
     * /usr/lib/invfs/tools -> PATH, RLIMIT_AS-capped children). `mac`
     * absent -> invfs_ape_compress fails -> a clean refusal: the original
     * FLAC stays RAW and the sweep stamps GENERIC_GUARD (no partial
     * state is ever committed). */
    uint8_t *recipe = NULL, *ape = NULL;
    size_t rlen = 0, ape_len = 0;
    flacx_cover *covers = NULL;
    uint32_t ncv = 0, i;
    uint64_t a;

    if (name_too_long_for_children(name)) return 0;
    if (flacx_extract(flac, flac_len, &recipe, &rlen, &covers, &ncv) != 0) {
        fprintf(stderr, "[vol] flacx_extract failed for %s\n", name);
        return 0;
    }
    if (invfs_ape_compress(flac, flac_len, &ape, &ape_len) != 0) {
        fprintf(stderr, "[vol] APE compress failed for %s\n", name);
        for (i = 0; i < ncv; i++) free(covers[i].data);
        free(covers); free(recipe);
        return 0;
    }
    /* invariant: transcode ONLY if it actually pays off — otherwise the
       original FLAC is kept (git-safe). On synthetic/24-bit material APE
       -c4000 often loses to FLAC -8; on real 16-bit CD music it wins by
       ~2-6% (B-series host benchmarks: APE = 91.1% of FLAC size). */
    {
        size_t cover_bytes = 0;
        for (i = 0; i < ncv; i++) cover_bytes += covers[i].len;
        if (!getenv("INVFS_FORCE_FLACR") && ape_len + rlen + cover_bytes >= flac_len) {
            if (getenv("INVFS_DEBUG"))
                fprintf(stderr, "[vol] %s: APE+recipe+covers %zu+%zu+%zu >= FLAC %zu — keep original\n",
                        name, ape_len, rlen, cover_bytes, flac_len);
            for (i = 0; i < ncv; i++) free(covers[i].data);
            free(covers); free(recipe); free(ape);
            return 0;
        }
    }
    /* Full-house guard: replay the READ path before committing anything --
     * mac -d the APE blob back to WAV, then flacx_rebuild with the recipe
     * and covers -- and demand the original FLAC bytes bit-exactly. The
     * cover array must be the DENSE kind=0 view the read path rebuilds
     * ("name!coverN" numbering skips zero-PADDING slots), not the sparse
     * extract array; >16 data covers is refused exactly like a reader
     * would (flacx_rebuild caps at 16). */
    {
        uint8_t *wav = NULL, *fl = NULL;
        size_t wav_len = 0, fl_len = 0;
        flacx_cover dcov[16];
        uint32_t nd = 0;
        int vok = 0;
        for (i = 0; i < ncv && nd <= 16; i++)
            if (covers[i].data) {
                if (nd == 16) { nd = 17; break; }   /* over the reader cap */
                dcov[nd++] = covers[i];
            }
        if (nd <= 16 &&
            invfs_ape_to_wav(ape, ape_len, &wav, &wav_len) == 0 && wav &&
            flacx_rebuild(wav, wav_len, recipe, rlen, dcov, nd,
                          &fl, &fl_len) == 0 &&
            fl_len == flac_len && memcmp(fl, flac, flac_len) == 0)
            vok = 1;
        free(wav); free(fl);
        if (!vok) {
            fprintf(stderr, "[vol] %s: FLACR decode-back guard refused — "
                            "keep original\n", name);
            for (i = 0; i < ncv; i++) free(covers[i].data);
            free(covers); free(recipe); free(ape);
            return 0;
        }
    }
    /* Children first, the name-owning record last (see the Windows body:
     * the name only ever flips to the transcoded form once everything
     * needed to decode it is already durable). */
    char rname[272];
    snprintf(rname, sizeof rname, "%s!recipe", name);
    /* store the recipe ZSTD-compressed (repetitive frame headers shrink
       ~2x); fall back to raw if it does not compress */
    uint64_t b = 0;
    {
        size_t cbound = ZSTD_compressBound(rlen);
        uint8_t *rc = (uint8_t *)malloc(cbound ? cbound : 1);
        if (rc) {
            size_t clen = ZSTD_compress(rc, cbound, recipe, rlen, 19);
            if (!ZSTD_isError(clen) && clen < rlen) {
                b = vol_create_blob_file(v, rname, rc, clen,
                                         (uint64_t)rlen, INVFS_ALGO_ZSTD);
                free(rc);
            } else {
                free(rc);
            }
        }
        if (!b)
            b = vol_create_blob_file(v, rname, recipe, rlen,
                                     (uint64_t)rlen, INVFS_ALGO_NONE);
    }
    if (!b) {
        fprintf(stderr, "[vol] recipe inode failed for %s\n", name);
        for (i = 0; i < ncv; i++) free(covers[i].data);
        free(covers); free(recipe); free(ape);
        return vol_transcode_abort(v, name);
    }
    /* covers as separate inodes "name!coverN" — identical covers across
       tracks become identical segments and are block-deduped. Only kind=0
       slots carry payloads (kind=1 zero-PADDING has no data). */
    uint32_t di = 0;
    for (i = 0; i < ncv; i++) {
        if (!covers[i].data) continue;   /* kind=1 zero-slot */
        char cn[288];
        snprintf(cn, sizeof cn, "%s!cover%u", name, di++);
        uint64_t ci = vol_create_blob_file(v, cn, covers[i].data, covers[i].len,
                                           (uint64_t)covers[i].len, INVFS_ALGO_NONE);
        if (!ci) {
            /* the recipe addresses this slot by name, so a dropped cover is a
               FLAC that cannot be rebuilt -- a failed transcode, not a warning
               to carry forward */
            fprintf(stderr, "[vol] cover inode failed for %s!cover%u\n", name, di - 1);
            for (i = 0; i < ncv; i++) free(covers[i].data);
            free(covers); free(recipe); free(ape);
            return vol_transcode_abort(v, name);
        }
    }
    a = vol_create_blob_file(v, name, ape, ape_len,
                             (uint64_t)flac_len, INVFS_ALGO_FLACR);
    for (i = 0; i < ncv; i++) free(covers[i].data);
    free(covers);
    free(recipe);
    free(ape);
    if (!a) {
        fprintf(stderr, "[vol] APE inode failed for %s\n", name);
        return vol_transcode_abort(v, name);
    }
    return a;
#endif
}


/* TAR container (density profile): split the archive into members without
   interpreting them (header bytes kept verbatim in the IVFT recipe), store
   each payload in sibling "name!partN" compressed by its best algorithm,
   rebuild = byte-for-byte reassembly (invariant 1:1). */
#define TARX_MAX_PARTS 2048

uint64_t vol_create_tar_file(invfs_volume *v, const char *name,
                             const uint8_t *tar, size_t tar_len)
{
    tarx_member *members = NULL;
    size_t n = 0, trailer_len = 0, i;
    uint8_t *trailer = NULL, *recipe = NULL;
    size_t rlen = 0;
    uint64_t total = 0;

    if (name_too_long_for_children(name)) return 0;
    if (tarx_extract(tar, tar_len, &members, &n, &trailer, &trailer_len) != 0) {
        fprintf(stderr, "[vol] tarx_extract failed for %s\n", name);
        return 0;
    }
    if (n == 0 || n > TARX_MAX_PARTS) {
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[vol] %s: %zu members — keep original\n", name, n);
        free(members); free(trailer);
        return 0;
    }
    if (tarx_build_recipe(members, n, trailer, trailer_len, &recipe, &rlen) != 0) {
        free(members); free(trailer);
        return 0;
    }

    /* parts: "name!partN", each compressed by its best algorithm */
    total = (uint64_t)rlen;
    for (i = 0; i < n; i++) {
        size_t dlen = (size_t)members[i].data_len;
        size_t plen = dlen + (members[i].pad_kind ? 0 : (size_t)members[i].pad_len);
        uint8_t *pd = (uint8_t *)malloc(plen ? plen : 1);
        if (!pd) { free(recipe); free(members); free(trailer);
                   return vol_transcode_abort(v, name); }
        memcpy(pd, tar + members[i].data_off, dlen);
        if (!members[i].pad_kind && members[i].pad_len)
            memcpy(pd + dlen, tar + members[i].data_off + dlen, members[i].pad_len);
        size_t bound = ZSTD_compressBound(plen);
        uint8_t *c = (uint8_t *)malloc(bound);
        if (!c) { free(pd); free(recipe); free(members); free(trailer);
                  return vol_transcode_abort(v, name); }
        size_t cl = ZSTD_compress(c, bound, pd, plen, 19);
        uint32_t algo = INVFS_ALGO_ZSTD;
        if (ZSTD_isError(cl) || cl >= plen) {
            algo = INVFS_ALGO_NONE;
            memcpy(c, pd, plen);
            cl = plen;
        }
        char pn[320];
        snprintf(pn, sizeof pn, "%s!part%u", name, (unsigned)i);
        uint64_t pino = vol_create_blob_file(v, pn, c, cl, (uint64_t)plen, algo);
        if (!pino) {
            fprintf(stderr, "[vol] part inode failed for %s\n", pn);
            free(c); free(pd); free(recipe); free(members); free(trailer);
            return vol_transcode_abort(v, name);
        }
        total += (uint64_t)cl;
        free(c); free(pd);
    }

    /* guard: transcode only when smaller (invariant: never lose) */
    if (total >= tar_len) {
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[vol] %s: parts+recipe %llu >= tar %zu — keep original\n",
                    name, (unsigned long long)total, tar_len);
        free(recipe); free(members); free(trailer);
        return vol_transcode_abort(v, name);
    }

    /* recipe blob: [0x01][zstd] or [0x00][raw] */
    size_t bound = ZSTD_compressBound(rlen);
    uint8_t *rc = (uint8_t *)malloc(bound + 1);
    if (!rc) { free(recipe); free(members); free(trailer);
               return vol_transcode_abort(v, name); }
    size_t rbl = 0;
    size_t rcl = ZSTD_compress(rc + 1, bound, recipe, rlen, 19);
    if (!ZSTD_isError(rcl) && rcl < rlen) { rc[0] = 1; rbl = rcl + 1; }
    else { rc[0] = 0; memcpy(rc + 1, recipe, rlen); rbl = rlen + 1; }
    uint64_t ino = vol_create_blob_file(v, name, rc, rbl,
                                        (uint64_t)tar_len, INVFS_ALGO_TARR);
    if (!ino) {
        fprintf(stderr, "[vol] tar inode failed for %s\n", name);
        free(rc); free(recipe); free(members); free(trailer);
        return vol_transcode_abort(v, name);
    }
    free(rc); free(recipe); free(members); free(trailer);
    return ino;
}


/* GZIP container (density profile): split a .tar.gz into tar members
   (siblings "name!partN", each compressed by its best algorithm) plus a
   gzip-recipe: [gzip header bytes][deflate params (level,memLevel)][crc32]
   [isize] + IVFT (tar structure). Rebuild reproduces the deflate stream
   BIT-EXACTLY with a vanilla zlib replica (windowBits=-15) — verified at
   transcode time against the original stream; non-zlib encoders (7-Zip,
   java) fail the probe and keep the original (invariant). */
uint64_t vol_create_gz_file(invfs_volume *v, const char *name,
                            const uint8_t *gz, size_t gz_len)
{
    if (gz_len < 18 || gz[0] != 0x1F || gz[1] != 0x8B) return 0;
    if (name_too_long_for_children(name)) return 0;
    unsigned flg = gz[3];
    size_t hlen = 10;
    if (flg & 0x04) { unsigned xl = gz[hlen] | (gz[hlen + 1] << 8); hlen += 2 + xl; }
    if (flg & 0x08) { while (gz[hlen]) hlen++; hlen++; }
    if (flg & 0x10) { while (gz[hlen]) hlen++; hlen++; }
    if (flg & 0x02) hlen += 2;
    if (hlen + 8 >= gz_len) return 0;
    size_t stream_len = gz_len - hlen - 8;
    unsigned crc_stored = (unsigned)gz[gz_len - 8] | ((unsigned)gz[gz_len - 7] << 8) |
                          ((unsigned)gz[gz_len - 6] << 16) | ((unsigned)gz[gz_len - 5] << 24);
    unsigned isize = (unsigned)gz[gz_len - 4] | ((unsigned)gz[gz_len - 3] << 8) |
                     ((unsigned)gz[gz_len - 2] << 16) | ((unsigned)gz[gz_len - 1] << 24);

    /* inflate raw deflate (zlib) */
    z_stream in;
    memset(&in, 0, sizeof in);
    if (inflateInit2(&in, -15) != Z_OK) return 0;
    size_t cap = (size_t)isize + (size_t)isize / 2 + 64;
    if (cap < 1024) cap = 1024;
    uint8_t *tar = (uint8_t *)malloc(cap);
    if (!tar) { inflateEnd(&in); return 0; }
    in.next_in = (Bytef *)(gz + hlen);
    in.avail_in = (uInt)(stream_len > 0x7FFFFFFF ? 0x7FFFFFFF : stream_len);
    in.next_out = tar;
    in.avail_out = (uInt)(cap > 0x7FFFFFFF ? 0x7FFFFFFF : cap);
    int rr = inflate(&in, Z_FINISH);
    inflateEnd(&in);
    if (rr != Z_STREAM_END) { free(tar); return 0; }
    size_t tar_len = (size_t)in.total_out;

    /* members */
    tarx_member *members = NULL;
    size_t n = 0, trailer_len = 0;
    uint8_t *trailer = NULL, *recipe = NULL;
    size_t rlen = 0, i;
    if (n == 0 && (tar_len < 512 || memcmp(tar + 257, "ustar", 5) != 0)) {
        /* not a tar inside: keep original */
        free(tar); return 0;
    }
    if (tarx_extract(tar, tar_len, &members, &n, &trailer, &trailer_len) != 0 ||
        n == 0 || n > TARX_MAX_PARTS) {
        free(tar); free(members); free(trailer);
        return 0;
    }
    (void)tar_len;

    /* probe: find (level, memLevel) reproducing the original stream */
    int plevel = 0, pmem = 0, found = 0;
    for (int lv = 1; lv <= 9 && !found; lv++) {
        for (int mm = 7; mm <= 9 && !found; mm++) {
            z_stream s;
            memset(&s, 0, sizeof s);
            if (deflateInit2(&s, lv, Z_DEFLATED, -15, mm,
                             Z_DEFAULT_STRATEGY) != Z_OK) continue;
            size_t bound = deflateBound(&s, (uLong)tar_len);
            uint8_t *re = (uint8_t *)malloc(bound);
            if (!re) { deflateEnd(&s); continue; }
            s.next_in = tar;
            s.avail_in = (uInt)(tar_len > 0x7FFFFFFF ? 0x7FFFFFFF : tar_len);
            s.next_out = re;
            s.avail_out = (uInt)bound;
            int r2 = deflate(&s, Z_FINISH);
            size_t re_len = (size_t)s.total_out;
            deflateEnd(&s);
            if (r2 == Z_STREAM_END && re_len == stream_len &&
                memcmp(re, gz + hlen, stream_len) == 0) {
                plevel = lv; pmem = mm; found = 1;
            }
            free(re);
        }
    }
    if (!found) {
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[vol] %s: deflate not reproducible — keep original\n", name);
        free(tar); free(members); free(trailer);
        return 0;
    }

    /* parts */
    uint64_t total = (uint64_t)rlen;
    for (i = 0; i < n; i++) {
        size_t dlen = (size_t)members[i].data_len;
        size_t plen = dlen + (members[i].pad_kind ? 0 : (size_t)members[i].pad_len);
        uint8_t *pd = (uint8_t *)malloc(plen ? plen : 1);
        if (!pd) { free(tar); free(members); free(trailer); free(recipe);
                   return vol_transcode_abort(v, name); }
        memcpy(pd, tar + members[i].data_off, dlen);
        if (!members[i].pad_kind && members[i].pad_len)
            memcpy(pd + dlen, tar + members[i].data_off + dlen, members[i].pad_len);
        size_t bound = ZSTD_compressBound(plen);
        uint8_t *c = (uint8_t *)malloc(bound);
        if (!c) { free(pd); free(tar); free(members); free(trailer); free(recipe);
                  return vol_transcode_abort(v, name); }
        size_t cl = ZSTD_compress(c, bound, pd, plen, 19);
        uint32_t algo = INVFS_ALGO_ZSTD;
        if (ZSTD_isError(cl) || cl >= plen) {
            algo = INVFS_ALGO_NONE;
            memcpy(c, pd, plen); cl = plen;
        }
        char pn[320];
        snprintf(pn, sizeof pn, "%s!part%u", name, (unsigned)i);
        uint64_t pino = vol_create_blob_file(v, pn, c, cl, (uint64_t)plen, algo);
        if (!pino) { free(c); free(pd); free(tar); free(members); free(trailer); free(recipe);
                     return vol_transcode_abort(v, name); }
        total += (uint64_t)cl;
        free(c); free(pd);
    }

    /* gzip recipe: [IVGZ][ver][level][mem][crc32][isize][hlen(2)][header] + IVFT */
    if (tarx_build_recipe(members, n, trailer, trailer_len, &recipe, &rlen) != 0) {
        free(tar); free(members); free(trailer);
        return vol_transcode_abort(v, name);
    }
    total += (uint64_t)rlen;
    /* guard: transcode only when smaller */
    if (total >= gz_len) {
        if (getenv("INVFS_DEBUG"))
            fprintf(stderr, "[vol] %s: parts+recipe %llu >= gz %zu — keep original\n",
                    name, (unsigned long long)total, gz_len);
        free(tar); free(members); free(trailer); free(recipe);
        return vol_transcode_abort(v, name);
    }
    size_t pre = 4 + 1 + 1 + 1 + 4 + 4 + 2 + hlen;
    uint8_t *gzr = (uint8_t *)malloc(pre + rlen);
    if (!gzr) { free(tar); free(members); free(trailer); free(recipe);
                return vol_transcode_abort(v, name); }
    size_t o = 0;
    memcpy(gzr + o, "IVGZ", 4); o += 4;
    gzr[o++] = 1;
    gzr[o++] = (uint8_t)plevel;
    gzr[o++] = (uint8_t)pmem;
    gzr[o++] = (uint8_t)(crc_stored & 0xFF); gzr[o++] = (uint8_t)((crc_stored >> 8) & 0xFF);
    gzr[o++] = (uint8_t)((crc_stored >> 16) & 0xFF); gzr[o++] = (uint8_t)((crc_stored >> 24) & 0xFF);
    gzr[o++] = (uint8_t)(isize & 0xFF); gzr[o++] = (uint8_t)((isize >> 8) & 0xFF);
    gzr[o++] = (uint8_t)((isize >> 16) & 0xFF); gzr[o++] = (uint8_t)((isize >> 24) & 0xFF);
    gzr[o++] = (uint8_t)(hlen & 0xFF); gzr[o++] = (uint8_t)((hlen >> 8) & 0xFF);
    memcpy(gzr + o, gz, hlen); o += hlen;
    memcpy(gzr + o, recipe, rlen); o += rlen;

    /* recipe blob: [0x01][zstd] or [0x00][raw] */
    size_t bound = ZSTD_compressBound(o);
    uint8_t *rc = (uint8_t *)malloc(bound + 1);
    if (!rc) { free(tar); free(members); free(trailer); free(recipe); free(gzr);
               return vol_transcode_abort(v, name); }
    size_t rbl = 0;
    size_t rcl = ZSTD_compress(rc + 1, bound, gzr, o, 19);
    if (!ZSTD_isError(rcl) && rcl < o) { rc[0] = 1; rbl = rcl + 1; }
    else { rc[0] = 0; memcpy(rc + 1, gzr, o); rbl = o + 1; }
    uint64_t ino = vol_create_blob_file(v, name, rc, rbl,
                                        (uint64_t)gz_len, INVFS_ALGO_GZR);
    if (!ino) {
        fprintf(stderr, "[vol] gz inode failed for %s\n", name);
        free(rc); free(gzr); free(tar); free(members); free(trailer); free(recipe);
        return vol_transcode_abort(v, name);
    }
    free(rc); free(gzr); free(tar); free(members); free(trailer); free(recipe);
    return ino;
}


/* ---- WP16a: container codecpacks (manifest type=container) ----
 *
 * A container pack decomposes a container file into MEMBER inodes that
 * flow through the entire normal pipeline (RAW -> text/binary batching /
 * generic ZSTD / nested containers, recursively). The pack owns the
 * format knowledge through four commands (codec.h has the placeholder
 * contract): enumerate (member table "idx<TAB>suggested_name<TAB>usize"),
 * extract (one member's bytes), strip (recipe = original minus member
 * payloads, pack-owned format), rebuild (recipe + a directory of member
 * files named "<idx>" -> the original, bit-exact).
 *
 * Storage shape (the TAR/EXER pattern): members are sibling inodes
 * "<name>!mbr<NNNN>" (NNNN = zero-padded idx; a sanitized suggested_name
 * rides after a '-' when the pack offers one) created RAW via
 * vol_create_file; the member table is stored verbatim as the sibling
 * "<name>!mbrt" (the recipe is pack-owned and the FS cannot parse it --
 * the table is how the read path learns the member set); the main record
 * is replaced by the recipe blob (zone BINARY, algo = the pack's, one
 * whole-file segment), stamped CONTAINER{algo, pack generation}.
 *
 * The house 1:1 invariant is enforced at sweep time: rebuild over the
 * recipe + extracted members must memcmp-equal the original BEFORE
 * anything reaches disk. Any failure abandons the decomposition (the
 * siblings, if any were already created, are purged) and the file falls
 * through to text/generic UNSTAMPED by the pack -- a guard failure here
 * means the pack is broken for this content, and the generic stamp the
 * file earns below is terminal until its content changes.
 *
 * Read path: algo -> pack container -> materialize "<idx>" files from the
 * member siblings read THROUGH their current stored form (vol_read_file;
 * the pack's extract cannot help -- the original container no longer
 * exists), run rebuild, hand back the bytes. Whole-file unit, ARC-cached
 * by inode id via algo_is_whole_file (the WHOLEFILE cap is forced at
 * registration). A missing/corrupt member or a pack failure fails the
 * read LOUDLY (-1; EIO at the FUSE boundary, exit 1 in invf-cat -- the
 * WP13 missing-pack errno semantics, there is no per-cause channel).
 *
 * WP16b (ABI v1.1): a pack that also declares a `map` command is SEEKABLE
 * (CAP_SEEK at registration). The sweep stores the FS-owned member map as
 * the "<name>!mbrmap" sibling (written LAST, after the map guard -- its
 * presence is the "guarded" marker) and reads serve ranges by LOCAL
 * SPLICE (recipe segment + member sibling reads via cpack_map_read): no
 * pack exec, no whole-file rebuild, and the map is self-describing, so
 * reads keep working with the pack uninstalled. See the map-format block
 * below cpack_parse_table.
 */

#define CPACK_MAX_MEMBERS 65536u   /* the WP10 §12 total-member sanity bound */

#define CPACK_MAX_IDX     65535u   /* idx values name "!mbr<NNNN>" + {dir} files */

#define CPACK_SNAME_MAX   24       /* sanitized suggested_name in a sibling name */

#define CPACK_NAME_RESERVE 40      /* "!mbr" + idx + "-" + sname (and "!mbrt") */


typedef struct {
    uint32_t idx;                        /* member index (the {dir} file name) */
    uint64_t usize;                      /* member size in bytes */
    char     sname[CPACK_SNAME_MAX + 1]; /* sanitized suggested name ("" ok) */
} cpack_member;


/* suggested_name -> the part that may ride inside a sibling name:
 * [A-Za-z0-9._-] kept, anything else folds to '_' (never a '/', never
 * a control byte), capped at CPACK_SNAME_MAX. */
static size_t cpack_sanitize(char *dst, const char *src, size_t n)
{
    size_t w = 0, i;
    for (i = 0; i < n && w < CPACK_SNAME_MAX; i++) {
        unsigned ch = (unsigned char)src[i];
        int ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                 (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                 ch == '-';
        dst[w++] = (char)(ok ? ch : '_');
    }
    dst[w] = 0;
    return w;
}


/* "<name>!mbr<NNNN>" or "<name>!mbr<NNNN>-<san>"; the caller sized the
 * name budget up front (CPACK_NAME_RESERVE), truncation cannot fire. */
static void cpack_mbr_name(char *out, size_t cap, const char *base,
                           uint32_t idx, const char *san)
{
    if (san && san[0])
        snprintf(out, cap, "%s!mbr%04u-%s", base, idx, san);
    else
        snprintf(out, cap, "%s!mbr%04u", base, idx);
}


/* Parse a member table (the enumerate output, stored verbatim as the
 * "name!mbrt" sibling): one line per member "idx<TAB>suggested_name<TAB>
 * usize". Read-side input is disk data, write-side input is pack output --
 * both are validated identically: idx unique and <= CPACK_MAX_IDX, at most
 * CPACK_MAX_MEMBERS rows. usize is deliberately NOT bounded by the
 * container's own size: a compressing container (7z solid blocks) can hold
 * members larger than the archive. Honesty is enforced where the bytes
 * move -- the sweep stats every extracted member against its announced
 * usize, the read side compares what vol_read_file returned against it.
 * *sum_out (nullable) accumulates the member bytes (overflow-refused) for
 * the admission estimate. Returns 0 on success. */
static int cpack_parse_table(const uint8_t *text, size_t len,
                             cpack_member **out, size_t *n_out,
                             uint64_t *sum_out)
{
    cpack_member *mem = NULL;
    size_t n = 0, cap = 0, pos = 0;
    uint8_t *seen = NULL;
    uint64_t sum = 0;
    int rc = -1;

    *out = NULL;
    *n_out = 0;
    if (sum_out) *sum_out = 0;
    seen = (uint8_t *)calloc(CPACK_MAX_IDX / 8 + 1, 1);
    if (!seen) return -1;
    while (pos < len) {
        size_t eol = pos, f1, f2, slen;
        uint64_t idx, usize;
        char numbuf[32];
        while (eol < len && text[eol] != '\n') eol++;
        if (eol == pos) { pos++; continue; }   /* blank line: tolerate */
        /* field boundaries: idx TAB sname TAB usize */
        f1 = pos;
        while (f1 < eol && text[f1] != '\t') f1++;
        f2 = f1;
        if (f2 < eol) f2++;
        while (f2 < eol && text[f2] != '\t') f2++;
        if (f1 >= eol || f2 >= eol || text[f1] != '\t' || text[f2] != '\t')
            goto out;                          /* malformed row */
        if ((size_t)(f1 - pos) >= sizeof numbuf ||
            (size_t)(eol - f2 - 1) >= sizeof numbuf)
            goto out;
        memcpy(numbuf, text + pos, f1 - pos);
        numbuf[f1 - pos] = 0;
        idx = strtoull(numbuf, NULL, 10);
        memcpy(numbuf, text + f2 + 1, eol - f2 - 1);
        numbuf[eol - f2 - 1] = 0;
        usize = strtoull(numbuf, NULL, 10);
        slen = f2 - f1 - 1;
        if (idx > CPACK_MAX_IDX || usize > UINT64_MAX - sum)
            goto out;
        if (seen[idx / 8] & (1u << (idx % 8)))
            goto out;                          /* duplicate idx */
        seen[idx / 8] |= (uint8_t)(1u << (idx % 8));
        if (n == CPACK_MAX_MEMBERS)
            goto out;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 64;
            cpack_member *nm =
                (cpack_member *)realloc(mem, nc * sizeof *nm);
            if (!nm) goto out;
            mem = nm;
            cap = nc;
        }
        mem[n].idx = (uint32_t)idx;
        mem[n].usize = usize;
        cpack_sanitize(mem[n].sname, (const char *)text + f1 + 1, slen);
        n++;
        sum += usize;
        pos = eol + 1;
    }
    *out = mem;
    *n_out = n;
    if (sum_out) *sum_out = sum;
    rc = 0;
out:
    if (rc) free(mem);
    free(seen);
    return rc;
}


/* ---- WP16b: seekable containers (the map command + local splice reads) ----
 *
 * A container pack that declares a `map` command (CAP_SEEK at registration)
 * tells the FS exactly which stored source reproduces each byte range of
 * the original container, so reads never exec the pack: ranges splice from
 * the recipe segment (the main record) and the member siblings. The map is
 * an FS-OWNED binary format (the pack only renders it), little-endian, all
 * fields read by memcpy like every other on-disk value here:
 *
 *   [4B "MRMP"][u32 count]
 *   count x { u64 orig_off, u64 len, u8 kind, u32 idx, u64 src_off }  (29 B)
 *
 * Entries are sorted by orig_off and must partition [0, container_size)
 * exactly (contiguous, no gaps, no overlaps, len > 0). kind 0 = RECIPE:
 * bytes live at [src_off, +len) of the recipe blob (idx must be 0). kind 1
 * = MEMBER: bytes are member idx's [src_off, +len) (idx must exist in the
 * member table, src_off+len within its usize). A zero-length member simply
 * has no map entries.
 *
 * The map is validated at sweep time and again at cache load (it is disk
 * data then); the sweep additionally PROVES it: every entry's source range
 * is read back through the real read path (the recipe segment of the
 * committed recipe record, member siblings through vol_read_range) and
 * chunked-memcmp'd against the original -- streaming, constant memory, and
 * no rebuild exec. The !mbrmap sibling is stored LAST: its presence on disk
 * is the "guard passed" marker, so a crash mid-commit can only ever fall
 * back to the pack's rebuild exec (or fail loudly with the pack absent),
 * never serve an unguarded map.
 */

#define CPACK_MAP_ENT_WIRE 29   /* u64 orig_off + u64 len + u8 kind +

                                 * u32 idx + u64 src_off */
#define CPACK_MAP_MAX_ENTS (4 * CPACK_MAX_MEMBERS + 4)  /* a member split

                                 * into recipe/member runs bounds entries;
                                 * four runs per member is generous */
#define CPACK_GUARD_CHUNK  (8ull << 20)   /* map-guard read-back window */


typedef struct {
    uint64_t orig_off;   /* offset in the ORIGINAL container */
    uint64_t len;        /* covered bytes (never 0) */
    uint64_t src_off;    /* RECIPE: offset in the recipe blob;
                          * MEMBER: offset within member idx */
    uint32_t idx;        /* MEMBER: the member index (its !mbrNNNN sibling);
                          * RECIPE: must be 0 */
    uint8_t  kind;       /* 0 = RECIPE, 1 = MEMBER */
} cpack_map_ent;


/* Parse a map blob (pack output at sweep time, disk data at read time --
 * validated identically). Strict: the blob is exactly the header plus
 * count entries. Returns 0 on success. */
static int cpack_map_parse(const uint8_t *blob, size_t blob_len,
                           cpack_map_ent **out, size_t *n_out)
{
    cpack_map_ent *ents;
    uint32_t count, i;

    *out = NULL;
    *n_out = 0;
    if (blob_len < 8 || memcmp(blob, "MRMP", 4) != 0) return -1;
    memcpy(&count, blob + 4, 4);   /* LE by the host convention (invarifs.h) */
    if (!count || count > CPACK_MAP_MAX_ENTS) return -1;
    if (blob_len != 8 + (size_t)count * CPACK_MAP_ENT_WIRE) return -1;
    ents = (cpack_map_ent *)malloc((size_t)count * sizeof *ents);
    if (!ents) return -1;
    for (i = 0; i < count; i++) {
        const uint8_t *p = blob + 8 + (size_t)i * CPACK_MAP_ENT_WIRE;
        memcpy(&ents[i].orig_off, p, 8);
        memcpy(&ents[i].len, p + 8, 8);
        ents[i].kind = p[16];
        memcpy(&ents[i].idx, p + 17, 4);
        memcpy(&ents[i].src_off, p + 21, 8);
        if (ents[i].kind > 1) { free(ents); return -1; }
    }
    *out = ents;
    *n_out = count;
    return 0;
}


static int cpack_member_idx_cmp(const void *a, const void *b)
{
    uint32_t ia = ((const cpack_member *)a)->idx;
    uint32_t ib = ((const cpack_member *)b)->idx;
    return ia < ib ? -1 : ia > ib;
}


/* a copy of the parsed member table sorted by idx (uniqueness was enforced
 * by the parse), for binary search in validate/serve */
static cpack_member *cpack_members_sorted(const cpack_member *mem, size_t n)
{
    cpack_member *s = (cpack_member *)malloc(n * sizeof *s);
    if (!s) return NULL;
    memcpy(s, mem, n * sizeof *s);
    qsort(s, n, sizeof *s, cpack_member_idx_cmp);
    return s;
}


static const cpack_member *cpack_member_find(const cpack_member *mem,
                                             size_t n, uint32_t idx)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (mem[mid].idx == idx) return &mem[mid];
        if (mem[mid].idx < idx) lo = mid + 1; else hi = mid;
    }
    return NULL;
}


/* Validate a parsed map against the container's shape: entries must exactly
 * partition [0, container_size) (sorted, contiguous, no overlap), RECIPE
 * ranges must fit the recipe blob, MEMBER ranges must fit their member's
 * announced usize. mem_sorted is idx-sorted. 0 = valid. */
static int cpack_map_validate(const cpack_map_ent *e, size_t n,
                              uint64_t container_size, uint64_t recipe_len,
                              const cpack_member *mem_sorted, size_t nmem)
{
    uint64_t pos = 0;
    size_t i;

    if (!n) return -1;
    for (i = 0; i < n; i++) {
        if (e[i].orig_off != pos)
            return -1;                       /* gap/overlap/unsorted */
        if (!e[i].len || e[i].len > container_size - pos)
            return -1;                       /* zero-length or past the end */
        if (e[i].kind == 0) {                /* RECIPE */
            if (e[i].idx != 0) return -1;
            if (e[i].src_off > recipe_len ||
                e[i].len > recipe_len - e[i].src_off)
                return -1;
        } else {                             /* MEMBER */
            const cpack_member *mm = cpack_member_find(mem_sorted, nmem,
                                                       e[i].idx);
            if (!mm) return -1;              /* unknown member */
            if (e[i].src_off > mm->usize ||
                e[i].len > mm->usize - e[i].src_off)
                return -1;
        }
        pos += e[i].len;
    }
    return pos == container_size ? 0 : -1;
}


/* Read the recipe blob = the single stored segment of record `ino`
 * (block_id 0), framing + CRC32C verified. Returns the malloc'd payload. */
static int cpack_recipe_seg(invfs_volume *v, uint64_t ino,
                            uint8_t **out, size_t *out_len)
{
    uint64_t pba = 0, plen = 0;
    uint32_t csize;
    uint8_t *blob;

    *out = NULL;
    *out_len = 0;
    if (vol_lookup_entry(v, ino, 0, &pba, &plen) != 0 || !pba) return -1;
    /* framed read, CRC-verified inside (an empty recipe is legal);
     * a shadow-zone failure gets one WP20 seal-parity recovery attempt */
    if (seg_read_checked(v, pba, plen, 0, &csize, &blob) != 0)
        return -1;
    *out = blob;
    *out_len = csize;
    return 0;
}


/* One member source read, chunked so no single vol_read_range call exceeds
 * what its int return can carry. The member is read through its CURRENT
 * stored form (the real read path -- batched, generic, or itself a nested
 * container); a short read is a corrupt member, and corrupt is LOUD. */
static int cpack_member_read(invfs_volume *v, const char *name,
                             const cpack_member *mem, size_t nmem,
                             uint32_t idx, uint64_t src_off,
                             uint8_t *dst, size_t len)
{
    char mn[320];
    const cpack_member *mm = cpack_member_find(mem, nmem, idx);
    uint64_t pino;
    size_t done = 0;

    if (!mm) return -1;
    cpack_mbr_name(mn, sizeof mn, name, idx, mm->sname);
    pino = vol_find(v, mn);
    if (!pino) return -1;
    while (done < len) {
        size_t want = len - done;
        int got;
        if (want > (64u << 20)) want = 64u << 20;
        got = vol_read_range(v, pino, src_off + done, want, dst + done);
        if (got != (int)want) return -1;
        done += want;
    }
    return 0;
}


/* Serve [off, off+len) of the original container from a validated map:
 * RECIPE ranges copy from the recipe blob, MEMBER ranges read the member
 * sibling. The map must cover the whole request (a validated map partitions
 * the container and callers clamp to its size). 0 = ok, -1 = the map lied
 * or a source is unreadable (loud -- the 1:1 invariant). */
static int cpack_map_serve(invfs_volume *v, const char *name,
                           const cpack_map_ent *e, size_t n,
                           const cpack_member *mem, size_t nmem,
                           const uint8_t *recipe, size_t recipe_len,
                           uint64_t off, uint8_t *dst, size_t len)
{
    size_t lo = 0, hi = n, done = 0;

    if (!len) return 0;
    /* the first entry whose range ends past off (entries are sorted) */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (e[mid].orig_off + e[mid].len > off) hi = mid;
        else lo = mid + 1;
    }
    while (done < len) {
        const cpack_map_ent *en;
        uint64_t rel, avail;
        size_t take;
        if (lo >= n) return -1;              /* ran off the map */
        en = &e[lo++];
        if (en->orig_off > off + done) return -1;   /* a gap is not a map */
        rel = (off + done) - en->orig_off;
        avail = en->len - rel;
        take = avail < (uint64_t)(len - done) ? (size_t)avail
                                              : len - done;
        if (en->kind == 0) {
            if (en->src_off + rel + take > recipe_len) return -1;
            memcpy(dst + done, recipe + (size_t)(en->src_off + rel), take);
        } else if (cpack_member_read(v, name, mem, nmem, en->idx,
                                     en->src_off + rel,
                                     dst + done, take) != 0) {
            return -1;
        }
        done += take;
    }
    return 0;
}


/* The seekable guard: with the members + the recipe record committed, read
 * every map entry's source range back through the real read path (the
 * recipe segment of the fresh recipe record, member siblings through
 * vol_read_range) and chunked-memcmp it against the original container.
 * Streaming, constant memory, no rebuild exec. 0 = the stored state
 * reproduces the original bit-exactly. */
static int cpack_map_guard(invfs_volume *v, const char *name,
                           uint64_t recipe_ino,
                           const uint8_t *full, size_t full_len,
                           const cpack_map_ent *ents, size_t n_ents,
                           const cpack_member *mem_sorted, size_t nmem)
{
    uint8_t *recipe = NULL, *buf = NULL;
    size_t recipe_len = 0, gi;
    int rc = -1;

    if (cpack_recipe_seg(v, recipe_ino, &recipe, &recipe_len) != 0) goto out;
    buf = (uint8_t *)malloc(CPACK_GUARD_CHUNK);
    if (!buf) goto out;
    for (gi = 0; gi < n_ents; gi++) {
        uint64_t done = 0;
        while (done < ents[gi].len) {
            uint64_t left = ents[gi].len - done;
            size_t clen = (size_t)(left > CPACK_GUARD_CHUNK
                                   ? CPACK_GUARD_CHUNK : left);
            if (cpack_map_serve(v, name, ents, n_ents, mem_sorted, nmem,
                                recipe, recipe_len,
                                ents[gi].orig_off + done, buf, clen) != 0 ||
                memcmp(buf, full + (size_t)ents[gi].orig_off + (size_t)done,
                       clen) != 0)
                goto out;
            done += clen;
        }
    }
    rc = 0;
out:
    free(recipe);
    free(buf);
    return rc;
}


/* The per-volume parsed-map cache: one entry per seekable container the
 * read path has touched. Never re-read mid-session: content is immutable
 * under a name (rewrites are delete+create, and the retire invalidates).
 * Freed wholesale at vol_close. */
struct cpack_map_cache {
    char    *name;               /* the container's name (owned) */
    cpack_map_ent *ents;         /* the validated map, sorted by orig_off */
    size_t   n_ents;
    cpack_member *mem;           /* the member table, sorted by idx */
    size_t   nmem;
    uint8_t *recipe;             /* the recipe blob (segment, CRC-checked) */
    size_t   recipe_len;
};


static void cpack_map_cache_free_ent(struct cpack_map_cache *m)
{
    free(m->name);
    free(m->ents);
    free(m->mem);
    free(m->recipe);
    memset(m, 0, sizeof *m);
}

void cpack_map_cache_reset(invfs_volume *v)
{
    size_t i;
    if (!v) return;
    for (i = 0; i < v->maps_n; i++)
        cpack_map_cache_free_ent(&v->maps[i]);
    free(v->maps);
    v->maps = NULL;
    v->maps_n = v->maps_cap = 0;
}


/* Retiring `name` kills the map cached for it, and retiring a "name!..."
 * sibling (a member, the table, the map itself) kills the container's:
 * the next read reloads from the current records. */
void cpack_map_cache_invalidate(invfs_volume *v, const char *name)
{
    size_t i, w = 0;

    if (!v || !name) return;
    for (i = 0; i < v->maps_n; i++) {
        size_t nl = strlen(v->maps[i].name);
        int hit = strcmp(v->maps[i].name, name) == 0 ||
                  (strncmp(name, v->maps[i].name, nl) == 0 && name[nl] == '!');
        if (hit) {
            cpack_map_cache_free_ent(&v->maps[i]);
            continue;
        }
        if (w != i) v->maps[w] = v->maps[i];
        w++;
    }
    v->maps_n = w;
}


/* Load-or-return the cached map for a container. NULL on any failure
 * (missing siblings -- the callers gate on the !mbrmap presence -- a
 * corrupt map or table, an unreadable recipe segment): everything is
 * re-validated against the live record because this IS disk data. */
static const struct cpack_map_cache *cpack_map_get(invfs_volume *v,
                                                   const char *name,
                                                   uint64_t ino,
                                                   uint64_t container_size)
{
    char tn[288], mn[288];
    uint8_t *table = NULL, *mapb = NULL, *recipe = NULL;
    size_t table_len = 0, map_len = 0, recipe_len = 0;
    cpack_member *mem = NULL;
    cpack_map_ent *ents = NULL;
    size_t nmem = 0, nents = 0, i;
    uint64_t tino, mino;
    struct cpack_map_cache *e;

    for (i = 0; i < v->maps_n; i++)
        if (strcmp(v->maps[i].name, name) == 0)
            return &v->maps[i];

    snprintf(tn, sizeof tn, "%s!mbrt", name);
    snprintf(mn, sizeof mn, "%s!mbrmap", name);
    tino = vol_find(v, tn);
    mino = vol_find(v, mn);
    if (!tino || !mino) return NULL;
    if (vol_read_file(v, mino, &mapb, &map_len) != 0 ||
        cpack_map_parse(mapb, map_len, &ents, &nents) != 0)
        goto fail;
    if (vol_read_file(v, tino, &table, &table_len) != 0 ||
        cpack_parse_table(table, table_len, &mem, &nmem, NULL) != 0 || !nmem)
        goto fail;
    qsort(mem, nmem, sizeof *mem, cpack_member_idx_cmp);
    if (cpack_recipe_seg(v, ino, &recipe, &recipe_len) != 0)
        goto fail;
    if (cpack_map_validate(ents, nents, container_size,
                           (uint64_t)recipe_len, mem, nmem) != 0)
        goto fail;
    free(table);
    table = NULL;
    free(mapb);
    mapb = NULL;

    if (v->maps_n == v->maps_cap) {
        size_t nc = v->maps_cap ? v->maps_cap * 2 : 8;
        struct cpack_map_cache *nm =
            (struct cpack_map_cache *)realloc(v->maps, nc * sizeof *nm);
        if (!nm) goto fail_recipe;
        v->maps = nm;
        v->maps_cap = nc;
    }
    e = &v->maps[v->maps_n];
    memset(e, 0, sizeof *e);
    e->name = strdup(name);
    if (!e->name) goto fail_recipe;
    e->ents = ents;
    e->n_ents = nents;
    e->mem = mem;
    e->nmem = nmem;
    e->recipe = recipe;
    e->recipe_len = recipe_len;
    v->maps_n++;
    return e;

fail_recipe:
    free(recipe);
fail:
    free(table);
    free(mapb);
    free(mem);
    free(ents);
    return NULL;
}


/* The WP16b read entry point: serve [off, off+len) of a seekable container
 * by local splice through its cached map. Returns the byte count (clamped
 * at the container size), -1 on any failure -- loud, like a corrupt member
 * on the exec path. */
int cpack_map_read(invfs_volume *v, const char *name, uint64_t ino,
                          uint64_t container_size, uint64_t off,
                          uint8_t *dst, size_t len)
{
    const struct cpack_map_cache *m;

    if (off >= container_size) return 0;
    if (off + len > container_size) len = (size_t)(container_size - off);
    m = cpack_map_get(v, name, ino, container_size);
    if (!m) return -1;
    if (cpack_map_serve(v, name, m->ents, m->n_ents, m->mem, m->nmem,
                        m->recipe, m->recipe_len, off, dst, len) != 0)
        return -1;
    return (int)len;
}



/* WP16a sweep attempt: decompose one RAW container through a container
 * codecpack. See the section header for the pipeline; the return
 * convention mirrors vol_pack_sweep (100+algo on commit, 1 = tools absent
 * -- wait RAW and unstamped, 0 = declined: fall through to text/generic;
 * GENERIC_MEMLIMIT is stamped on a policy refusal, everything else leaves
 * the stamp to the generic path below). */
int vol_containerpack_sweep(invfs_volume *v, uint64_t inode_id,
                                   const char *name, const invfs_codec *pc,
                                   const uint8_t *full, size_t full_len)
{
    const invfs_pack_def *def;
    cpack_member *mem = NULL;
    size_t nmem = 0, i;
    uint64_t sum_usize = 0, ws;
    char dir[64], pin[128], ptable[128], precipe[128], pout[128],
         pmap[128], pmdir[192];
    uint8_t *table = NULL, *recipe = NULL, *outb = NULL;
    size_t table_len = 0, recipe_len = 0, out_len = 0;
    /* WP16b: the pack's map (a seekable container), parsed + validated */
    uint8_t *mapb = NULL;
    size_t map_len = 0;
    cpack_map_ent *ments = NULL;
    size_t nments = 0;
    cpack_member *mem_sorted = NULL;   /* idx-sorted copy for the map paths */
    invfs_meta_pub keep;
    int have_keep, rc = 0;

    if (!pc->probe || !pc->probe()) return 1;    /* tools absent: wait */
    def = invfs_codec_pack_def(pc);
    if (!def || !def->is_container) return 0;
    if (strlen(name) + CPACK_NAME_RESERVE > INVFS_MAX_NAME) return 0;
    {
        /* leftover siblings can only come from a decomposition killed
         * mid-commit (a finished one is CONTAINER-stamped and never
         * reaches here): purge and proceed rather than refusing the file
         * forever (the EXER rule) */
        char tn[288];
        snprintf(tn, sizeof tn, "%s!mbrt", name);
        if (vol_find(v, tn) != 0)
            vol_delete_siblings(v, name);
    }
    if (tool_tmpdir(dir, sizeof dir) != 0) return 0;
    snprintf(pin, sizeof pin, "%s/in", dir);
    snprintf(ptable, sizeof ptable, "%s/table", dir);
    snprintf(precipe, sizeof precipe, "%s/recipe", dir);
    snprintf(pout, sizeof pout, "%s/out", dir);
    snprintf(pmap, sizeof pmap, "%s/map", dir);
    snprintf(pmdir, sizeof pmdir, "%s/mbr", dir);
    if (tool_write(pin, full, full_len) != 0) goto out;

    /* 1. enumerate: the member table */
    if (invfs_codec_pack_cmd(pc, INVFS_PACK_CMD_ENUMERATE, pin, NULL,
                             NULL, NULL, ptable) != 0)
        { if (getenv("INVFS_DEBUG_PACKS")) fprintf(stderr,"[cpack] enumerate failed\n"); goto out; }
    if (slurp_file(ptable, &table, &table_len) != 0) goto out;
    if (cpack_parse_table(table, table_len, &mem, &nmem, &sum_usize) != 0 ||
        nmem == 0)
        { if (getenv("INVFS_DEBUG_PACKS")) fprintf(stderr,"[cpack] table parse failed nmem=%zu\n", nmem); goto out; }

    /* 2. admission (WP10 §12; sweep-time only): the rebuild is a
     * whole-file read into the inode-keyed ARC, so the container obeys
     * the whole-file arc rule; the decode working set comes from the
     * pack's estimate command when it has one (header-derived, never a
     * trial decode), else the manifest dec_mem constant, else the ABI
     * default: sum(member usize) + the container's own size. The recipe
     * is not in hand yet (strip runs next), and the container's size
     * bounds it -- the default covers the read path's true peak (the
     * whole-file output buffer plus one member in flight). */
    if (v->arc_budget && (uint64_t)full_len > v->arc_budget) {
        vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_MEMLIMIT,
                        (uint8_t)pc->algo, pc->generation);
        goto out;
    }
    ws = pc->dec_mem_bytes;
    if (def->estimate) {
        /* a pack that cannot size the job refuses the file (the WP13
         * estimate convention: GENERIC_GUARD, re-armed by a generation
         * bump) */
        if (invfs_codec_pack_estimate(pc, pin, &ws) != 0) {
            vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_GUARD,
                            (uint8_t)pc->algo, pc->generation);
            goto out;
        }
    } else if (!ws) {
        ws = sum_usize + (uint64_t)full_len;
    }
    if (ws && ws > vol_get_dec_mem_limit(v)) {
        vol_stamp_class(v, inode_id, INVFS_CLASS_GENERIC_MEMLIMIT,
                        (uint8_t)pc->algo, pc->generation);
        goto out;
    }

    /* WP16b DEFER_ENOSPC admission: the commit below writes the members,
     * the table and the recipe (+ the map) while the ORIGINAL container is
     * still stored, so price the new shape before the pack does any more
     * work. Member csizes are unknowable pre-write (they compress through
     * the pipeline later, after this sweep's batching); the heuristic
     * charges half the announced member total plus the flat 64 MiB margin,
     * which also covers the recipe/table/map and the records. Under it:
     * wait RAW, re-evaluated every sweep (the class predicate's
     * DEFER_ENOSPC case), never fall to generic. rc 1 = the same silent
     * "wait RAW" the tools-absent case uses. */
    if (sweep_enospc(v, sum_usize / 2 + INVFS_ENOSPC_MARGIN)) {
        vol_stamp_class(v, inode_id, INVFS_CLASS_DEFER_ENOSPC,
                        (uint8_t)pc->algo, pc->generation);
        rc = 1;
        goto out;
    }

    /* 3. strip: the recipe (original minus member payloads, pack-owned) */
    if (invfs_codec_pack_cmd(pc, INVFS_PACK_CMD_STRIP, pin, NULL,
                             NULL, NULL, precipe) != 0)
        { if (getenv("INVFS_DEBUG_PACKS")) fprintf(stderr,"[cpack] strip failed\n"); goto out; }
    if (slurp_file(precipe, &recipe, &recipe_len) != 0) goto out;

    /* 4. extract every member into the scratch dir as "<idx>"; the pack
     * must produce exactly the announced byte count */
    if (mkdir(pmdir, 0700) != 0) goto out;
    for (i = 0; i < nmem; i++) {
        char idxbuf[16], pm[256];
        struct stat st;
        snprintf(idxbuf, sizeof idxbuf, "%u", mem[i].idx);
        snprintf(pm, sizeof pm, "%s/%u", pmdir, mem[i].idx);
        if (invfs_codec_pack_cmd(pc, INVFS_PACK_CMD_EXTRACT, pin, idxbuf,
                                 NULL, NULL, pm) != 0)
            { if (getenv("INVFS_DEBUG_PACKS")) fprintf(stderr,"[cpack] extract idx=%u failed\n", mem[i].idx); goto out; }
        if (stat(pm, &st) != 0 || (uint64_t)st.st_size != mem[i].usize)
            { if (getenv("INVFS_DEBUG_PACKS")) fprintf(stderr,"[cpack] extract idx=%u size mismatch\n", mem[i].idx); goto out; }
    }

    /* 5. the map command (ABI v1.1), when the pack has one: the FS-owned
     * binary partition of the container into recipe-blob and member ranges.
     * Parse + validate the SHAPE here; the byte-exactness proof runs
     * through the real read path after the commit below (the sources --
     * the recipe segment and the member siblings -- only exist inside the
     * volume then), replacing the rebuild exec entirely. */
    if (def->map) {
        if (invfs_codec_pack_cmd(pc, INVFS_PACK_CMD_MAP, pin, NULL,
                                 NULL, NULL, pmap) != 0)
            { if (getenv("INVFS_DEBUG_PACKS")) fprintf(stderr,"[cpack] map cmd failed\n"); goto out; }
        if (slurp_file(pmap, &mapb, &map_len) != 0) goto out;
        if (cpack_map_parse(mapb, map_len, &ments, &nments) != 0) {
            if (getenv("INVFS_DEBUG_PACKS")) fprintf(stderr,"[cpack] map parse failed\n");
            fprintf(stderr, "sweep: %s: %s: map unreadable, "
                            "decomposition abandoned\n", pc->name, name);
            goto out;
        }
        mem_sorted = cpack_members_sorted(mem, nmem);
        if (!mem_sorted) goto out;
        if (cpack_map_validate(ments, nments, (uint64_t)full_len,
                               (uint64_t)recipe_len, mem_sorted, nmem) != 0) {
            fprintf(stderr, "sweep: %s: %s: map does not partition the "
                            "container, decomposition abandoned\n",
                    pc->name, name);
            goto out;
        }
    } else {
        /* 5. GUARD (the house 1:1 invariant): rebuild from the recipe + the
         * extracted members and memcmp against the original BEFORE anything
         * reaches disk. ANY failure abandons the decomposition; no siblings
         * exist yet, so there is nothing to purge. */
        if (invfs_codec_pack_cmd(pc, INVFS_PACK_CMD_REBUILD, NULL, NULL,
                                 pmdir, precipe, pout) != 0)
            goto out;
        if (slurp_file(pout, &outb, &out_len) != 0 ||
            out_len != full_len ||
            (full_len && memcmp(outb, full, full_len) != 0)) {
            fprintf(stderr, "sweep: %s: %s: rebuild guard refused, "
                            "decomposition abandoned\n", pc->name, name);
            goto out;
        }
        free(outb);
        outb = NULL;
    }

    /* 6. commit: children first (the FLAC note) -- members (RAW, the
     * normal pipeline owns them from here), then the member table, then
     * the name-owning recipe record; retire the old record last. */
    have_keep = vol_get_meta(v, inode_id, &keep) == 0;
    for (i = 0; i < nmem; i++) {
        char pm[256], mn[320];
        uint8_t *mb = NULL;
        size_t mlen = 0;
        uint64_t pino;
        snprintf(pm, sizeof pm, "%s/%u", pmdir, mem[i].idx);
        if (slurp_file(pm, &mb, &mlen) != 0 || mlen != (size_t)mem[i].usize) {
            free(mb);
            vol_transcode_abort(v, name);
            goto out;
        }
        cpack_mbr_name(mn, sizeof mn, name, mem[i].idx, mem[i].sname);
        pino = vol_create_file(v, mn, mb, mlen);
        free(mb);
        if (!pino) {
            fprintf(stderr, "sweep: %s: member inode failed for %s\n",
                    pc->name, mn);
            vol_transcode_abort(v, name);
            goto out;
        }
        /* member meta: uid/gid/mode from the container's record */
        if (have_keep) {
            invfs_meta_pub mm = keep;
            mm.type = INVFS_ITYP_REG;
            mm.nlink = 1;
            mm.target[0] = 0;
            vol_apply_meta(v, mn, &mm);
        }
    }
    {
        char tn[288];
        snprintf(tn, sizeof tn, "%s!mbrt", name);
        if (!vol_create_file(v, tn, table, table_len)) {
            fprintf(stderr, "sweep: %s: member table inode failed (%s)\n",
                    pc->name, tn);
            vol_transcode_abort(v, name);
            goto out;
        }
    }
    if (def->map) {
        /* 6. commit, seekable (ABI v1.1): the recipe record flips the name
         * with the old RAW record still intact, the map guard then proves
         * the stored state bit-exact THROUGH THE REAL READ PATH (recipe
         * segment reads + member sibling vol_read_range -- streaming, no
         * rebuild exec), and !mbrmap lands LAST: its presence on disk is
         * the "the guard passed" marker, so a crash mid-commit can only
         * fall back to the pack's rebuild exec (or fail loudly with the
         * pack absent), never serve an unguarded map. A guard/write
         * failure rolls the name back onto the untouched old record and
         * purges the siblings, like a rebuild-guard refusal. */
        char mbn[288];
        uint64_t newino, old_pos = 0, old_ctime = 0;
        const name_index_entry *ne = idx_get(v, name, strlen(name));
        if (ne) old_ctime = ne->ctime;
        old_pos = idx_get_id(v, inode_id);

        newino = vol_create_blob_file(v, name, recipe, recipe_len,
                                      (uint64_t)full_len, pc->algo);
        if (!newino) {
            /* no space for the main record: NOT a guard refusal (the
             * vol_jxl_retry convention) -- leave unstamped, a retry
             * re-arms */
            fprintf(stderr, "sweep: %s container create failed (%s)\n",
                    pc->name, name);
            vol_transcode_abort(v, name);
            goto out;
        }
        if (cpack_map_guard(v, name, newino, full, full_len,
                            ments, nments, mem_sorted, nmem) != 0) {
            fprintf(stderr, "sweep: %s: %s: map guard refused, "
                            "decomposition abandoned\n", pc->name, name);
            vol_delete_inode(v, newino, name);
            vol_transcode_abort(v, name);
            if (old_pos)
                idx_put(v, name, strlen(name), inode_id, old_pos,
                        (uint64_t)full_len, old_ctime);
            goto out;
        }
        snprintf(mbn, sizeof mbn, "%s!mbrmap", name);
        if (!vol_create_file(v, mbn, mapb, map_len)) {
            fprintf(stderr, "sweep: %s: member map inode failed (%s)\n",
                    pc->name, mbn);
            vol_delete_inode(v, newino, name);
            vol_transcode_abort(v, name);
            if (old_pos)
                idx_put(v, name, strlen(name), inode_id, old_pos,
                        (uint64_t)full_len, old_ctime);
            goto out;
        }
        vol_delete_inode(v, inode_id, name);
        /* the fresh blob record has no ext; carry the old meta across
         * (the vol_pack_sweep flow) */
        if (have_keep) {
            invfs_meta_pub chk;
            if (vol_get_meta(v, newino, &chk) != 0)
                vol_apply_meta(v, name, &keep);
        }
        vol_stamp_class(v, newino, INVFS_CLASS_CONTAINER,
                        (uint8_t)pc->algo, pc->generation);
    } else {
        uint64_t newino = vol_create_blob_file(v, name, recipe, recipe_len,
                                               (uint64_t)full_len, pc->algo);
        if (!newino) {
            /* no space for the main record: NOT a guard refusal (the
             * vol_jxl_retry convention) -- leave unstamped, a retry
             * re-arms */
            fprintf(stderr, "sweep: %s container create failed (%s)\n",
                    pc->name, name);
            vol_transcode_abort(v, name);
            goto out;
        }
        vol_delete_inode(v, inode_id, name);
        /* the fresh blob record has no ext; carry the old meta across
         * (the vol_pack_sweep flow) */
        if (have_keep) {
            invfs_meta_pub chk;
            if (vol_get_meta(v, newino, &chk) != 0)
                vol_apply_meta(v, name, &keep);
        }
        vol_stamp_class(v, newino, INVFS_CLASS_CONTAINER,
                        (uint8_t)pc->algo, pc->generation);
    }

    /* 7. WP14b pattern: defer the fresh members into THIS run's batching
     * accumulators (the flush re-reads and re-sniffs each from its live
     * record, so a head sniff is enough here; members that sniff as
     * nothing stay RAW for the next run's generic pass). */
    {
        int n_bin = 0, n_text = 0;
        for (i = 0; i < nmem; i++) {
            char mn[320];
            uint64_t pino, fsz = 0;
            uint8_t head[8192];
            int got, bfam, tfam;
            cpack_mbr_name(mn, sizeof mn, name, mem[i].idx, mem[i].sname);
            pino = vol_find(v, mn);
            if (!pino) continue;
            got = vol_read_range(v, pino, 0, sizeof head, head);
            if (got <= 0 ||
                vol_stat_full(v, mn, NULL, &fsz, NULL) != 0 || !fsz)
                continue;
            bfam = invfs_binary_family(head, (size_t)got, mn);
            if (bfam > 0) {
                if (bz_defer(v, pino, mn, fsz, (uint32_t)bfam) == 0) n_bin++;
                continue;
            }
            tfam = invfs_text_family(mn, head, (size_t)got);
            if (tfam > 0) {
                const invfs_codec *tc = invfs_codec_by_algo(INVFS_ALGO_PPMD);
                if (tc && tc->dec_mem_bytes > vol_get_dec_mem_limit(v))
                    vol_stamp_class(v, pino, INVFS_CLASS_GENERIC_MEMLIMIT,
                                    INVFS_ALGO_PPMD, tc->generation);
                else if (tz_defer(v, pino, mn, fsz, (uint32_t)tfam) == 0)
                    n_text++;
            }
        }
        /* one summary line per container, not one per member (the
         * defer_container_parts convention) */
        if (n_bin)
            printf("  %s!*: %d parts -> ZSTD batch\n", name, n_bin);
        if (n_text)
            printf("  %s!*: %d parts -> PPMd batch\n", name, n_text);
    }
    rc = 100 + (int)pc->algo;   /* the WP13 pack rc convention */
out:
    /* scratch cleanup (member temps are named by the parsed idx set) */
    tool_rm(dir, "in");
    tool_rm(dir, "table");
    tool_rm(dir, "recipe");
    tool_rm(dir, "out");
    tool_rm(dir, "map");
    if (mem) {
        for (i = 0; i < nmem; i++) {
            char pm[256];
            snprintf(pm, sizeof pm, "%s/%u", pmdir, mem[i].idx);
            unlink(pm);
        }
    }
    rmdir(pmdir);
    rmdir(dir);
    free(mem);
    free(table);
    free(recipe);
    free(outb);
    free(mapb);
    free(ments);
    free(mem_sorted);
    return rc;
}


/* WP16a read side: rebuild the original container from the recipe blob +
 * the member siblings. Returns 0 and fills dst (want_len bytes) exactly,
 * -1 on any failure (loud: a missing/corrupt member, a bad table, or a
 * pack error all mean the file cannot be served). */
int pack_container_rebuild(invfs_volume *v, const invfs_codec *pc,
                                  const char *name,
                                  const uint8_t *recipe, size_t recipe_len,
                                  uint8_t *dst, size_t want_len)
{
#ifdef _WIN32
    (void)v; (void)pc; (void)name; (void)recipe; (void)recipe_len;
    (void)dst; (void)want_len;
    return -1;   /* the POSIX tool layer does not exist on Windows */
#else
    char dir[64], precipe[128], pout[128], pmdir[192], tn[288];
    uint8_t *table = NULL, *outb = NULL;
    size_t table_len = 0, out_len = 0;
    cpack_member *mem = NULL;
    size_t nmem = 0, i;
    int made = 0, rc = -1;

    snprintf(tn, sizeof tn, "%s!mbrt", name);
    {
        uint64_t tino = vol_find(v, tn);
        if (!tino || vol_read_file(v, tino, &table, &table_len) != 0) {
            fprintf(stderr, "%s: member table '%s' unreadable\n",
                    pc->name, tn);
            return -1;
        }
    }
    if (cpack_parse_table(table, table_len, &mem, &nmem, NULL) != 0 ||
        nmem == 0) {
        fprintf(stderr, "%s: member table '%s' corrupt\n", pc->name, tn);
        goto out;
    }
    if (tool_tmpdir(dir, sizeof dir) != 0) goto out;
    made = 1;
    snprintf(precipe, sizeof precipe, "%s/recipe", dir);
    snprintf(pout, sizeof pout, "%s/out", dir);
    snprintf(pmdir, sizeof pmdir, "%s/mbr", dir);
    if (tool_write(precipe, recipe, recipe_len) != 0) goto out;
    if (mkdir(pmdir, 0700) != 0) goto out;
    for (i = 0; i < nmem; i++) {
        char mn[320], pm[256];
        uint64_t pino;
        uint8_t *mb = NULL;
        size_t mlen = 0;
        cpack_mbr_name(mn, sizeof mn, name, mem[i].idx, mem[i].sname);
        pino = vol_find(v, mn);
        if (!pino || vol_read_file(v, pino, &mb, &mlen) != 0) {
            fprintf(stderr, "%s: member '%s' unreadable\n", pc->name, mn);
            free(mb);
            goto out;
        }
        if (mlen != (size_t)mem[i].usize) {
            fprintf(stderr, "%s: member '%s' corrupt (got %zu, want %llu)\n",
                    pc->name, mn, mlen, (unsigned long long)mem[i].usize);
            free(mb);
            goto out;
        }
        snprintf(pm, sizeof pm, "%s/%u", pmdir, mem[i].idx);
        if (tool_write(pm, mb, mlen) != 0) { free(mb); goto out; }
        free(mb);
    }
    if (invfs_codec_pack_cmd(pc, INVFS_PACK_CMD_REBUILD, NULL, NULL,
                             pmdir, precipe, pout) != 0) {
        fprintf(stderr, "%s: rebuild failed for %s\n", pc->name, name);
        goto out;
    }
    if (slurp_file(pout, &outb, &out_len) != 0 || out_len != want_len) {
        fprintf(stderr, "%s: rebuild of %s produced %zu bytes, want %zu\n",
                pc->name, name, out_len, want_len);
        goto out;
    }
    if (want_len) memcpy(dst, outb, want_len);
    rc = 0;
out:
    if (made) {
        tool_rm(dir, "recipe");
        tool_rm(dir, "out");
        if (mem) {
            for (i = 0; i < nmem; i++) {
                char pm[256];
                snprintf(pm, sizeof pm, "%s/%u", pmdir, mem[i].idx);
                unlink(pm);
            }
        }
        rmdir(pmdir);
        rmdir(dir);
    }
    free(mem);
    free(table);
    free(outb);
    return rc;
#endif
}
