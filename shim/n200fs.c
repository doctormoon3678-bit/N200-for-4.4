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
int   __android_log_print(int, const char *, const char *, ...);   /* defined below */

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

/* Defined at the end of the file, used before it. */
static void gps_start(void);
static int  parse_gps(const char *v);
extern int  g_sdstate;
extern int  g_force;
extern int  g_trace;
extern int  g_trace_all;
static int  is_gps_fifo(const char *p);
static int  gps_open_pipe(const char *path);
static void *(*r_fdopen)(int, const char *);
static int  is_gps_fifo(const char *p);
static int  gps_open_pipe(const char *path);
static void *(*r_fdopen)(int, const char *);

/* open(2) flags. Generic Linux values; identical on every Android ARM ABI. */
#define O_ACCMODE 00000003
#define O_CREAT   00000100
#define O_TRUNC   00001000
#define O_APPEND  00002000

/*
 * The roots the app was built with. Left in place in the .so files on
 * purpose: they are the sentinels this library keys on.
 *
 * /mnt/extSdCard is the one in the ELF strings, 288 times. The others only
 * show up at runtime, which is why they were missed at first: the engine
 * builds them from a root it gets elsewhere, and a device log caught it
 * opening /storage/sdcard0/Navkingdom/Persistence/... and
 * /storage/sdcard1/Navkingdom/Common/DB_MAP/map_version.ini. sdcard1 is the
 * removable card, which on a Samsung is /storage/extSdCard, so every one of
 * those reads failed.
 *
 * "Storage Card" is older still -- this engine began on Windows CE, and the
 * backslash spellings below are in the binaries verbatim.
 *
 * need_nk limits the two /storage roots to paths under Navkingdom/, so a
 * device where /storage/sdcard0 is real and holds something else is left
 * alone.
 */
struct root { const char *p; int need_nk; };
static const struct root ROOTS[] = {
    { "/mnt/extSdCard",   0 },
    { "/storage/sdcard0", 1 },
    { "/storage/sdcard1", 1 },
    { "/Storage Card2",   0 },
    { "/Storage Card",    0 },
    /* The head unit's "navigation is up" node. There is nothing listening on
     * a phone, but the engine's write to it failed, and a readiness signal
     * that cannot be written is the kind of thing a state machine waits on.
     * Redirected so it succeeds. */
    { "/navis/navis-onoff", 0 },
    { 0, 0 }
};

/* The one that goes in the log banner and that system() rewrites. */
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
static int   (*r_read)(int, void *, u32);
static int   (*r_write)(int, const void *, u32);
static int   (*r_close)(int);

static char g_data[PATHMAX];     /* read root  -- holds Navkingdom/        */
static char g_write[PATHMAX];    /* write root -- always writable          */
static char g_mounts[512];       /* storage mount points, for the log      */
static int  g_enginelog = 1;     /* switch on the engine's own log file     */
static volatile int g_ready;     /* 1 once both roots are usable           */
static volatile int g_busy;      /* re-entry guard for the resolve step    */

/*
 * Fallback mount points, used after /proc/mounts has been searched. These are
 * worth keeping even though the mount table is authoritative, because a card
 * is often reachable through a symlink or an alias that is not itself a mount
 * -- /mnt/extsdcard pointing at /storage/extSdCard, say -- and such a name
 * never appears in /proc/mounts.
 *
 * Case matters here: these are real directory names in a tmpfs, not entries on
 * the card's own case-insensitive filesystem, so extSdCard and extsdcard are
 * two different paths and both have shipped on real devices.
 */
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
    "/storage/removable/microsd",
    "/storage/usbcard1",
    "/mnt/extsdcard",
    "/mnt/extSdCard",
    "/mnt/ExtSdCard",
    "/mnt/external_sd",
    "/mnt/externalSD",
    "/mnt/ext_sd",
    "/mnt/sdcard1",
    "/mnt/sdcard2",
    "/mnt/sdcard-ext",
    "/mnt/sdcard/external_sd",
    "/mnt/sdcard/extStorages/SdCard",
    "/mnt/emmc",
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

/* Read a whole small file. Returns the byte count, or -1. Uses the raw
 * descriptor calls, not stdio, so nothing here can re-enter this library. */
static int read_file(const char *path, char *buf, int cap)
{
    int fd, n, total = 0;
    if (!r_open || !r_read) return -1;
    fd = r_open(path, 0 /* O_RDONLY */, 0);
    if (fd < 0) return -1;
    /* /proc files do not report a size and can come back in short reads. */
    while (total < cap - 1) {
        n = r_read(fd, buf + total, (u32)(cap - 1 - total));
        if (n <= 0) break;
        total += n;
    }
    if (r_close) r_close(fd);
    buf[total] = 0;
    return total;
}

/* Read "data=" / "write=" from a n200fs.conf, if one is there. */
static void read_conf(const char *path)
{
    char buf[1024];
    int n, i, start;
    n = read_file(path, buf, (int)sizeof buf);
    if (n <= 0) return;
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
                else if (s_ncmp(line, "enginelog=", 10) == 0)
                    g_enginelog = line[10] != '0';
                else if (s_ncmp(line, "gps=", 4) == 0)
                    parse_gps(line + 4);
                else if (s_ncmp(line, "sdstate=", 8) == 0)
                    g_sdstate = line[8] != '0' && s_ncmp(line + 8, "off", 3) != 0;
                else if (s_ncmp(line, "force=", 6) == 0)
                    g_force = line[6] != '0' && s_ncmp(line + 6, "off", 3) != 0;
                else if (s_ncmp(line, "trace=", 6) == 0) {
                    g_trace_all = s_ncmp(line + 6, "all", 3) == 0;
                    g_trace = line[6] != '0' && s_ncmp(line + 6, "off", 3) != 0;
                }
            }
        }
        start = i + 1;
    }
    s_trim(g_data);
    s_trim(g_write);
}

/*
 * Is this a real map tree, or just a directory that happens to be called
 * Navkingdom?
 *
 * The difference matters more than it looks. Latching onto an empty or
 * half-deleted Navkingdom means every read resolves to a file that is not
 * there, fopen returns NULL, and the app -- which never checks -- dereferences
 * it. That is the exact crash this whole project exists to fix, so the test
 * asks for the two directories the engine cannot start without.
 */
static int tree_ok(const char *nk)
{
    char buf[PATHMAX];
    if (join(buf, sizeof buf, nk, "Navi") && is_dir(buf)) return 1;
    if (join(buf, sizeof buf, nk, "Common") && is_dir(buf)) return 1;
    return 0;
}

/*
 * layout 0: <root>/Navkingdom          -- a card, prepared by hand
 * layout 1: <root>/n200dt/Navkingdom   -- what older repacks of this APK told
 *                                         people to create on INTERNAL storage
 *
 * The two are searched in separate passes, never together, because a leftover
 * layout-1 tree on internal storage must never be mistaken for a card.
 */
static int try_root(const char *root, int layout)
{
    char buf[PATHMAX];
    char cand[PATHMAX];
    int n = 0;

    if (!root || !root[0]) return 0;
    cand[0] = 0;
    if (!s_cat(cand, sizeof cand, &n, root)) return 0;
    if (layout == 1 && !s_cat(cand, sizeof cand, &n, "/n200dt")) return 0;
    if (!join(buf, sizeof buf, cand, "Navkingdom")) return 0;
    if (!is_dir(buf)) return 0;
    if (!tree_ok(buf)) {
        LOGI("%s exists but holds no Navi/ or Common/; ignoring it", buf);
        return 0;
    }
    s_copy(g_data, sizeof g_data, cand);
    return 1;
}

/*
 * Every name primary storage answers to.
 *
 * KitKat mounts the built-in storage at several paths at once --
 * /storage/emulated/0, /storage/emulated/legacy, /mnt/shell/emulated/0,
 * /storage/sdcard0 -- and EXTERNAL_STORAGE names only one of them. Comparing
 * against that one variable is not enough: the others look like separate
 * filesystems in /proc/mounts, so a search for "anything that is not primary
 * storage" happily picks internal storage under a second name, finds the
 * leftover n200dt tree there, and calls it the card.
 */
