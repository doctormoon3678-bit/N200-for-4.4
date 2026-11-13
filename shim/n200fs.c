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

/* Append one line to n200fs.log, next to the data. */
static void log_line(const char *a, const char *b, const char *c)
{
    char p[PATHMAX];
    int fd;
    if (!r_open || !r_write || !g_write[0]) return;
    if (!join(p, sizeof p, g_write, "n200fs.log")) return;
    /* O_WRONLY|O_CREAT|O_APPEND */
    fd = r_open(p, 1 | 0100 | 02000, 0644);
    if (fd < 0) return;
    if (a) r_write(fd, a, (u32)s_len(a));
    if (b) r_write(fd, b, (u32)s_len(b));
    if (c) r_write(fd, c, (u32)s_len(c));
    r_write(fd, "\n", 1);
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
    if (count >= 120) return;
    count++;
    log_line("opened ", what, orig);
    if (count == 120)
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
