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

    /* 4. overlay: once written, a read finds the written copy, not the card's */
    f = n200_fopen("/mnt/extSdCard///Navkingdom/Common/DB_MAP/NF60.sti", "w");
    if (f) { fputs("LOCAL", f); fclose(f); }
    f = n200_fopen("/mnt/extSdCard///Navkingdom/Common/DB_MAP/NF60.sti", "r");
    buf[0] = 0;
    if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
    ck(strcmp(buf, "LOCAL") == 0, "a written file shadows the card copy on read");
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
    ck(n200_opendir("/mnt/extSdCard///Navkingdom") != NULL,
       "opendir() resolves");
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

    /* 10. paths that are not the app's root must be passed through untouched */
    f = n200_fopen("/etc/hostname", "r");
    ck(f != NULL, "an unrelated absolute path is not redirected");
    if (f) fclose(f);
    ck(n200_fopen("/mnt/extSdCardOther/x", "r") == NULL,
       "a path that merely starts with the prefix is not matched");

    printf("%s: %d failure(s)\n\n", argc > 1 ? argv[1] : "scenario", failures);
    return failures ? 1 : 0;
}