static int is_primary_alias(const char *mp, const char *primary)
{
    static const char *const P[] = {
        "/sdcard", "/mnt/sdcard", "/storage/sdcard0", "/storage/self/primary", 0
    };
    int i;
    if (s_ncmp(mp, primary, s_len(primary)) == 0 &&
        s_len(mp) == s_len(primary))
        return 1;
    if (s_ncmp(mp, "/storage/emulated", 17) == 0) return 1;
    if (s_ncmp(mp, "/mnt/shell/emulated", 19) == 0) return 1;
    for (i = 0; P[i]; i++)
        if (s_ncmp(mp, P[i], s_len(P[i])) == 0 && s_len(mp) == s_len(P[i]))
            return 1;
    return 0;
}

/*
 * Search the mount table for the card.
 *
 * This is what makes the library work on a device nobody anticipated. A card
 * is a real filesystem, so whatever the vendor decided to call its mount point
 * -- /storage/extSdCard, /mnt/extsdcard, /storage/sdcard1 -- it is listed in
 * /proc/mounts, and a list of guessed names is not needed to find it.
 *
 * Parsed by hand from the raw bytes: field 2 of each line is the mount point.
 * Nothing but open/read/close is involved, so there is no structure layout to
 * get wrong and no stdio call that could re-enter this library.
 *
 * Mount points are also logged, because when this fails they are the one thing
 * needed to say why.
 */
static int scan_mounts(const char *primary, int probe, int layout)
{
    char buf[8192];
    int listn = 0;
    int n, i, found = 0;
    int pass;

    n = read_file("/proc/mounts", buf, (int)sizeof buf);
    if (n <= 0) {
        s_copy(g_mounts, sizeof g_mounts, "(/proc/mounts unreadable)");
        LOGI("/proc/mounts is unreadable; falling back to known mount points");
        return 0;
    }
    g_mounts[0] = 0;

    /* Pass 0 takes anything that is not primary storage under any of its
     * names, so a card wins over the built-in storage even when both carry a
     * Navkingdom. Pass 1 then allows primary. */
    for (pass = 0; pass < 2 && !found; pass++) {
        int start = 0;
        for (i = 0; i <= n; i++) {
            char mp[PATHMAX];
            int k = 0, field = 0, j;
            if (buf[i] != '\n' && buf[i] != 0) continue;
            /* skip to field 2 (the mount point) */
            for (j = start; j < i && field < 1; j++)
                if (buf[j] == ' ') field++;
            for (; j < i && buf[j] != ' ' && k + 1 < (int)sizeof mp; j++) {
                /* /proc/mounts escapes a space as \040 */
                if (buf[j] == '\\' && j + 3 < i && buf[j + 1] == '0' &&
                    buf[j + 2] == '4' && buf[j + 3] == '0') {
                    mp[k++] = ' ';
                    j += 3;
                } else {
                    mp[k++] = buf[j];
                }
            }
            mp[k] = 0;
            start = i + 1;

            /* Only storage-shaped mount points are worth a syscall. */
            if (s_ncmp(mp, "/storage", 8) != 0 && s_ncmp(mp, "/mnt", 4) != 0)
                continue;
            /* Listed before the primary-storage filter, so the log shows
             * every storage mount the device has -- including the aliases
             * that are skipped, since mistaking one of those for a card is
             * exactly the failure this list is here to diagnose. */
            if (pass == 0 && listn + k + 4 < (int)sizeof g_mounts) {
                if (listn) s_cat(g_mounts, sizeof g_mounts, &listn, " ");
                s_cat(g_mounts, sizeof g_mounts, &listn, mp);
                if (is_primary_alias(mp, primary))
                    s_cat(g_mounts, sizeof g_mounts, &listn, "(internal)");
            }

            if (pass == 0 && is_primary_alias(mp, primary))
                continue;
            if (probe && try_root(mp, layout)) {
                found = 1;
                break;
            }
        }
    }
    if (g_mounts[0]) LOGI("mounted storage: %s", g_mounts);
    return found;
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

    if (g_data[0]) {                     /* set from n200fs.conf */
        scan_mounts(primary, 0, 0);      /* collect the list for the log only */
        if (join(buf, sizeof buf, g_data, "Navkingdom") && !is_dir(buf))
            LOGE("n200fs.conf says data=%s but %s is not a directory",
                 g_data, buf);
        return;
    }

    /*
     * Order matters, and the reason is the whole point of this mode.
     *
     * A card prepared by hand holds <card>/Navkingdom. Internal storage, on a
     * phone that ran an older build of this repack, holds
     * /sdcard/n200dt/Navkingdom -- very often an incomplete copy, since
     * running out of room there is what sent people to a card in the first
     * place. Searching both layouts together lets that leftover win on a
     * device where primary storage is mounted under a second name, and then
     * every read resolves into a half-populated tree.
     *
     * So: real cards first, in both layouts, and only then internal storage.
     */
    if (scan_mounts(primary, 1, 0)) return;       /* <card>/Navkingdom      */
    for (i = 0; CANDIDATES[i]; i++)               /* symlinked cards, which */
        if (try_root(CANDIDATES[i], 0)) return;   /* are not mounts         */
    if (scan_mounts(primary, 1, 1)) return;       /* <card>/n200dt/...      */
    for (i = 0; CANDIDATES[i]; i++)
        if (try_root(CANDIDATES[i], 1)) return;

    /* Nothing on a card: fall back to primary storage, which is what a build
     * without this library would have used anyway. */
    if (try_root(primary, 0)) return;
    if (try_root(primary, 1)) return;

    LOGE("no Navkingdom directory found on any mounted storage.");
    LOGE("Put the map tree at <card>/Navkingdom, or name the card yourself:");
    LOGE("  echo data=/your/card > %s/n200fs.conf", g_write);
    s_copy(g_data, sizeof g_data, g_write);
}

/*
 * Leave the same three facts in a file next to the data, as well as in
 * logcat. Whoever is installing this is as likely to be holding the phone with
 * a file manager open as to have adb on a desktop, and when the card is not
 * found the mount list is the one thing needed to say why.
 */
