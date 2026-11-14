#ifndef ATOMIC_FILE_H
#define ATOMIC_FILE_H

/* Replace a file on flash so that it is either the old or the new content, never empty or cut.
 *
 * The data goes to a temporary file in the same directory (named after the path, the process and
 * the thread, so concurrent writers never share it), every write is checked, the file is synced
 * and closed, and only then renamed over the old one. On any failure the temporary file is removed,
 * the old file stays as it was and -1 is returned: callers must then report the save as failed.
 * The new file keeps the old file's permission bits (0644 for a new file).
 *
 * Header only, so every daemon (hub_daemon, hub_rf, hub_btstack, hub_util) can use it without a
 * change to its source list. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static inline int atomic_file_write(const char *path, const void *data, size_t len) {
    char tmp[320];
    struct stat st;
    if (!path || (!data && len > 0)) return -1;
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%ld", path, (int)getpid(), (long)syscall(SYS_gettid));
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    mode_t mode = (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? (st.st_mode & 07777) : 0644;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    const char *p = (const char *)data;
    size_t left = len;
    int failed = 0;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            failed = 1;
            break;
        }
        p += w;
        left -= (size_t)w;
    }
    if (!failed && fchmod(fd, mode) != 0) failed = 1;
    if (!failed && fsync(fd) != 0) failed = 1;
    if (close(fd) != 0) failed = 1;
    if (failed || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* NUL-terminated text */
static inline int atomic_file_write_str(const char *path, const char *text) {
    if (!text) return -1;
    return atomic_file_write(path, text, strlen(text));
}

#endif /* ATOMIC_FILE_H */
