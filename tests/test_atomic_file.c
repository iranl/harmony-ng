/* atomic_file.h: the file is the old or the new content, never cut, and failures are reported.
 * A file size limit (RLIMIT_FSIZE, SIGXFSZ ignored) stands in for a full flash. */
#include "atomic_file.h"

#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>

static int failed;

#define CHECK(cond, name)                                         \
    do {                                                          \
        if (cond) {                                               \
            printf("  PASS %s\n", name);                          \
        } else {                                                  \
            printf("  FAIL %s (line %d)\n", name, __LINE__);      \
            failed++;                                             \
        }                                                         \
    } while (0)

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int read_eq(const char *path, const char *want) {
    char buf[256] = {0};
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return n == strlen(want) && memcmp(buf, want, n) == 0;
}

static int entries(const char *dir) {
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    }
    closedir(d);
    return n;
}

int main(void) {
    char dir[] = "/tmp/atomic_file_testXXXXXX";
    char path[128];
    printf("[-] atomic_file_write\n");
    if (!mkdtemp(dir)) return 1;
    snprintf(path, sizeof(path), "%s/devices.json", dir);

    CHECK(atomic_file_write_str(path, "old content\n") == 0, "new file written");
    CHECK(read_eq(path, "old content\n"), "new file content");
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0644, "new file mode 0644");

    chmod(path, 0600);
    CHECK(atomic_file_write_str(path, "second\n") == 0 && read_eq(path, "second\n"), "replaced");
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "old file mode kept");

    /* Full flash: writes past 1000 bytes fail with EFBIG. Soft limit only, raised again below:
     * stdout may be a log file larger than the limit. */
    fflush(stdout);
    signal(SIGXFSZ, SIG_IGN);
    struct rlimit saved, rl;
    getrlimit(RLIMIT_FSIZE, &saved);
    rl = saved;
    rl.rlim_cur = 1000;
    setrlimit(RLIMIT_FSIZE, &rl);
    char *big = malloc(5000);
    memset(big, 'x', 4999);
    big[4999] = '\0';
    int past = atomic_file_write_str(path, big) == -1;
    int kept = read_eq(path, "second\n") && file_size(path) == 7;
    int no_tmp = entries(dir) == 1;
    int within = atomic_file_write(path, big, 900) == 0 && file_size(path) == 900;
    setrlimit(RLIMIT_FSIZE, &saved);
    free(big);
    CHECK(past, "write past the limit fails");
    CHECK(kept, "old file kept on failure");
    CHECK(no_tmp, "temporary file removed on failure");
    CHECK(within, "write within the limit");

    CHECK(atomic_file_write_str(NULL, "x") == -1 && atomic_file_write_str(path, NULL) == -1, "NULL refused");
    snprintf(path, sizeof(path), "%s/missing_dir/file", dir);
    CHECK(atomic_file_write_str(path, "x") == -1, "missing directory reported");

    printf("[-] symbolic link written through\n");
    char real[128], link[128];
    snprintf(real, sizeof(real), "%s/real.conf", dir);
    snprintf(link, sizeof(link), "%s/link.conf", dir);
    CHECK(atomic_file_write_str(real, "target\n") == 0 && symlink(real, link) == 0, "link made");
    CHECK(atomic_file_write_str(link, "through\n") == 0, "write to the link");
    struct stat lst;
    CHECK(lstat(link, &lst) == 0 && S_ISLNK(lst.st_mode), "link is still a link");
    CHECK(read_eq(real, "through\n"), "target got the new content");

    printf("[-] atomic_file_write_pair\n");
    char first[128], second[128];
    snprintf(first, sizeof(first), "%s/wpa.conf", dir);
    snprintf(second, sizeof(second), "%s/wpa.ath0.conf", dir);
    CHECK(atomic_file_write_pair(first, second, "net=1\n", 16384) == 0 &&
          read_eq(first, "net=1\n") && read_eq(second, "net=1\n"), "both files written");
    snprintf(second, sizeof(second), "%s/missing_dir/wpa.ath0.conf", dir);
    CHECK(atomic_file_write_pair(first, second, "net=2\n", 16384) == -1, "second write failing reported");
    CHECK(read_eq(first, "net=1\n"), "first file back to its old content");
    char fresh[128];
    snprintf(fresh, sizeof(fresh), "%s/fresh.conf", dir);
    CHECK(atomic_file_write_pair(fresh, second, "net=3\n", 16384) == -1 && access(fresh, F_OK) != 0,
          "first file that did not exist is removed again");

    snprintf(path, sizeof(path), "rm -rf %s", dir);
    if (system(path) != 0) return 1;
    if (failed) {
        printf("atomic_file: %d FAILED\n", failed);
        return 1;
    }
    printf("[+] atomic_file: all passed\n");
    return 0;
}
