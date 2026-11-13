/*
 * libn200fs.so -- runtime data-root redirection for com.npnd.n200.
 *
 * WHY THIS EXISTS
 * ---------------
 * The app hard-codes one data root, /mnt/extSdCard, for BOTH the map database
 * it reads and the state files it writes, in 288 places across 13 native
 * libraries. On a stock Android 4.4 phone neither half works:
 *
 *   - /mnt/extSdCard is not where KitKat mounts a card. Samsung puts it at
 *     /storage/extSdCard, other vendors at /storage/sdcard1, /storage/ext_sd,
 *     /storage/external_SD ... there is no single right answer to compile in.
 *   - Since KitKat a third-party app may not write anywhere on SECONDARY
 *     storage outside Android/data/<pkg>/. So even a correct card path cannot
 *     take the writes.
 *
 * Rewriting the strings in the .so files cannot fix this. The replacement has
 * to be byte-for-byte no longer than the original or every file offset in the
 * ELF moves, and "/mnt/extSdCard" is 14 bytes against "/storage/extSdCard"'s
 * 18. There is no 14-byte path to a KitKat SD card.
 *
 * So instead of rewriting the paths, redirect them when they are used. This
 * library is injected as the first DT_NEEDED of every app library that calls
 * libc's file entry points (see tools/inject_needed.py), which on the pre-N
 * dynamic linker makes those libraries bind fopen/open/stat/... here rather
 * than to libc. Each call maps the hard-coded root onto a root that works:
 *
 *      read   /mnt/extSdCard///Navkingdom/Common/DB_MAP/x.sti
 *          -> /storage/extSdCard/Navkingdom/Common/DB_MAP/x.sti     (the card)
 *      write  /mnt/extSdCard///Navkingdom/LoggerServerPID
 *          -> /sdcard/n200dt/Navkingdom/LoggerServerPID        (primary storage)
 *
 * The split is decided per call from the open mode, not from a list of paths,
 * so no write path can be missed -- and a missed write path is what the
 * original tombstone was: fopen() returned NULL, the result was never checked,
 * and fprintf dereferenced it.
 *
 * Reads are an overlay: the write root is tried first so that state the app
 * has written wins, then the card. Parent directories are created on demand,
 * because the app assumes its tree already exists and never calls mkdir for
 * most of it.
 *
 * The card is found at runtime by looking for a Navkingdom directory under a
 * list of known mount points, so the same APK works on devices that name the
 * card differently. n200fs.conf overrides it when the guess is wrong.
 *
 * DELIBERATELY FREESTANDING
 * -------------------------
 * No libc headers are included and no kernel structure is ever dereferenced:
 * struct stat, struct statfs, DIR and FILE are passed through as opaque
 * pointers. That is what makes this safe to build without an NDK and safe to
 * run against any 4.4 bionic -- there is no layout to get wrong.
 *
 * The real entry points are fetched with dlsym rather than called by name, so
 * nothing here can recurse into itself.
 */

typedef unsigned int   u32;
typedef unsigned long  ulong;

/* ---- imports ---------------------------------------------------------- */
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
char *getenv(const char *);
int   __android_log_print(int, const char *, const char *, ...);

/* Bionic's values. Overridable only so that shim/selftest.sh can exercise this
 * file against glibc on a build machine, where all three differ. */
#ifndef N200FS_LIBC
#define N200FS_LIBC    "libc.so"
#endif
#ifndef RTLD_NOW
#define RTLD_NOW       0
#endif
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT   ((void *)0xffffffff)
#endif

#define LOGI(...) __android_log_print(4, "n200fs", __VA_ARGS__)
#define LOGE(...) __android_log_print(6, "n200fs", __VA_ARGS__)

/* open(2) flags. Generic Linux values; identical on every Android ARM ABI. */
#define O_ACCMODE 00000003
#define O_CREAT   00000100
#define O_TRUNC   00001000
#define O_APPEND  00002000

/* The root the app was built with. Left in place in the .so files on purpose:
 * it is the sentinel this library keys on. */
static const char PFX[]  = "/mnt/extSdCard";
#define PFX_LEN 14          /* strlen(PFX) */

