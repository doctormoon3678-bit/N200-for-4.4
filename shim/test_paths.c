/*
 * Host self-test for n200fs.c -- see shim/selftest.sh, which builds the fixture
 * tree and runs this once per scenario.
 *
 * n200fs.c is compiled into this program with its entry points renamed
 * (-Dfopen=n200_fopen and so on), so the real libc is still available here and
 * the redirector can be called directly and compared against it. Nothing in
 * n200fs.c is stubbed or conditionally compiled out: what runs here is the
 * same code that goes into libn200fs.so, including its load-time constructor,
 * which is why the fixture has to exist before the process starts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

void *n200_fopen(const char *, const char *);
int   n200_open(const char *, int, ...);
int   n200_stat(const char *, void *);
int   n200_access(const char *, int);
void *n200_opendir(const char *);
int   closedir(void *);
int   _Z22NaviGetNaviSDCardStatev(void);
int   _ZN14NVDebugMessage8traceMsgEiPKcS1_S1_z(void *, void *, void *, void *, ...);
int   n200_mkdir(const char *, unsigned);
int   n200_unlink(const char *);
int   n200_rename(const char *, const char *);
int   n200_system(const char *);

/* The shim logs through liblog; on the host just print, so a failing run shows
 * which roots it picked. */
int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    (void)prio;
    fprintf(stderr, "    [%s] ", tag);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return 0;
}

static int failures;
static const char *CARD, *WRITE;
static int same_root;        /* no card: reads and writes share a directory */

static void ck(int cond, const char *what)
{
    printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) failures++;
}

void check_gps(void (*ck)(int, const char *), void (*cks)(int, const char *));

static void ck_skip_named(int cond, const char *what)
{
    (void)cond;
    printf("  %-5s %s\n", "skip", what);
}

/* A check that only means something when the two roots are distinct. */
static void ck_split(int cond, const char *what)
{
    if (same_root) {
        printf("  %-5s %s\n", "skip", what);
        return;
    }
    ck(cond, what);
}