static void write_log(void)
{
    char path[PATHMAX], out[2048];
    int fd, n = 0;

    if (!r_open || !r_write) return;
    if (!join(path, sizeof path, g_write, "n200fs.log")) return;

    out[0] = 0;
    s_cat(out, sizeof out, &n, "n200fs\nredirecting ");
    s_cat(out, sizeof out, &n, PFX);
    s_cat(out, sizeof out, &n, "\n  reads  -> ");
    s_cat(out, sizeof out, &n, g_data);
    s_cat(out, sizeof out, &n, "\n  writes -> ");
    s_cat(out, sizeof out, &n, g_write);
    s_cat(out, sizeof out, &n, "\n\nmounted storage:\n  ");
    s_cat(out, sizeof out, &n, g_mounts[0] ? g_mounts : "(/proc/mounts unreadable)");

    /*
     * What the chosen read root actually holds. Probed by name rather than
     * listed, so there is no directory entry layout to depend on -- and these
     * are the directories the engine cannot start without, so a "no" here is
     * the answer on its own.
     */
    s_cat(out, sizeof out, &n, "\n\nmap tree under ");
    s_cat(out, sizeof out, &n, g_data);
    s_cat(out, sizeof out, &n, ":\n");
    {
        static const char *const DIRS[] = {
            "Navkingdom",
            "Navkingdom/Navi",
            "Navkingdom/Navi/CONF",
            "Navkingdom/Navi/DB_HMI",
            "Navkingdom/Common",
            "Navkingdom/Common/DB_MAP",
            "Navkingdom/Common/DB_SEARCH",
            "Navkingdom/Common/DB_SEARCH/poi_img",
            "Navkingdom/Persistence",
            "Navkingdom/manual",
            0
        };
        /* Files the engine asks for by name during startup. A directory can
         * be there and still be half-copied. */
        static const char *const FILES[] = {
            "Navkingdom/NaviVersionInfo.txt",
            "Navkingdom/NavKingdom.cfg",
            "Navkingdom/Common/SystemInfo_Map.xml",
            "Navkingdom/Common/DB_MAP/READY_NF60_NPND_Korea.sti",
            "Navkingdom/Navi/CONF/naviConfig_N200.txt",
            "Navkingdom/Common/DB_MAP/NF60_NPND_Korea.sti",
            0
        };
        int i;
        for (i = 0; DIRS[i]; i++) {
            char p[PATHMAX];
            int ok = join(p, sizeof p, g_data, DIRS[i]) && is_dir(p);
            s_cat(out, sizeof out, &n, "  ");
            s_cat(out, sizeof out, &n, ok ? "yes  " : "NO   ");
            s_cat(out, sizeof out, &n, DIRS[i]);
            s_cat(out, sizeof out, &n, "/\n");
        }
        for (i = 0; FILES[i]; i++) {
            char p[PATHMAX];
            int ok = join(p, sizeof p, g_data, FILES[i]) &&
                     r_access && r_access(p, 0) == 0;
            s_cat(out, sizeof out, &n, "  ");
            s_cat(out, sizeof out, &n, ok ? "yes  " : "NO   ");
            s_cat(out, sizeof out, &n, FILES[i]);
            s_cat(out, sizeof out, &n, "\n");
        }
    }

    s_cat(out, sizeof out, &n,
          "\nIf 'reads' is wrong, put the map tree at <card>/Navkingdom,\n"
          "or name the card in n200fs.conf next to this file:\n"
          "  data=/your/card\n"
          "\nAnything the app then failed to open is appended below.\n");

    /* O_WRONLY|O_CREAT|O_TRUNC. Generic Linux values, the same on ARM. */
    fd = r_open(path, 1 | 0100 | 01000, 0644);
    if (fd < 0) return;
    r_write(fd, out, (u32)n);
    if (r_close) r_close(fd);
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
    GET(read); GET(write); GET(close);
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

    /*
     * Switch on the engine's own logging.
     *
     * It looks for Navkingdom/log_start_flag_true and, finding it, writes a
     * detailed trace of its own -- which says far more about why it will not
     * start than watching its file calls from outside ever can. Creating the
     * flag in the write root is enough: a read of it comes through the overlay
     * and finds it there. Put enginelog=0 in n200fs.conf to stop this.
     */
    if (g_enginelog && r_open) {
        char f[PATHMAX];
        if (join(f, sizeof f, g_write, "Navkingdom/log_start_flag_true")) {
            make_parents(f);
            if (r_access && r_access(f, 0) != 0) {
                int fd = r_open(f, 1 | 0100, 0644);   /* O_WRONLY|O_CREAT */
                if (fd >= 0) {
                    if (r_close) r_close(fd);
                    LOGI("created %s (engine logging on)", f);
                }
            }
        }
    }

    LOGI("read  root: %s", g_data);
    LOGI("write root: %s", g_write);
    LOGI("%s -> the two roots above", PFX);
    write_log();

    g_ready = 1;
    g_busy = 0;

    /* Last, so the roots and the log banner are in place first. */
    gps_start();
}

/* ---- path mapping ------------------------------------------------------ */

/*
 * Should a failure on this path be reported even though it is not one of ours?
 *
 * The app also carries absolute paths that only ever existed on the head unit
 * -- /mnt/hdd/nav1/map/..., /storage/usb0/NVSLOG.CONF,
 * /sys/devices/virtual/switch/reverse/state -- and no redirect touches those.
 * A failure there is worth a line, because it is otherwise invisible and it
 * looks exactly like the engine quietly giving up. The kernel pseudo
 * filesystems are excluded: libraries probe those constantly and the misses
 * mean nothing.
 */
static int worth_reporting(const char *p)
{
    if (!p || p[0] != '/') return 0;
    if (s_ncmp(p, "/proc", 5) == 0) return 0;
    if (s_ncmp(p, "/sys", 4) == 0) return 0;
    if (s_ncmp(p, "/dev", 4) == 0) return 0;
    if (s_ncmp(p, "/data", 5) == 0) return 0;
    if (s_ncmp(p, "/system", 7) == 0) return 0;
    return 1;
}

/* Is this path the app's own mutable state rather than read-only payload?
 * Persistence/ is the whole of it: the app reads these back after writing
 * them, so a read has to see the copy it wrote. */
static int is_state(const char *rest)
{
    if (s_ncmp(rest, "Navkingdom/Persistence", 22) == 0) return 1;
    if (s_ncmp(rest, "Persistence", 11) == 0) return 1;
    return 0;
}

/*
 * Let the DMB/TTS stub write into this same log.
 *
 * The stub traces to logcat, which is no help to someone holding the phone
 * with a file manager and no desktop -- and the question the trace answers,
 * "did the engine call TTSPlayerClass::start() and then stop", is exactly the
 * one left when the last thing in this log is a file that opened fine. The
 * stub reaches this through dlsym rather than a DT_NEEDED, so it still works
 * in a build that has no redirector in it.
 */
void n200fs_note(const char *msg);

/*
 * Append one line to n200fs.log, next to the data.
 *
 * Assembled into one buffer and written with a single write(), because the
 * engine is many threads and a line built from four writes interleaves with
 * another thread's: the device log came back with two reports spliced
 * through each other, mid-word. One O_APPEND write of a whole line cannot
 * tear against another.
 */
static void log_line(const char *a, const char *b, const char *c)
{
    char p[PATHMAX], line[2 * PATHMAX];
    int fd, n = 0;
    if (!r_open || !r_write || !g_write[0]) return;
    if (!join(p, sizeof p, g_write, "n200fs.log")) return;
    line[0] = 0;
    if (a) s_cat(line, sizeof line, &n, a);
    if (b) s_cat(line, sizeof line, &n, b);
    if (c) s_cat(line, sizeof line, &n, c);
    s_cat(line, sizeof line, &n, "\n");
    /* O_WRONLY|O_CREAT|O_APPEND */
    fd = r_open(p, 1 | 0100 | 02000, 0644);
    if (fd < 0) return;
    r_write(fd, line, (u32)n);
    if (r_close) r_close(fd);
}

/*
 * Record a call that actually failed on one of the app's paths.
 *
 * This is the single most useful thing this library can report, and it reports
 * the real outcome rather than guessing from an existence check beforehand:
 * a file can be missing, unreadable, on a card that has gone away, or refused
 * because the directory could not be created, and all four end the same way.
 * The app never checks fopen's result, so any of them becomes a SIGSEGV
 * inside fprintf with nothing to say which file was wanted -- the tombstone
 * this project started from. Naming both the path asked for and the path it
 * was mapped to turns a force-close into one line.
 */
/*
 * Record the app's successful opens, up to a limit.
 *
 * This answers the question the failure log cannot: how far did it get? An
 * empty list means the process died before touching a single file, which puts
 * the fault in LocalServiceActivities.<clinit> -- 25 System.loadLibrary calls
 * before anything is drawn -- and not in this library's business at all. A
 * list that stops partway names the last thing that worked.
 *
 * Bounded, because once the engine is running it opens map tiles continuously
 * and the point here is startup.
 */
static void log_ok(const char *what, const char *orig)
{
    static int count;
    if (count >= 400) return;
    count++;
    log_line("opened ", what, orig);
    if (count == 400)
        log_line("(startup trace ends here; it got at least this far)", "", "");
}

void n200fs_note(const char *msg)
{
    static int count;
    if (!msg || count >= 200) return;
    count++;
    LOGI("%s", msg);
    log_line("stub   ", msg, "");
}

/*
 * stat/access/opendir that found nothing, each distinct path reported once.
 *
 * These are how the engine asks "is it there yet", and when it is stuck it
 * asks in a loop -- so logging every miss would bury the log, and logging none
 * (which is what this did until now) hides the one thing that matters: which
 * file it is waiting for. Deduplicating gives one line per distinct question.
 */