#define PATHMAX 640
#define CMDMAX  1024

/* ---- tiny string helpers (no libc dependency) ------------------------- */

static int s_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int s_ncmp(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y) return (int)x - (int)y;
        if (!x) return 0;
    }
    return 0;
}

static char *s_chr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return 0;
}

/* Append src to dst (which holds *len bytes) if it fits. Returns 0 on
 * overflow, and leaves dst unusable -- every caller checks. */
static int s_cat(char *dst, int cap, int *len, const char *src)
{
    int i = 0;
    while (src[i]) {
        if (*len + 1 >= cap) return 0;
        dst[(*len)++] = src[i++];
    }
    dst[*len] = 0;
    return 1;
}

static int s_copy(char *dst, int cap, const char *src)
{
    int n = 0;
    dst[0] = 0;
    return s_cat(dst, cap, &n, src);
}

/* Trim trailing '/' and whitespace, in place. */
static void s_trim(char *s)
{
    int n = s_len(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t' ||
            (c == '/' && n > 1)) {
            s[--n] = 0;
        } else {
            break;
        }
    }
}

/* ---- the real libc entry points --------------------------------------- */

static void *(*r_fopen)(const char *, const char *);
static int   (*r_open)(const char *, int, int);
static int   (*r_stat)(const char *, void *);
static int   (*r_access)(const char *, int);
static void *(*r_opendir)(const char *);
static int   (*r_closedir)(void *);
static int   (*r_mkdir)(const char *, u32);
static int   (*r_rmdir)(const char *);
static int   (*r_unlink)(const char *);
static int   (*r_remove)(const char *);
static int   (*r_rename)(const char *, const char *);
static int   (*r_utimes)(const char *, const void *);
static int   (*r_statfs)(const char *, void *);
static int   (*r_system)(const char *);

static char g_data[PATHMAX];     /* read root  -- holds Navkingdom/        */
static char g_write[PATHMAX];    /* write root -- always writable          */
static volatile int g_ready;     /* 1 once both roots are usable           */
static volatile int g_busy;      /* re-entry guard for the resolve step    */

/* Mount points to look for the card at, best guess first. The app's own
 * /mnt/extSdCard is included: on a head unit, and on some older builds, it
 * really is the mount point. */
static const char *const CANDIDATES[] = {
    "/storage/extSdCard",
    "/storage/extsdcard",
    "/storage/ExtSdCard",
    "/storage/sdcard1",
    "/storage/external_SD",
    "/storage/externalSD",
    "/storage/external_sd",
    "/storage/ext_sd",
    "/storage/MicroSD",
    "/storage/microsd",
    "/storage/sdcard-ext",
    "/storage/removable/sdcard1",
    "/storage/usbcard1",
    "/mnt/extSdCard",
    "/mnt/external_sd",
    "/mnt/ext_sd",
    "/mnt/sdcard/external_sd",
    "/mnt/sdcard2",
    "/mnt/media_rw/extSdCard",
    "/mnt/media_rw/sdcard1",
    0
};

static int is_dir(const char *p)
{
    void *d;
    if (!r_opendir) return 0;
    d = r_opendir(p);
    if (!d) return 0;
    if (r_closedir) r_closedir(d);
    return 1;
}

/* Join root + "/" + rest into buf. Returns 0 if it would not fit. */
static int join(char *buf, int cap, const char *root, const char *rest)
{
    int n = 0;
    if (!s_cat(buf, cap, &n, root)) return 0;
    if (rest[0]) {
        if (!s_cat(buf, cap, &n, "/")) return 0;
        if (!s_cat(buf, cap, &n, rest)) return 0;
    }
    return 1;
}

/* mkdir -p for every directory component of a file path. */
static void make_parents(const char *path)
{
    char tmp[PATHMAX];
    int i;
    if (!r_mkdir) return;
    if (!s_copy(tmp, sizeof tmp, path)) return;
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        r_mkdir(tmp, 0775);
        tmp[i] = '/';
    }
}

/* ---- configuration ----------------------------------------------------- */

/* Read "data=" / "write=" from a n200fs.conf, if one is there. Hand-parsed
 * so that no stdio call can re-enter this library. */
