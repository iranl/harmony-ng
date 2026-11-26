/*
 * /init built into the Linux 6.18 kernel image of the Hub (step 3b).
 *
 * The kernel slot (1.875 MB) leaves no room for a userland, so this only mounts the two JFFS2
 * partitions the stock firmware already has and hands over to a script there, which can be
 * changed from the stock system without writing the kernel again:
 *
 *   /cache  mtd:cache (BusyBox in /cache/bin, our files in /cache/kernel6)
 *   /data   mtd:data  (the stock writable layer: /data/data/harmony, configuration)
 *   then    /cache/bin/busybox sh /cache/kernel6/rc.sh
 *
 * When something is missing, a BusyBox shell (or, without BusyBox, nothing) is started on the
 * console instead.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUSYBOX "/cache/bin/busybox"
#define RC "/cache/kernel6/rc.sh"

static void msg(const char *what, const char *arg, int err) {
    dprintf(2, "init: %s %s%s%s\n", what, arg, err ? ": " : "", err ? strerror(err) : "");
}

static void mnt(const char *src, const char *dst, const char *type, unsigned long flags) {
    mkdir(dst, 0755);
    if (mount(src, dst, type, flags, NULL) != 0)
        msg("cannot mount", dst, errno);
}

int main(void) {
    mnt("proc", "/proc", "proc", 0);
    mnt("sys", "/sys", "sysfs", 0);
    mnt("dev", "/dev", "devtmpfs", 0);
    mnt("tmp", "/tmp", "tmpfs", 0);

    int fd = open("/dev/console", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) close(fd);
    }

    mnt("mtd:cache", "/cache", "jffs2", 0);
    mnt("mtd:data", "/data", "jffs2", 0);

    /* the kernel loads modules on demand through this path */
    fd = open("/proc/sys/kernel/modprobe", O_WRONLY);
    if (fd >= 0) {
        if (write(fd, "/tmp/bin/modprobe\n", 18) < 0) msg("cannot set", "modprobe path", errno);
        close(fd);
    }

    if (access(RC, R_OK) == 0) {
        execl(BUSYBOX, "sh", RC, (char *)NULL);
        msg("cannot run", BUSYBOX, errno);
    } else {
        msg("missing", RC, errno);
    }
    execl(BUSYBOX, "sh", (char *)NULL);
    msg("cannot run", BUSYBOX, errno);
    for (;;)
        pause();
}