static void log_probe_fail(const char *what, const char *orig)
{
    static char seen[24][96];
    static int nseen;
    int i;

    for (i = 0; i < nseen; i++)
        if (s_ncmp(seen[i], orig, 95) == 0) return;
    if (nseen < 24) {
        s_copy(seen[nseen], 96, orig);
        nseen++;
    }
    LOGI("%s missing: %s", what, orig);
    log_line("probe  ", what, "");
    log_line("    missing: ", orig, "");
}

static void log_fail(const char *what, const char *orig, const char *mapped)
{
    static int count;
    if (count >= 150) return;
    count++;
    LOGE("%s FAILED: %s -> %s", what, orig, mapped);
    log_line("FAILED ", what, "");
    log_line("    asked for: ", orig, "");
    log_line("    looked at: ", mapped, "");
    if (count == 150) {
        LOGE("(further failure reports suppressed)");
        log_line("(further failure reports suppressed)", "", "");
    }
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

/*
 * Join a root to the remainder of a redirected path, turning backslashes into
 * slashes on the way.
 *
 * On the system this engine was first written for they were separators; on
 * Linux a backslash is an ordinary filename character, so
 * "Common/DB_MAP\\map_version.ini" is one long filename and can never
 * resolve. Normalising only ever applies to paths under a root the app
 * hard-codes, so a real filename containing a backslash elsewhere is
 * untouched.
 */
static int join_rest(char *buf, int cap, const char *root, const char *rest)
{
    int n = 0, i;
    buf[0] = 0;
    if (!s_cat(buf, cap, &n, root)) return 0;
    if (rest[0]) {
        if (!s_cat(buf, cap, &n, "/")) return 0;
        for (i = 0; rest[i]; i++) {
            if (n + 1 >= cap) return 0;
            buf[n++] = is_sep(rest[i]) ? '/' : rest[i];
        }
        buf[n] = 0;
    }
    return 1;
}

/*
 * Does p start with one of the app's hard-coded roots, as a whole path
 * component? Separators compare equal either way round, because the Windows
 * CE-era paths in these binaries are spelled with backslashes.
 */
static int strip_root(const char *p, const char **rest)
{
    int i, k;
    if (!p) return 0;
    for (i = 0; ROOTS[i].p; i++) {
        const char *r = ROOTS[i].p;
        for (k = 0; r[k]; k++) {
            if (is_sep(p[k]) && is_sep(r[k])) continue;
            if (p[k] != r[k]) break;
        }
        if (r[k]) continue;                     /* did not match in full */
        if (p[k] && !is_sep(p[k])) continue;    /* /mnt/extSdCardFoo is not ours */
        {
            const char *q = p + k;
            while (is_sep(*q)) q++;             /* the app writes /// a lot */
            if (ROOTS[i].need_nk && s_ncmp(q, "Navkingdom", 10) != 0) continue;
            *rest = q;
            return 1;
        }
    }
    return 0;
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
    /* The whole cost for a path that is none of ours: one compare per root,
     * each bailing on the first character for anything that is not /mnt/... ,
     * /storage/... or /Storage... */
    if (!strip_root(p, &rest)) return p;
    resolve();
    if (!g_ready) return p;

    if (want_write) {
        if (!join_rest(buf, cap, g_write, rest)) return p;
        make_parents(buf);
        return buf;
    }

    /*
     * Which root a read prefers.
     *
     * Persistence/ is the app's own mutable state -- the ~120 .bak and .dat
     * files it writes and reads back -- so there the copy it wrote has to win
     * over whatever the card shipped with, or settings and saved routes would
     * silently reset every start.
     *
     * Everything else is read-only payload: Common/, Navi/, manual/. Those
     * must come from the card even when a stale copy is still sitting on
     * internal storage from an earlier, internal-only install. Preferring the
     * write root for those is what makes a half-deleted leftover tree shadow
     * the real data.
     *
     * Either way the other root is still tried, so a file present in only one
     * place is found wherever it is.
     */
    {
        const char *first = is_state(rest) ? g_write : g_data;
        const char *second = (first == g_write) ? g_data : g_write;

        if (!join_rest(buf, cap, first, rest)) return p;
        if (r_access && r_access(buf, 0 /* F_OK */) == 0) return buf;
        if (join_rest(buf, cap, second, rest) &&
            r_access && r_access(buf, 0) == 0)
            return buf;
        /* In neither root. Hand back the preferred one so the caller fails
         * with a sensible path; whether that failure matters is decided by
         * the caller, and logged there. */
        if (!join_rest(buf, cap, first, rest)) return p;
        return buf;
    }
}

/* ---- the interposed entry points --------------------------------------- */

void *fopen(const char *path, const char *mode)
{
    char buf[PATHMAX];
    const char *m;
    void *f;
    int w, ours;
    resolve();
    if (!r_fopen) return 0;
    if (mode && mode[0] == 'r' && is_gps_fifo(path)) {
        int pfd = gps_open_pipe(path);
        if (pfd >= 0) {
            void *fp = r_fdopen ? r_fdopen(pfd, "r") : 0;
            if (fp) return fp;
            if (r_close) r_close(pfd);
        }
    }
    { const char *rest; ours = strip_root(path, &rest) || worth_reporting(path); }
    w = mode && (mode[0] != 'r' || s_chr(mode, '+') != 0);
    m = map_path(path, w, buf, sizeof buf);
    f = r_fopen(m, mode);
    /* The caller will not check this, so say so here while both paths are
     * still in hand. */
    if (ours) {
        if (f) log_ok(w ? "w " : "r ", path);
        else   log_fail(w ? "fopen(write)" : "fopen(read)", path, m);
    }
    return f;
}

int open(const char *path, int flags, ...)
{
    char buf[PATHMAX];
    const char *m;
    int mode = 0, w, ours, fd;
    resolve();
    if (!r_open) return -1;
    if (flags & O_CREAT) {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }
    /* The engine asking for its GPS feed. Give it a pipe we control rather
     * than a FIFO that may not exist. */
    if ((flags & O_ACCMODE) != 1 && is_gps_fifo(path)) {
        int pfd = gps_open_pipe(path);
        if (pfd >= 0) return pfd;
    }
    { const char *rest; ours = strip_root(path, &rest) || worth_reporting(path); }
    w = (flags & O_ACCMODE) != 0 || (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0;
    m = map_path(path, w, buf, sizeof buf);
    fd = r_open(m, flags, mode);
    if (ours) {
        if (fd >= 0) log_ok(w ? "w " : "r ", path);
        else         log_fail(w ? "open(write)" : "open(read)", path, m);
    }
    return fd;
}

int stat(const char *path, void *st)
{
    char buf[PATHMAX];
    const char *rest;
    int rc, ours;
    resolve();
    if (!r_stat) return -1;
    ours = strip_root(path, &rest);
    rc = r_stat(map_path(path, 0, buf, sizeof buf), st);
    if (rc < 0 && ours) log_probe_fail("stat", path);
    return rc;
}

int access(const char *path, int amode)
{
    char buf[PATHMAX];
    const char *rest;
    int rc, ours;
    resolve();
    if (!r_access) return -1;
    ours = strip_root(path, &rest);
    /* W_OK is 2: an access(W_OK) check is about to be followed by a write. */
    rc = r_access(map_path(path, (amode & 2) != 0, buf, sizeof buf), amode);
    if (rc < 0 && ours && (amode & 2) == 0) log_probe_fail("access", path);
    return rc;
}

void *opendir(const char *path)
{
    char buf[PATHMAX];
    const char *rest;
    void *d;
    int ours;
    resolve();
    if (!r_opendir) return 0;
    ours = strip_root(path, &rest);
    d = r_opendir(map_path(path, 0, buf, sizeof buf));
    if (!d && ours) log_probe_fail("opendir", path);
    return d;
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

/* ====================================================================== *
 *  Synthetic GPS
 *
 *  WHY
 *  ---
 *  The engine will not leave its "preparing navigation" screen until the
 *  position module has a fix. Its own log makes that plain: 1170 identical
 *  lines of "[PREDICT_LOG] 0 0 0 0 1 0 0" -- the dead-reckoning loop running
 *  on all-zero input.
 *
 *  Position reaches it as raw NMEA through a FIFO:
 *
 *      Android GPS -> GpsBridge (Java) -> /data/data/<pkg>/files/gpsfifo
 *                  -> libNV_UART.so -> libNV_POS.so
 *
 *  GpsBridge forwards onNmeaReceived() only -- its onLocationChanged() is an
 *  empty method -- so a mock-location app cannot drive this: mock providers
 *  deliver Location objects, never NMEA. Indoors, with no real fix, nothing is
 *  ever written and the app cannot start at all.
 *
 *  So write the NMEA here instead. libn200fs.so is already in the process, and
 *  a FIFO does not care which thread fills it.
 *
 *  WHAT libNV_UART.so REQUIRES
 *  ---------------------------
 *  Two of its own messages set the rules:
 *      "NMEA Data Read Fail because of no GPRMC Message"  -- GPRMC is required
 *      "NMEA checksum err: %s"                            -- checksums are checked
 *  It also parses GPGGA, GPGSA, GPGSV and GPVTG, so a full stationary burst is
 *  sent rather than the bare minimum.
 *
 *  NO FLOATING POINT
 *  -----------------
 *  This library links -nostdlib, so there is no libgcc and no soft-float
 *  support code to call. Coordinates are held in microdegrees and formatted
 *  with integer arithmetic throughout; every divisor is a compile-time
 *  constant, so the compiler turns them into multiplies rather than calls to
 *  __aeabi_idiv. The build gate fails on any unexpected import, which is what
 *  keeps that honest.
 * ====================================================================== */

static int  (*r_usleep)(u32);
static long (*r_time)(void *);
static int  (*r_pthread_create)(void *, const void *, void *(*)(void *), void *);

#ifndef N200FS_GPS_DEFAULT
#define N200FS_GPS_DEFAULT 1
#endif
static int g_gps = N200FS_GPS_DEFAULT;                 /* inject unless told otherwise */
static int g_lat = 37566500;          /* microdegrees; Seoul City Hall */
static int g_lon = 126978000;

/* ---- integer formatting ------------------------------------------------ */

static void put_u(char *b, int *n, int cap, unsigned v, int digits)
{
    char t[12];
    int k = 0;
    if (!digits) {                       /* natural width */
        if (!v) t[k++] = '0';
        while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    } else {
        while (digits--) { t[k++] = (char)('0' + v % 10); v /= 10; }
    }
    while (k-- > 0) {
        if (*n + 1 >= cap) return;
        b[(*n)++] = t[k];
    }
    b[*n] = 0;
}

/* 37566500 -> "3733.9900" : degrees, then minutes to four decimals. */
static void put_coord(char *b, int *n, int cap, int udeg, int degdigits)
{
    unsigned a = (unsigned)(udeg < 0 ? -udeg : udeg);
    unsigned deg = a / 1000000u;
    unsigned min_e4 = (a % 1000000u) * 6u / 10u;   /* frac * 60, scaled 1e4 */
    put_u(b, n, cap, deg, degdigits);
    put_u(b, n, cap, min_e4 / 10000u, 2);
    if (*n + 1 < cap) b[(*n)++] = '.';
    put_u(b, n, cap, min_e4 % 10000u, 4);
    b[*n] = 0;
}

/* ---- one sentence, with checksum -------------------------------------- */

static void send_nmea(int fd, const char *body, int blen)
{
    char line[160];
    int n = 0, i;
    unsigned char ck = 0;
    static const char HEX[] = "0123456789ABCDEF";

    if (blen + 8 > (int)sizeof line) return;
    line[n++] = '$';
    for (i = 0; i < blen; i++) {
        line[n++] = body[i];
        ck ^= (unsigned char)body[i];
    }
    line[n++] = '*';
    line[n++] = HEX[(ck >> 4) & 0xF];
    line[n++] = HEX[ck & 0xF];
    line[n++] = '\r';
    line[n++] = '\n';
    if (r_write) r_write(fd, line, (u32)n);
}

/* ---- UTC from time(), integer only ------------------------------------ */

static void civil_from_days(long z, unsigned *y, unsigned *m, unsigned *d)
{
    long era, doe, yoe, doy, mp, day, mon, yr;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yr = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    mon = mp < 10 ? mp + 3 : mp - 9;
    if (mon <= 2) yr++;
    *y = (unsigned)yr;
    *m = (unsigned)mon;
    *d = (unsigned)day;
}

/* ---- the burst -------------------------------------------------------- */

static void send_burst(int fd)
{
    char b[160];
    int n;
    long t = r_time ? r_time(0) : 0;
    long days = t / 86400;
    unsigned secs = (unsigned)(t - days * 86400);
    unsigned hh = secs / 3600, mi = (secs / 60) % 60, ss = secs % 60;
    unsigned yy, mo, dd;

    civil_from_days(days, &yy, &mo, &dd);

    /* GPRMC -- the one it refuses to work without. "A" = fix valid. */
    n = 0; b[0] = 0;
    s_cat(b, sizeof b, &n, "GPRMC,");
    put_u(b, &n, sizeof b, hh, 2); put_u(b, &n, sizeof b, mi, 2);
    put_u(b, &n, sizeof b, ss, 2);
    s_cat(b, sizeof b, &n, ".00,A,");
    put_coord(b, &n, sizeof b, g_lat, 2);
    s_cat(b, sizeof b, &n, g_lat < 0 ? ",S," : ",N,");
    put_coord(b, &n, sizeof b, g_lon, 3);
    s_cat(b, sizeof b, &n, g_lon < 0 ? ",W," : ",E,");
    s_cat(b, sizeof b, &n, "0.00,0.00,");
    put_u(b, &n, sizeof b, dd, 2); put_u(b, &n, sizeof b, mo, 2);
    put_u(b, &n, sizeof b, yy % 100, 2);
    s_cat(b, sizeof b, &n, ",,,A");
    send_nmea(fd, b, n);

    /* GPGGA -- fix quality 1, 10 satellites, HDOP 0.9, 50 m altitude. */
    n = 0; b[0] = 0;
    s_cat(b, sizeof b, &n, "GPGGA,");
    put_u(b, &n, sizeof b, hh, 2); put_u(b, &n, sizeof b, mi, 2);
    put_u(b, &n, sizeof b, ss, 2);
    s_cat(b, sizeof b, &n, ".00,");
    put_coord(b, &n, sizeof b, g_lat, 2);
    s_cat(b, sizeof b, &n, g_lat < 0 ? ",S," : ",N,");
    put_coord(b, &n, sizeof b, g_lon, 3);
    s_cat(b, sizeof b, &n, g_lon < 0 ? ",W," : ",E,");
    s_cat(b, sizeof b, &n, "1,10,0.9,50.0,M,20.0,M,,");
    send_nmea(fd, b, n);

    /* GPGSA -- automatic mode, 3D fix. */
    send_nmea(fd, "GPGSA,A,3,01,02,03,04,05,06,07,08,09,10,,,1.6,0.9,1.3", 53);

    /* GPGSV -- one message, four satellites in view, healthy C/No. */
    send_nmea(fd, "GPGSV,1,1,10,01,60,045,45,02,55,135,44,03,50,225,43,04,45,315,42", 64);

    /* GPVTG -- stationary. */
    send_nmea(fd, "GPVTG,0.00,T,,M,0.00,N,0.00,K,A", 31);
}

/*
 * Hand the engine a pipe instead of depending on the FIFO.
 *
 * The FIFO was the fragile part. GpsBridge creates it by shelling out to
 * "mknod" or "toolbox mknod", neither of which exists on every build, and if
 * that fails there is no node to open, no GPS at all, and nothing in any log
 * to say why.
 *
 * libNV_UART.so is one of the libraries this library is bound ahead of, so its
 * open() comes through here. When it asks for the gpsfifo, give it the read
 * end of a pipe() and keep the write end. A pipe needs no filesystem node, no
 * mknod, no permissions and no reader/writer handshake, and it is the same
 * S_IFIFO to fstat() as the real thing.
 *
 * The cost: the real GpsBridge can no longer reach the engine while this is
 * on, because its writes go to the path and not to this pipe. That is what
 * gps=off is for.
 */
static int (*r_pipe)(int *);
static int (*r_fcntl)(int, int, long);
static int g_gps_wfd = -1;

static int is_gps_fifo(const char *p)
{
    int n;
    if (!g_gps || !p) return 0;
    n = s_len(p);
    return n >= 8 && s_ncmp(p + n - 8, "/gpsfifo", 8) == 0;
}

/* Returns the read end for the caller; keeps the write end for the feeder. */
static int gps_open_pipe(const char *path)
{
    int fds[2];
    if (!r_pipe || r_pipe(fds) != 0) return -1;
    if (g_gps_wfd >= 0 && r_close) r_close(g_gps_wfd);
    /* Non-blocking write end: if the engine stops reading, the feeder takes an
     * EAGAIN instead of wedging on a full pipe. */
    if (r_fcntl) r_fcntl(fds[1], 4 /* F_SETFL */, 04000 /* O_NONBLOCK */);
    g_gps_wfd = fds[1];
    LOGI("handing %s a pipe, feeding synthetic NMEA", path);
    log_line("gps    engine opened the gpsfifo; feeding synthetic NMEA", "", "");
    return fds[0];
}

static void *gps_thread(void *arg)
{
    int sent = 0, waited = 0;
    (void)arg;
    for (;;) {
        if (g_gps_wfd >= 0) {
            send_burst(g_gps_wfd);
            if (!sent) {
                sent = 1;
                log_line("gps    first NMEA burst written", "", "");
            }
        } else if (++waited == 20) {
            /* So that a log with no gps line at all means something definite:
             * nothing ever asked for the fifo. */
            log_line("gps    waiting -- nothing has opened the gpsfifo", "", "");
        }
        if (r_usleep) r_usleep(1000000);        /* 1 Hz, like a receiver */
    }
    return 0;                                   /* not reached */
}

static void gps_start(void)
{
    static unsigned long tid;
    static int started;
    if (!g_gps || started) return;
    r_usleep = (void *)dlsym(RTLD_DEFAULT, "usleep");
    r_pipe = (void *)dlsym(RTLD_DEFAULT, "pipe");
    r_fcntl = (void *)dlsym(RTLD_DEFAULT, "fcntl");
    r_fdopen = (void *)dlsym(RTLD_DEFAULT, "fdopen");
    r_time = (void *)dlsym(RTLD_DEFAULT, "time");
    r_pthread_create = (void *)dlsym(RTLD_DEFAULT, "pthread_create");
    if (!r_pthread_create || !r_time || !r_usleep || !r_pipe) {
        LOGE("cannot start the synthetic GPS feed (missing libc entry points)");
        return;
    }
    started = 1;
    if (r_pthread_create(&tid, 0, gps_thread, 0) != 0)
        LOGE("synthetic GPS thread would not start");
}

/*
 * "off" / "no" / "0" turns the feed off. Otherwise "<lat>,<lon>" in decimal
 * degrees, parsed into microdegrees without touching a float.
 */
static int parse_gps(const char *v)
{
    int sign = 1, i = 0, k;
    int val[2] = {0, 0}, got = 0;

    if (s_ncmp(v, "off", 3) == 0 || s_ncmp(v, "no", 2) == 0 ||
        (v[0] == '0' && (v[1] == 0 || v[1] == '\n'))) {
        g_gps = 0;
        return 1;
    }
    while (got < 2) {
        int whole = 0, frac = 0, scale = 1000000;
        sign = 1;
        while (v[i] == ' ') i++;
        if (v[i] == '-') { sign = -1; i++; }
        else if (v[i] == '+') i++;
        if (v[i] < '0' || v[i] > '9') return 0;
        while (v[i] >= '0' && v[i] <= '9') whole = whole * 10 + (v[i++] - '0');
        if (v[i] == '.') {
            i++;
            for (k = 0; k < 6; k++) {
                scale /= 10;
                if (v[i] >= '0' && v[i] <= '9') frac += (v[i++] - '0') * scale;
            }
            while (v[i] >= '0' && v[i] <= '9') i++;   /* ignore extra digits */
        }
        val[got++] = sign * (whole * 1000000 + frac);
        if (got < 2) {
            while (v[i] == ' ') i++;
            if (v[i] != ',') return 0;
            i++;
        }
    }
    g_lat = val[0];
    g_lon = val[1];
    g_gps = 1;
    return 1;
}

/* ====================================================================== *
 *  The "preparing navigation" gate
 *
 *  CNaviReadyScene, in libNV_GUI.so, polls on a timer and logs exactly what
 *  it is waiting for:
 *
 *    [CNaviReadyScene::OnTimerEvent] NaviGetNaviSDCardState(%d) bDBLoadInit(%d)
 *    CNaviReadyScene::OnTimerEvent() : mdGetFirstLayerOnState() = %d, ...
 *
 *  So the screen is gated on the SD card state, not on GPS -- which is why
 *  neither a real fix nor a synthetic one moved it.
 *
 *  NaviGetNaviSDCardState() is defined in libGUI_WIDGET.so and IMPORTED by
 *  libNV_GUI.so, so this library -- bound ahead of libGUI_WIDGET in
 *  libNV_GUI's DT_NEEDED -- can answer it instead. The original is a plain
 *  getter on a global bool (ARM, 52 bytes: load the global, LDRB, return), so
 *  returning 1 here is exactly equivalent to that global being set, without
 *  patching a single byte of vendor code.
 *
 *  The global is set from CNaviScene::OnSDCardInsert(), which runs when Java's
 *  SetMessgeEvent(0, 1, PATH_SDCARD) arrives. That path is now reached -- the
 *  dex patch sees to it -- but whether the native handler accepts the event is
 *  not something that can be established from outside, and this makes it moot.
 *
 *  sdstate=off in n200fs.conf gives the original behaviour back.
 * ====================================================================== */

int g_sdstate = 1;
int g_force = 0;      /* force=on in n200fs.conf releases stuck gates */

/*
 * All five values CNaviReadyScene and its neighbours poll are getters defined
 * in libGUI_WIDGET.so and imported by libNV_GUI.so, so each one passes through
 * here on its way. That is used twice over: the real value is fetched from
 * libGUI_WIDGET.so and logged whenever it changes, so the log states which
 * condition is holding the screen; and the two that have been seen to stick
 * can be answered here instead.
 */
struct gate {
    const char *sym, *label;
    int (*real)(void);
    int tried, last;
    long since;                     /* when it was first seen at 0 */
};

static long now_s(void)
{
    static long (*t)(void *);
    if (!t) t = (long (*)(void *))dlsym(RTLD_DEFAULT, "time");
    return t ? t(0) : 0;
}

static int gate_real(struct gate *g, int (*self)(void))
{
    int v;
    if (!g->tried) {
        void *h;
        g->tried = 1;
        g->last = -2;
        /* By handle, not RTLD_DEFAULT: a global lookup would find this
         * library's own definition first. */
        h = dlopen("libGUI_WIDGET.so", RTLD_NOW);
        if (h) g->real = (int (*)(void))dlsym(h, g->sym);
        if (g->real == self) g->real = 0;
    }
    v = g->real ? (g->real() & 0xff) : -1;
    if (v != g->last) {
        char num[4];
        g->last = v;
        num[0] = v < 0 ? '?' : (char)('0' + (v > 9 ? 9 : v));
        num[1] = 0;
        LOGI("gate %s = %s", g->label, num);
        log_line("gate   ", g->label, v < 0 ? " = (real function not found)"
                                   : v == 0 ? " = 0" : " = 1");
    }
    return v;
}

/* Answer 1 once the real value has sat at 0 for `wait` seconds. */
static int gate_forced(struct gate *g, int v, int wait)
{
    long t;
    if (v == 1 || !g_force) return v < 0 ? 0 : v;
    t = now_s();
    if (!g->since) g->since = t;
    if (t - g->since < wait) return v < 0 ? 0 : v;
    if (g->since != -1) {
        g->since = -1;
        log_line("gate   ", g->label, " stuck at 0 -- answering 1 from here on");
    }
    return 1;
}

int _Z22NaviGetNaviSDCardStatev(void)
{
    static struct gate g = { "_Z22NaviGetNaviSDCardStatev",
                             "NaviGetNaviSDCardState", 0, 0, 0, 0 };
    int v = gate_real(&g, _Z22NaviGetNaviSDCardStatev);
    if (g_sdstate) return 1;
    return v < 0 ? 0 : v;
}

int _Z22mdGetFirstLayerOnStatev(void)
{
    static struct gate g = { "_Z22mdGetFirstLayerOnStatev",
                             "mdGetFirstLayerOnState", 0, 0, 0, 0 };
    return gate_forced(&g, gate_real(&g, _Z22mdGetFirstLayerOnStatev), 30);
}

int _Z21NaviGetMapInitialDonev(void)
{
    static struct gate g = { "_Z21NaviGetMapInitialDonev",
                             "NaviGetMapInitialDone", 0, 0, 0, 0 };
    return gate_forced(&g, gate_real(&g, _Z21NaviGetMapInitialDonev), 30);
}

/* Watched only: these are passed straight through. */
int _Z17NaviChkFirstMountv(void)
{
    static struct gate g = { "_Z17NaviChkFirstMountv",
                             "NaviChkFirstMount", 0, 0, 0, 0 };
    int v = gate_real(&g, _Z17NaviChkFirstMountv);
    return v < 0 ? 0 : v;
}

int _Z28NaviGetUnloadNaviProcessFlagv(void)
{
    static struct gate g = { "_Z28NaviGetUnloadNaviProcessFlagv",
                             "NaviGetUnloadNaviProcessFlag", 0, 0, 0, 0 };
    int v = gate_real(&g, _Z28NaviGetUnloadNaviProcessFlagv);
    long t;
    /* This one is stuck the other way round: 1 means "navigation processes
     * not loaded", and it is cleared by the SD-insert handler that a device
     * log showed never running. The processes themselves are up -- the map
     * tables are being read -- so after the same wait, answer 0. */
    if (v != 1 || !g_force) return v < 0 ? 0 : v;
    t = now_s();
    if (!g.since) g.since = t;
    if (t - g.since < 30) return 1;
    if (g.since != -1) {
        g.since = -1;
        log_line("gate   ", g.label, " stuck at 1 -- answering 0 from here on");
    }
    return 0;
}

/* ====================================================================== *
 *  Capturing the engine's own trace
 *
 *  Its debug messages go through NVDebugMessage::traceMsg/traceErr into
 *  libNVDebugMessage.so, which forwards them over shared memory to an ILogger
 *  server process -- a head-unit daemon that does not exist here. So the most
 *  informative output the engine produces goes nowhere.
 *
 *  Those two are imported by libNV_GUI.so and five of its neighbours, so they
 *  can be answered here and written to n200fs.log instead.
 *
 *  The signature is traceMsg(int, const char*, const char*, const char*, ...)
 *  by its mangled name, but whether there is also a hidden `this` cannot be
 *  told from the name, so no argument position is assumed: all six candidate
 *  slots are examined and any that points at readable text is logged. Every
 *  dereference is checked first by handing the pointer to write() on
 *  /dev/null, which makes the kernel return EFAULT rather than letting a stray
 *  integer argument fault the process. Results are cached per pointer, because
 *  these are static literals and the engine traces thousands of times a
 *  second.
 *
 *  Filtered to the messages that bear on why startup stalls, so the log stays
 *  readable; trace=all in n200fs.conf keeps everything.
 * ====================================================================== */

int g_trace = 1;
int g_trace_all;

/*
 * Is this address inside a loaded shared object?
 *
 * dladdr() answers that by walking the linker's list of loaded objects and
 * comparing addresses -- it never dereferences the pointer, so a stray integer
 * argument in a pointer slot costs nothing. The engine's trace format strings
 * are literals in its libraries' .rodata, so they validate; 1, 7 and
 * 0xdeadbeef do not.
 *
 * The first attempt here handed the pointer to write() on /dev/null and let
 * the kernel return EFAULT. That does work on a device, but it is a syscall
 * per pointer and it cannot be tested -- ASan intercepts write(), checks the
 * buffer itself and reports the fault instead of passing it to the kernel, so
 * the self-test crashed on exactly the case the guard existed for. A guard
 * that cannot be exercised is not a guard.
 */
static int (*r_dladdr)(const void *, void *);

static int readable(const void *p)
{
    static const void *key[64];
    static char val[64];
    void *info[4];
    unsigned h;

    if (!p) return 0;
    if (!r_dladdr) {
        r_dladdr = (int (*)(const void *, void *))dlsym(RTLD_DEFAULT, "dladdr");
        if (!r_dladdr) return 0;
    }
    h = (unsigned)(((unsigned long)p >> 3) & 63);
    if (key[h] == p) return val[h];
    key[h] = p;
    val[h] = (char)(r_dladdr(p, info) != 0);
    return val[h];
}

/* Copy p into out if it looks like a printable string. Returns its length. */
static int grab_text(const char *p, char *out, int cap)
{
    int i;
    if (!readable(p)) return 0;
    for (i = 0; i < cap - 1; i++) {
        unsigned char c;
        /* Re-validate as we go: a string near the end of a mapping must not
         * be run off the end of it. */
        if ((i & 63) == 0 && !readable(p + i)) break;
        c = (unsigned char)p[i];
        if (c == 0) break;
        if (c < 0x20 && c != '\n' && c != '\t') return 0;   /* not text */
        out[i] = (char)c;
    }
    out[i] = 0;
    return i >= 4 ? i : 0;
}

static int trace_interesting(const char *t)
{
    static const char *const KEY[] = {
        "Ready", "SDCard", "SdCard", "DBLoad", "DBLoadInit", "FirstLayer",
        "mdGet", "Navi ON", "Unload", "LoadNavi", "Mount", "MessgeEvent",
        "Media", "LI_EVENTS", "OPEN_DB", "CLOSE_DB", "INSERT", "EJECT",
        "STARTUP", "PreActivated", "MapInitial", "Surface", "surface", "EGL",
        "fail", "Fail", "FAIL", "error", "Error", "ERROR", 0
    };
    int i, j;
    if (g_trace_all) return 1;
    for (i = 0; KEY[i]; i++) {
        for (j = 0; t[j]; j++) {
            int k = 0;
            while (KEY[i][k] && t[j + k] == KEY[i][k]) k++;
            if (!KEY[i][k]) return 1;
        }
    }
    return 0;
}

/* Expand a printf format with the word-sized arguments that followed it.
 * Only what the engine's messages use: %d %i %u %x %X %c %s %p, with flags,
 * width and an 'l'. A %s argument is validated before it is read; anything
 * unrecognised (%f, %lld) ends expansion, since the argument words can no
 * longer be lined up. */
static void fmt_expand(char *out, int cap, int *n, const char *f,
                       const unsigned long *v, int nv)
{
    int k = 0;
    while (*f && *n + 1 < cap) {
        char c, t[200];
        if (*f != '%') { out[(*n)++] = *f++; out[*n] = 0; continue; }
        f++;
        if (*f == '%') { out[(*n)++] = *f++; out[*n] = 0; continue; }
        while (*f == '-' || *f == '+' || *f == ' ' || *f == '#' || *f == '.' ||
               (*f >= '0' && *f <= '9')) f++;
        if (*f == 'l' && f[1] != 'l') f++;
        c = *f;
        if (!c) break;
        f++;
        if (k >= nv) { s_cat(out, cap, n, "?"); continue; }
        if (c == 'd' || c == 'i' || c == 'u') {
            long sv = (long)(int)v[k++];
            if (c != 'u' && sv < 0) { s_cat(out, cap, n, "-"); sv = -sv; }
            put_u(out, n, cap, (unsigned)sv, 0);
        } else if (c == 'x' || c == 'X' || c == 'p') {
            static const char H[] = "0123456789abcdef";
            unsigned x = (unsigned)v[k++];
            int sh, on = 0;
            for (sh = 28; sh >= 0; sh -= 4) {
                int dgt = (int)((x >> sh) & 15);
                if (dgt || on || sh == 0) {
                    on = 1;
                    if (*n + 1 < cap) { out[(*n)++] = H[dgt]; out[*n] = 0; }
                }
            }
        } else if (c == 'c') {
            char ch = (char)v[k++];
            if (ch >= 0x20 && *n + 1 < cap) { out[(*n)++] = ch; out[*n] = 0; }
        } else if (c == 's') {
            if (grab_text((const char *)v[k], t, (int)sizeof t))
                s_cat(out, cap, n, t);
            else
                s_cat(out, cap, n, "<?>");
            k++;
        } else {
            s_cat(out, cap, n, "<..>");
            break;
        }
    }
}

/* Each distinct message is logged a few times, not every time: the engine
 * repeats some of them hundreds of times a second and they crowded the
 * messages that matter out of the last log. */
static int seen_too_often(const void *fmt)
{
    static const void *key[256];
    static unsigned char cnt[256];
    unsigned h = (unsigned)(((unsigned long)fmt >> 2) & 255);
    if (key[h] != fmt) { key[h] = fmt; cnt[h] = 0; }
    if (cnt[h] >= 4) return 1;
    cnt[h]++;
    return 0;
}

static void trace_capture(const char *tag, void *a1, void *a2, void *a3,
                          const unsigned long *v, int nv)
{
    static int count;
    char line[480];
    char piece[200];
    int n = 0;

    if (!g_trace || count >= 1500) return;
    line[0] = 0;
    if (grab_text((const char *)a1, piece, (int)sizeof piece)) {
        s_cat(line, sizeof line, &n, piece);
        s_cat(line, sizeof line, &n, " | ");
    }
    if (grab_text((const char *)a2, piece, (int)sizeof piece)) {
        s_cat(line, sizeof line, &n, piece);
        s_cat(line, sizeof line, &n, " | ");
    }
    if (readable(a3) && grab_text((const char *)a3, piece, 8)) {
        if (seen_too_often(a3)) return;
        fmt_expand(line, (int)sizeof line, &n, (const char *)a3, v, nv);
    } else if (!n) {
        return;
    }
    if (!trace_interesting(line)) return;
    count++;
    log_line(tag, line, "");
    if (count == 1500) log_line("trace  (further engine trace suppressed)", "", "");
}

#define TRACE_HOOK(sym, tag) \
int sym(void *a0, void *a1, void *a2, void *a3, ...) \
{ \
    unsigned long v[8]; \
    int i; \
    __builtin_va_list ap; \
    (void)a0; \
    __builtin_va_start(ap, a3); \
    for (i = 0; i < 8; i++) v[i] = __builtin_va_arg(ap, unsigned long); \
    __builtin_va_end(ap); \
    trace_capture(tag, a1, a2, a3, v, 8); \
    return 0; \
}
TRACE_HOOK(_ZN14NVDebugMessage8traceMsgEiPKcS1_S1_z, "trace  ")
TRACE_HOOK(_ZN14NVDebugMessage8traceErrEiPKcS1_S1_z, "traceE ")
TRACE_HOOK(_ZN14NVDebugMessage8traceWarEiPKcS1_S1_z, "traceW ")

/* ====================================================================== *
 *  The mount name that never matched
 *
 *  CCommonEventHandler::OnMediaMountEvent, in libNV_GUI.so, decides whether a
 *  mount event is the navigation card with
 *
 *      strcasecmp(mountName, "/mnt/extSdCard//")
 *      strcasecmp(mountName, "mnt/extSdCard//")
 *
 *  -- both with two trailing slashes -- and only then calls SDCardChk(), whose
 *  INSERT branch sends eCNS2K_NPND_LoadNaviProcess and clears the unload
 *  flag. The Java side sends PATH_SDCARD, which is "mnt/extSdCard" with none.
 *  The comparison fails, the event falls through to the USB branch, fails
 *  there too, and the handler returns having done nothing: a device trace
 *  shows BEGIN and END four ticks apart with no SDCardChk between them. That
 *  is the whole of the "preparing navigation" hang.
 *
 *  The string cannot be lengthened in the dex, so make the comparison agree
 *  instead: libNV_GUI.so imports strcasecmp, and here two spellings of the
 *  card root that differ only in a leading slash or trailing slashes compare
 *  equal. Everything else is an ordinary ASCII case-insensitive compare.
 * ====================================================================== */

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* "/mnt/extSdCard", "mnt/extSdCard//", ... -- the card root and nothing more */
static int is_card_root(const char *p)
{
    static const char R[] = "mnt/extsdcard";
    int i;
    if (*p == '/') p++;
    for (i = 0; R[i]; i++)
        if (lc((unsigned char)p[i]) != R[i]) return 0;
    p += i;
    while (*p == '/') p++;
    return *p == 0;
}

int strcasecmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    if ((x[0] == '/' || lc(x[0]) == 'm') && (y[0] == '/' || lc(y[0]) == 'm') &&
        is_card_root(a) && is_card_root(b)) {
        static int said;
        if (!said) {
            said = 1;
            log_line("mount  card root names treated as equal: ", a, "");
        }
        return 0;
    }
    for (;; x++, y++) {
        int d = lc(*x) - lc(*y);
        if (d || !*x) return d;
    }
}

/* ====================================================================== *
 *  Capturing the modules that log through Android's logger
 *
 *  libNV_MD.so (the map renderer) and libNV_CNS2K.so do not use
 *  NVDebugMessage; they call __android_log_print directly, so nothing they
 *  say reaches n200fs.log through the trace hooks above -- and the map
 *  renderer is exactly the module whose "first layer on" state never comes
 *  true. Both import __android_log_print, so it is answered here: the call is
 *  passed on to liblog unchanged, and for callers in those two libraries a
 *  copy goes to n200fs.log, tagged "alog".
 *
 *  The caller is identified from the return address with dladdr(), cached per
 *  address, so the cost for everyone else is one table probe.
 * ====================================================================== */

static int wanted_caller(const void *ra)
{
    static const void *key[64];
    static char val[64];
    void *info[4];
    const char *f;
    unsigned h = (unsigned)(((unsigned long)ra >> 2) & 63);
    int i, hit = 0;

    if (key[h] == ra) return val[h];
    key[h] = ra;
    val[h] = 0;
    if (!r_dladdr)
        r_dladdr = (int (*)(const void *, void *))dlsym(RTLD_DEFAULT, "dladdr");
    if (!r_dladdr || !r_dladdr(ra, info) || !info[0]) return 0;
    f = (const char *)info[0];
    for (i = 0; f[i]; i++)
        if (s_ncmp(f + i, "libNV_MD", 8) == 0 || s_ncmp(f + i, "libNV_CNS2K", 11) == 0)
            hit = 1;
    val[h] = (char)hit;
    return hit;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    static int (*real_v)(int, const char *, const char *, __builtin_va_list);
    static int tried, count;
    __builtin_va_list ap;
    int r = 0;

    if (!tried) {
        void *h;
        tried = 1;
        h = dlopen("liblog.so", RTLD_NOW);
        if (h) real_v = (int (*)(int, const char *, const char *,
                                 __builtin_va_list))dlsym(h, "__android_log_vprint");
    }
    if (g_trace && count < 1500 && fmt &&
        wanted_caller(__builtin_return_address(0)) && !seen_too_often(fmt)) {
        unsigned long v[8];
        char line[480], piece[64];
        int i, n = 0;
        __builtin_va_list c;
        __builtin_va_start(c, fmt);
        for (i = 0; i < 8; i++) v[i] = __builtin_va_arg(c, unsigned long);
        __builtin_va_end(c);
        line[0] = 0;
        if (grab_text(tag, piece, (int)sizeof piece)) {
            s_cat(line, sizeof line, &n, piece);
            s_cat(line, sizeof line, &n, " | ");
        }
        fmt_expand(line, (int)sizeof line, &n, fmt, v, 8);
        count++;
        log_line("alog   ", line, "");
    }
    if (real_v) {
        __builtin_va_start(ap, fmt);
        r = real_v(prio, tag, fmt, ap);
        __builtin_va_end(ap);
    }
    return r;
}