static void read_conf(const char *path)
{
    char buf[1024];
    int fd, n, i, start;
    if (!r_open) return;
    fd = r_open(path, 0 /* O_RDONLY */, 0);
    if (fd < 0) return;
    {
        int (*r_read)(int, void *, u32) =
            (int (*)(int, void *, u32))dlsym(RTLD_DEFAULT, "read");
        int (*r_close)(int) = (int (*)(int))dlsym(RTLD_DEFAULT, "close");
        n = r_read ? r_read(fd, buf, (u32)(sizeof buf - 1)) : -1;
        if (r_close) r_close(fd);
    }
    if (n <= 0) return;
    buf[n] = 0;
    LOGI("reading %s", path);

    start = 0;
    for (i = 0; i <= n; i++) {
        char line[PATHMAX];
        int len;
        if (buf[i] != '\n' && buf[i] != 0) continue;
        len = i - start;
        if (len > 0 && len < (int)sizeof line) {
            int k;
            for (k = 0; k < len; k++) line[k] = buf[start + k];
            line[len] = 0;
            s_trim(line);
            if (line[0] != '#') {
                if (s_ncmp(line, "data=", 5) == 0 && line[5])
                    s_copy(g_data, sizeof g_data, line + 5);
                else if (s_ncmp(line, "write=", 6) == 0 && line[6])
                    s_copy(g_write, sizeof g_write, line + 6);
            }
        }
        start = i + 1;
    }
    s_trim(g_data);
    s_trim(g_write);
}

/* Decide where reads and writes go. */
static void pick_roots(void)
{
    char buf[PATHMAX];
    const char *primary;
    int i;

    primary = getenv("EXTERNAL_STORAGE");
    if (!primary || !primary[0]) primary = "/sdcard";

    /* Writes default to a folder on primary storage: writable with the
     * WRITE_EXTERNAL_STORAGE the app already declares, and reachable from any
     * file manager on the phone. */
    if (!g_write[0]) {
        int n = 0;
        g_write[0] = 0;
        if (!s_cat(g_write, sizeof g_write, &n, primary) ||
            !s_cat(g_write, sizeof g_write, &n, "/n200dt"))
            s_copy(g_write, sizeof g_write, "/sdcard/n200dt");
    }
    make_parents(g_write);
    if (r_mkdir) r_mkdir(g_write, 0775);

    if (g_data[0]) return;          /* set from n200fs.conf */

    /* Look for the map tree. A root "contains Navkingdom" either directly or
     * one level down in n200dt, which is where older repacks of this APK put
     * it. */
    for (i = 0; CANDIDATES[i]; i++) {
        if (join(buf, sizeof buf, CANDIDATES[i], "Navkingdom") && is_dir(buf)) {
            s_copy(g_data, sizeof g_data, CANDIDATES[i]);
            return;
        }
        if (join(buf, sizeof buf, CANDIDATES[i], "n200dt/Navkingdom") &&
            is_dir(buf)) {
            int n = 0;
            g_data[0] = 0;
            s_cat(g_data, sizeof g_data, &n, CANDIDATES[i]);
            s_cat(g_data, sizeof g_data, &n, "/n200dt");
            return;
        }
    }
    /* Nothing on a card: fall back to primary storage, which is what a build
     * without this library would have used anyway. */
    if (join(buf, sizeof buf, primary, "n200dt/Navkingdom") && is_dir(buf)) {
        int n = 0;
        g_data[0] = 0;
        s_cat(g_data, sizeof g_data, &n, primary);
        s_cat(g_data, sizeof g_data, &n, "/n200dt");
        return;
    }
    if (join(buf, sizeof buf, primary, "Navkingdom") && is_dir(buf)) {
        s_copy(g_data, sizeof g_data, primary);
        return;
    }
    s_copy(g_data, sizeof g_data, g_write);
}