static int slurp(const char *path, char *buf, int cap)
{
    FILE *f = fopen(path, "r");          /* the real fopen, not the shim's */
    int n;
    if (!f) return -1;
    n = (int)fread(buf, 1, (size_t)cap - 1, f);
    fclose(f);
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

static int exists(const char *fmt, const char *a, const char *b)
{
    char p[1024];
    struct stat st;
    snprintf(p, sizeof p, fmt, a, b);
    return stat(p, &st) == 0;
}

int main(int argc, char **argv)
{
    char buf[256];
    FILE *f;
    int fd;

    CARD  = getenv("N200T_CARD");        /* where the map tree really is */
    WRITE = getenv("N200T_WRITE");       /* where writes must land       */
    if (!CARD || !WRITE) {
        fprintf(stderr, "N200T_CARD / N200T_WRITE not set\n");
        return 2;
    }
    same_root = strcmp(CARD, WRITE) == 0;
    printf("== %s\n", argc > 1 ? argv[1] : "(scenario)");

    /* 1. a read of the map database reaches the card, through three slashes */
    f = n200_fopen("/mnt/extSdCard///Navkingdom/Common/DB_MAP/NF60.sti", "r");
    ck(f != NULL, "fopen(map, \"r\") returns a stream");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        ck(strcmp(buf, "MAPDATA") == 0, "  and it is the file on the card");
    } else {
        ck(0, "  and it is the file on the card");
    }

    /* 2. the crash case: a write whose directory does not exist yet must still
     *    hand back a usable stream, because the app never checks for NULL */
    f = n200_fopen("/mnt/extSdCard///Navkingdom/LoggerServerPID", "w");
    ck(f != NULL, "fopen(LoggerServerPID, \"w\") is NOT NULL (the tombstone)");
    if (f) { fputs("1234", f); fclose(f); }
    ck(exists("%s/Navkingdom/LoggerServerPID", WRITE, 0),
       "  and it was created under the write root");
    ck_split(!exists("%s/Navkingdom/LoggerServerPID", CARD, 0),
             "  and nothing was written to the card");

    /* 3. parents are created several levels deep */
    f = n200_fopen("/mnt/extSdCard///Navkingdom/Persistence/SD/player_Rental.bak", "w");
    ck(f != NULL, "fopen() three directories deep creates the parents");
    if (f) { fputs("x", f); fclose(f); }

    /* 4. State the app wrote must win over whatever the card shipped, or
     *    settings and saved routes would reset at every start. */
    f = n200_fopen("/mnt/extSdCard///Navkingdom/Persistence/HMI_Persistence.bak", "w");
    if (f) { fputs("MYSTATE", f); fclose(f); }
    f = n200_fopen("/mnt/extSdCard///Navkingdom/Persistence/HMI_Persistence.bak", "r");
    buf[0] = 0;
    if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
    ck(strcmp(buf, "MYSTATE") == 0, "written Persistence state is read back");

    /* 5. ...but read-only payload must come from the card even when a stale
     *    copy is sitting in the write root. This is the case that made a
     *    leftover internal tree shadow the real map data. */
    if (!same_root) {
        snprintf(buf, sizeof buf, "%s/Navkingdom/Navi/CONF", WRITE);
        n200_mkdir("/mnt/extSdCard///Navkingdom/Navi/CONF", 0775);
        snprintf(buf, sizeof buf, "%s/Navkingdom/Navi/CONF/naviConfig_N200.txt", WRITE);
        { FILE *s = fopen(buf, "w"); if (s) { fputs("STALE", s); fclose(s); } }
        f = n200_fopen("/mnt/extSdCard///Navkingdom/Navi/CONF/naviConfig_N200.txt", "r");
        buf[0] = 0;
        if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
        ck(strcmp(buf, "cfg") == 0,
           "Navkingdom/Navi comes from the card, not a stale internal copy");
    } else {
        ck_split(0, "Navkingdom/Navi comes from the card, not a stale copy");
    }
    ck_split(slurp(
        (snprintf(buf, sizeof buf, "%s/Navkingdom/Common/DB_MAP/NF60.sti", CARD),
         buf), buf, sizeof buf) >= 0 && strcmp(buf, "MAPDATA") == 0,
             "  and the card copy is untouched");

    /* 5. open(2): O_RDONLY reads through, O_CREAT writes to the write root */
    fd = n200_open("/mnt/extSdCard///Navkingdom/Navi/CONF/naviConfig_N200.txt",
                   O_RDONLY, 0);
    ck(fd >= 0, "open(config, O_RDONLY) finds it on the card");
    if (fd >= 0) close(fd);
    fd = n200_open("/mnt/extSdCard///Navkingdom/meminfo.txt",
                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ck(fd >= 0, "open(O_CREAT) succeeds");
    if (fd >= 0) close(fd);
    ck(exists("%s/Navkingdom/meminfo.txt", WRITE, 0),
       "  and lands under the write root");

    /* 6. directory and metadata calls */
    ck(n200_mkdir("/mnt/extSdCard///Navkingdom/Persistence/CarDialy", 0775) == 0 ||
       exists("%s/Navkingdom/Persistence/CarDialy", WRITE, 0),
       "mkdir() creates under the write root");
    ck(n200_access("/mnt/extSdCard///Navkingdom/Common/DB_MAP/NF60.sti", F_OK) == 0,
       "access(F_OK) resolves through the overlay");
    {
        void *dir = n200_opendir("/mnt/extSdCard///Navkingdom");
        ck(dir != NULL, "opendir() resolves");
        if (dir) closedir(dir);
    }
    ck(n200_stat("/mnt/extSdCard///Navkingdom/Navi/CONF/naviConfig_N200.txt",
                 (void *)buf) == 0,
       "stat() resolves to the card");

    /* 7. unlink and rename stay on the write root */
    ck(n200_unlink("/mnt/extSdCard///Navkingdom/meminfo.txt") == 0,
       "unlink() removes the written copy");
    ck(n200_rename("/mnt/extSdCard///Navkingdom/LoggerServerPID",
                   "/mnt/extSdCard///Navkingdom/UpdateCheck.dat") == 0,
       "rename() moves within the write root");

    /* 8. system(): the root is rewritten inside the command text, and the
     *    directory a redirection writes into is created first */
    ck(n200_system("echo hi > /mnt/extSdCard///Navkingdom/Persistence/probe.txt") == 0,
       "system() redirection into a missing directory works");
    ck(exists("%s/Navkingdom/Persistence/probe.txt", WRITE, 0),
       "  and the file is under the write root");
    ck(n200_system("rm -rf /mnt/extSdCard///Navkingdom/Persistence/probe.txt") == 0,
       "system(\"rm -rf ...\") is rewritten too");
    ck(!exists("%s/Navkingdom/Persistence/probe.txt", WRITE, 0),
       "  and it removed the right file");

    /* 9. the on-device diagnostic: whoever installs this may have a file
     *    manager rather than adb, so the roots are left in a file too */
    snprintf(buf, sizeof buf, "%s/n200fs.log", WRITE);
    {
        char log[1024];
        int n = slurp(buf, log, sizeof log);
        ck(n > 0, "n200fs.log is written next to the data");
        ck(n > 0 && strstr(log, CARD) != NULL,
           "  and names the read root");
        ck(n > 0 && strstr(log, "mounted storage") != NULL &&
           strstr(log, "unreadable") == NULL,
           "  and lists the mount points, whichever root was chosen");
    }

    /* 9b. A file in neither root. The app will not check the NULL it gets,
     *     so the library has to be the thing that records it. */
    f = n200_fopen("/mnt/extSdCard///Navkingdom/Common/DB_MAP/NOT_THERE.sti", "r");
    ck(f == NULL, "a file in neither root still fails, as it must");
    if (f) fclose(f);
    snprintf(buf, sizeof buf, "%s/n200fs.log", WRITE);
    {
        char log[2048];
        int n = slurp(buf, log, sizeof log);
        ck(n > 0 && strstr(log, "FAILED") != NULL,
           "  and the failure is recorded in n200fs.log");
        ck(n > 0 && strstr(log, "NOT_THERE.sti") != NULL,
           "  naming the file the app asked for");
    }

    /* 9c. The other roots the engine builds at runtime, and the Windows CE
     *     spellings in the binaries. Every path here is one a device log
     *     caught the app opening and this library not redirecting. */
    fd = n200_open("/storage/sdcard1/Navkingdom/Common/DB_MAP/NF60.sti", O_RDONLY, 0);
    ck(fd >= 0, "/storage/sdcard1 is redirected to the card");
    if (fd >= 0) close(fd);

    f = n200_fopen("/storage/sdcard1/Navkingdom/Common/DB_MAP\\NF60.sti", "r");
    ck(f != NULL, "  and a backslash in it is treated as a separator");
    if (f) fclose(f);

    f = n200_fopen("/storage/sdcard0/Navkingdom/Persistence/usermark.bak", "w");
    ck(f != NULL, "/storage/sdcard0 Persistence writes are redirected");
    if (f) { fputs("x", f); fclose(f); }
    ck(exists("%s/Navkingdom/Persistence/usermark.bak", WRITE, 0),
       "  and land in the write root, with everything else");

    f = n200_fopen("\\Storage Card\\Navkingdom\\Navi\\CONF\\naviConfig_N200.txt", "r");
    ck(f != NULL, "the Windows CE \"\\\\Storage Card\" spelling resolves too");
    if (f) fclose(f);

    /* A /storage/sdcard0 path that is NOT the app's data must be left alone,
     * because on some devices that really is primary storage. */
    ck(n200_fopen("/storage/sdcard0/DCIM/somebodys_photo.jpg", "r") == NULL,
       "but /storage/sdcard0 outside Navkingdom is not touched");

    /* 10. paths that are not the app's root must be passed through untouched */
    f = n200_fopen("/etc/hostname", "r");
    ck(f != NULL, "an unrelated absolute path is not redirected");
    if (f) fclose(f);
    ck(n200_fopen("/mnt/extSdCardOther/x", "r") == NULL,
       "a path that merely starts with the prefix is not matched");

    /* The "preparing navigation" gate, and the engine trace capture. */
    ck(_Z22NaviGetNaviSDCardStatev() == 1,
       "NaviGetNaviSDCardState() reports the card as present");

    /* A realistic call, and then a hostile one: integer arguments in pointer
     * slots must not fault the process. */
    _ZN14NVDebugMessage8traceMsgEiPKcS1_S1_z(
        (void *)0, (void *)"NaviReadyScene.cpp", (void *)"OnTimerEvent",
        (void *)"[CNaviReadyScene::OnTimerEvent] NaviGetNaviSDCardState(%d)"
                " bDBLoadInit(%d) - TIMER_ID_CNAVIMODE_READY",
        (void *)0, (void *)1);
    _ZN14NVDebugMessage8traceMsgEiPKcS1_S1_z(
        (void *)1, (void *)2, (void *)3, (void *)0xdeadbeef,
        (void *)0xffffffff, (void *)0x7);
    _ZN14NVDebugMessage8traceMsgEiPKcS1_S1_z(
        (void *)0, (void *)"x.cpp", (void *)"f",
        (void *)"something entirely unremarkable happened", 0, 0);
    ck(1, "  engine trace hooks survive integer arguments in pointer slots");
    snprintf(buf, sizeof buf, "%s/n200fs.log", WRITE);
    {
        char log[8192];
        int n = slurp(buf, log, sizeof log);
        ck(n > 0 && strstr(log, "NaviGetNaviSDCardState(0) bDBLoadInit(1)") != NULL,
           "  and the gate's own trace line is captured");
        ck(n > 0 && strstr(log, "unremarkable") == NULL,
           "  while uninteresting messages are filtered out");
    }

    check_gps(ck, ck_skip_named);

    printf("%s: %d failure(s)\n\n", argc > 1 ? argv[1] : "scenario", failures);
    return failures ? 1 : 0;
}

/* ---------------------------------------------------------------------- *
 * The synthetic GPS feed, validated the way libNV_UART.so would: every
 * sentence checksummed, GPRMC present and flagged valid, coordinates
 * matching the configured position. Nothing is stubbed -- the real thread
 * writes into the real FIFO path and this reads it back out.
 * ---------------------------------------------------------------------- */
#define N200_FIFO "/data/data/com.npnd.n200/files/gpsfifo"

static int nmea_ck_ok(const char *line, int len)
{
    int i;
    unsigned char ck = 0;
    if (len < 6 || line[0] != '$') return 0;
    for (i = 1; i < len && line[i] != '*'; i++) ck ^= (unsigned char)line[i];
    if (i + 2 >= len + 1 || line[i] != '*') return 0;
    {
        char want[3];
        snprintf(want, sizeof want, "%02X", ck);
        return line[i + 1] == want[0] && line[i + 2] == want[1];
    }
}

void check_gps(void (*ck)(int, const char *),
               void (*cks)(int, const char *))
{
    char buf[4096];
    int fd, total = 0, tries, nlines = 0, bad = 0;
    int saw_rmc = 0, saw_gga = 0, saw_valid = 0, saw_coord = 0;
    /* Open it the way libNV_UART.so does, through the redirector. There is
     * deliberately no FIFO on disk: the point is that a pipe comes back
     * anyway, so the feed no longer depends on mknod having worked. */
    fd = n200_open(N200_FIFO, O_RDONLY, 0);
    ck(fd >= 0, "the engine's open() of the gpsfifo succeeds with no FIFO on disk");
    if (fd < 0) {
        cks(0, "  synthetic NMEA arrives through that fd");
        return;
    }
    for (tries = 0; tries < 60 && total < 400; tries++) {
        int n = (int)read(fd, buf + total, sizeof buf - 1 - (size_t)total);
        if (n > 0) total += n;
        else usleep(100000);
    }
    close(fd);
    buf[total > 0 ? total : 0] = 0;

    {
        char *p = buf, *e;
        while ((e = strstr(p, "\r\n")) != NULL) {
            int len = (int)(e - p);
            if (len > 0) {
                nlines++;
                if (!nmea_ck_ok(p, len)) bad++;
                if (!strncmp(p, "$GPRMC,", 7)) {
                    saw_rmc = 1;
                    if (strstr(p, ",A,") && strstr(p, "3733.9900") &&
                        strstr(p, "12658.6800"))
                        saw_valid = 1;
                    if (strstr(p, "3733.9900,N") && strstr(p, "12658.6800,E"))
                        saw_coord = 1;
                }
                if (!strncmp(p, "$GPGGA,", 7)) saw_gga = 1;
            }
            p = e + 2;
        }
    }
    ck(nlines >= 5, "  synthetic NMEA arrives through that fd");
    ck(bad == 0, "  every sentence's checksum is correct");
    ck(saw_rmc, "  GPRMC is present (libNV_UART refuses to work without it)");
    ck(saw_gga, "  GPGGA is present");
    ck(saw_valid, "  GPRMC is flagged valid at the configured position");
    ck(saw_coord, "  and the coordinates convert correctly to ddmm.mmmm");
}