static void resolve(void)
{
    void *c;
    char conf[PATHMAX];
    const char *primary;

    if (g_ready || g_busy) return;
    g_busy = 1;

    /* libc.so is already in the process, so this binds to the loaded copy and
     * touches no file. RTLD_DEFAULT is the fallback: the linker walks the
     * solist from the start, where libc sits long before this library. */
    c = dlopen(N200FS_LIBC, RTLD_NOW);
#define GET(f) do { \
        r_##f = (void *)dlsym(c ? c : RTLD_DEFAULT, #f); \
        if (!r_##f && c) r_##f = (void *)dlsym(RTLD_DEFAULT, #f); \
    } while (0)
    GET(fopen); GET(open); GET(stat); GET(access); GET(opendir);
    GET(closedir); GET(mkdir); GET(rmdir); GET(unlink); GET(remove);
    GET(rename); GET(utimes); GET(statfs); GET(system);
#undef GET

    if (!r_fopen || !r_open) {
        LOGE("could not resolve libc; paths will NOT be redirected");
        g_busy = 0;
        return;
    }

    primary = getenv("EXTERNAL_STORAGE");
    if (!primary || !primary[0]) primary = "/sdcard";
    if (join(conf, sizeof conf, primary, "n200dt/n200fs.conf"))
        read_conf(conf);
    if (!g_data[0] && join(conf, sizeof conf, primary, "n200fs.conf"))
        read_conf(conf);

    pick_roots();

    LOGI("read  root: %s", g_data);
    LOGI("write root: %s", g_write);
    LOGI("%s -> the two roots above", PFX);

    g_ready = 1;
    g_busy = 0;
}

/* ---- path mapping ------------------------------------------------------ */

/* Does p start with the app's hard-coded root, as a whole path component? */
static int strip_root(const char *p, const char **rest)
{
    const char *q = p;
    if (!q) return 0;
    if (s_ncmp(q, PFX, PFX_LEN) != 0) return 0;
    q += PFX_LEN;
    if (*q && *q != '/') return 0;      /* /mnt/extSdCardFoo is not ours */
    while (*q == '/') q++;              /* the app writes /// a lot */
    *rest = q;
    return 1;
}

/*
 * Map one path. want_write picks the root outright; for reads the write root
 * is consulted first so anything the app has already written wins over the
 * pristine copy on the card.
 *
 * Returns p itself when there is nothing to do, so a non-matching path costs
 * one string compare and no copying.
 */
static const char *map_path(const char *p, int want_write, char *buf, int cap)
{
    const char *rest;
    if (!p) return p;
    if (s_ncmp(p, PFX, PFX_LEN) != 0) return p;   /* fast path */
    resolve();
    if (!g_ready) return p;
    if (!strip_root(p, &rest)) return p;

    if (want_write) {
        if (!join(buf, cap, g_write, rest)) return p;
        make_parents(buf);
        return buf;
    }
    if (!join(buf, cap, g_write, rest)) return p;
    if (r_access && r_access(buf, 0 /* F_OK */) == 0) return buf;
    if (!join(buf, cap, g_data, rest)) return p;
    return buf;
}

/* ---- the interposed entry points --------------------------------------- */

void *fopen(const char *path, const char *mode)
{
    char buf[PATHMAX];
    int w;
    resolve();
    if (!r_fopen) return 0;
    w = mode && (mode[0] != 'r' || s_chr(mode, '+') != 0);
    return r_fopen(map_path(path, w, buf, sizeof buf), mode);
}

int open(const char *path, int flags, ...)
{
    char buf[PATHMAX];
    int mode = 0, w;
    resolve();
    if (!r_open) return -1;
    if (flags & O_CREAT) {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }
    w = (flags & O_ACCMODE) != 0 || (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0;
    return r_open(map_path(path, w, buf, sizeof buf), flags, mode);
}

int stat(const char *path, void *st)
{
    char buf[PATHMAX];
    resolve();
    if (!r_stat) return -1;
    return r_stat(map_path(path, 0, buf, sizeof buf), st);
}

int access(const char *path, int amode)
{
    char buf[PATHMAX];
    resolve();
    if (!r_access) return -1;
    /* W_OK is 2: an access(W_OK) check is about to be followed by a write. */
    return r_access(map_path(path, (amode & 2) != 0, buf, sizeof buf), amode);
}

void *opendir(const char *path)
{
    char buf[PATHMAX];
    resolve();
    if (!r_opendir) return 0;
    return r_opendir(map_path(path, 0, buf, sizeof buf));
}

int mkdir(const char *path, u32 mode)
{
    char buf[PATHMAX];
    resolve();
    if (!r_mkdir) return -1;
    return r_mkdir(map_path(path, 1, buf, sizeof buf), mode);
}

int rmdir(const char *path)
{
    char buf[PATHMAX];
    resolve();
    if (!r_rmdir) return -1;
    return r_rmdir(map_path(path, 1, buf, sizeof buf));
}

int unlink(const char *path)
{
    char buf[PATHMAX];
    resolve();
    if (!r_unlink) return -1;
    return r_unlink(map_path(path, 1, buf, sizeof buf));
}

int remove(const char *path)
{
    char buf[PATHMAX];
    resolve();
    if (!r_remove) return -1;
    return r_remove(map_path(path, 1, buf, sizeof buf));
}

int rename(const char *from, const char *to)
{
    char a[PATHMAX], b[PATHMAX];
    const char *fa, *fb;
    resolve();
    if (!r_rename) return -1;
    fa = map_path(from, 1, a, sizeof a);
    fb = map_path(to, 1, b, sizeof b);
    return r_rename(fa, fb);
}

int utimes(const char *path, const void *times)
{
    char buf[PATHMAX];
    resolve();
    if (!r_utimes) return -1;
    return r_utimes(map_path(path, 1, buf, sizeof buf), times);
}

int statfs(const char *path, void *sb)
{
    char buf[PATHMAX];
    resolve();
    if (!r_statfs) return -1;
    /* Asked for free space: report the filesystem the writes land on. */
    return r_statfs(map_path(path, 0, buf, sizeof buf), sb);
}

/*
 * system() is handed whole shell commands with the root embedded in them --
 * "rm -rf /mnt/extSdCard///Navkingdom/Persistence/locus.bak",
 * "cat /proc/meminfo |grep Cached > /mnt/extSdCard///Navkingdom/meminfo.txt".
 * Every one of them writes, so they all go to the write root. Rewrite the
 * command text rather than a path.
 */
int system(const char *cmd)
{
    char out[CMDMAX];
    int starts[16];                 /* where each rewritten path begins */
    int nstarts = 0;
    int n = 0;
    const char *p = cmd;
    int i;

    resolve();
    if (!r_system) return -1;
    if (!cmd || !g_ready) return r_system(cmd);

    out[0] = 0;
    while (*p) {
        if (s_ncmp(p, PFX, PFX_LEN) == 0 &&
            (p[PFX_LEN] == 0 || p[PFX_LEN] == '/')) {
            if (nstarts < 16) starts[nstarts++] = n;
            if (!s_cat(out, sizeof out, &n, g_write)) return r_system(cmd);
            p += PFX_LEN;
            while (p[0] == '/' && p[1] == '/') p++;   /* collapse /// to / */
            continue;
        }
        if (n + 1 >= (int)sizeof out) return r_system(cmd);
        out[n++] = *p++;
        out[n] = 0;
    }

    /* `rm -rf` of a missing path is harmless, but a `>` redirection into a
     * missing directory is not -- and the app never creates these. Create the
     * parents of each path we rewrote. Each one ends at the first shell
     * metacharacter or whitespace. */
    for (i = 0; i < nstarts; i++) {
        char tok[PATHMAX];
        int s = starts[i], k = 0;
        while (out[s + k] && k + 1 < (int)sizeof tok) {
            char c = out[s + k];
            if (c == ' ' || c == '\t' || c == '"' || c == '\'' ||
                c == '>' || c == '<' || c == '|' || c == ';' || c == '&')
                break;
            tok[k] = c;
            k++;
        }
        tok[k] = 0;
        if (k) make_parents(tok);
    }
    return r_system(out);
}

/*
 * Pick the roots at load time. The linker runs this from within the dlopen
 * that pulled this library in, while the process is still single-threaded as
 * far as the app's own code is concerned, so every later call sees g_ready
 * already set and no thread can race the first mapping. The lazy resolve() at
 * the head of each entry point stays as a backstop.
 */
__attribute__((constructor))
static void n200fs_init(void)
{
    resolve();
}
