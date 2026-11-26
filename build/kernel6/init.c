/*
 * /init built into the Linux 6.18 kernel image of the Hub.
 *
 * Root file system boot (mtd "root" holds an XZ squashfs, build/build_rootfs6.sh):
 *   /rom      the squashfs, read-only (lower layer)
 *   /overlay  mtd:data, JFFS2 (upper layer: /overlay/upper, work dir /overlay/work)
 *   /         overlayfs of the two, as the stock kernel's unionfs did (dirs=/mnt/data=rw:/mnt/root=ro)
 *   then      /bin/sh /etc/init.d/rcS (build/kernel6/rc.sh) as PID 1
 * The first boot moves what the data partition holds (the stock union's writable layer) into
 * /overlay/upper.
 *
 * Fallback boot (the root partition still holds the stock root, or no squashfs, or a new root
 * file system waits in /cache/kernel6/rootfs.sqfs for rc.sh to write it): /cache and /mnt/data
 * mounted, /cache/bin/busybox sh /cache/kernel6/rc.sh.
 *
 * Factory reset: with /cache/kernel6/factory-reset present (rc.sh sets it when the Pair button is
 * held at boot) the data and cache partitions are erased before anything is mounted from them.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUSYBOX "/cache/bin/busybox"
#define RC "/cache/kernel6/rc.sh"
#define RESET_FLAG "/cache/kernel6/factory-reset"
#define PENDING_ROOT "/cache/kernel6/rootfs.sqfs"

static void msg(const char *what, const char *arg, int err) {
    dprintf(2, "init: %s %s%s%s\n", what, arg, err ? ": " : "", err ? strerror(err) : "");
}

static int mnt(const char *src, const char *dst, const char *type, unsigned long flags,
               const char *data) {
    mkdir(dst, 0755);
    if (mount(src, dst, type, flags, data) == 0) return 0;
    msg("cannot mount", dst, errno);
    return -1;
}

/* mtd number of the partition with this name (/proc/mtd), or -1 */
static int mtd_num(const char *name) {
    FILE *f = fopen("/proc/mtd", "r");
    char line[128], want[48];
    int n, found = -1;
    if (!f) return -1;
    snprintf(want, sizeof(want), "\"%s\"", name);
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "mtd%d:", &n) == 1 && strstr(line, want)) {
            found = n;
            break;
        }
    fclose(f);
    return found;
}

/* erase a whole partition (empty JFFS2: the next mount starts fresh) */
static void erase_mtd(const char *name) {
    char dev[32];
    struct mtd_info_user mi;
    int n = mtd_num(name), fd;
    if (n < 0) return;
    snprintf(dev, sizeof(dev), "/dev/mtd%d", n);
    fd = open(dev, O_RDWR);
    if (fd < 0 || ioctl(fd, MEMGETINFO, &mi) != 0) {
        msg("cannot open", dev, errno);
        if (fd >= 0) close(fd);
        return;
    }
    for (unsigned int off = 0; off < mi.size; off += mi.erasesize) {
        struct erase_info_user ei = { .start = off, .length = mi.erasesize };
        if (ioctl(fd, MEMERASE, &ei) != 0) msg("erase failed:", dev, errno);
    }
    close(fd);
    msg("erased", name, 0);
}

/* the root partition holds a squashfs with XZ compression (ours, not the stock LZMA root) */
static int our_root(int n) {
    char dev[32];
    unsigned char sb[22];
    int fd, ok;
    snprintf(dev, sizeof(dev), "/dev/mtd%d", n);
    fd = open(dev, O_RDONLY);
    if (fd < 0) return 0;
    ok = read(fd, sb, sizeof(sb)) == (ssize_t)sizeof(sb) && memcmp(sb, "hsqs", 4) == 0 &&
         sb[20] == 4 && sb[21] == 0;   /* compression id 4 = XZ, little endian */
    close(fd);
    return ok;
}

/* first boot of the root file system: the data partition still has the stock union layout
 * (data/, etc/, ... at its top): all of it goes to upper/ (renames, no copy) */
static void migrate_data(void) {
    DIR *d = opendir("/overlay");
    struct dirent *e;
    char from[300], to[300];
    if (!d) return;
    mkdir("/overlay/upper", 0755);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, "upper") ||
            !strcmp(e->d_name, "work"))
            continue;
        snprintf(from, sizeof(from), "/overlay/%s", e->d_name);
        snprintf(to, sizeof(to), "/overlay/upper/%s", e->d_name);
        if (rename(from, to) != 0) msg("cannot move", from, errno);
    }
    closedir(d);
}

static void move_mount(const char *from, const char *to) {
    mkdir(to, 0755);
    if (mount(from, to, NULL, MS_MOVE, NULL) != 0) msg("cannot move mount", from, errno);
}

static int root_boot(int root_mtd) {
    char dev[32];
    struct stat st;
    snprintf(dev, sizeof(dev), "/dev/mtdblock%d", root_mtd);
    if (mnt(dev, "/rom", "squashfs", MS_RDONLY, NULL) != 0) return -1;
    if (mnt("mtd:data", "/overlay", "jffs2", 0, NULL) != 0) {
        umount("/rom");
        return -1;
    }
    if (stat("/overlay/upper", &st) != 0) migrate_data();
    mkdir("/overlay/upper", 0755);
    mkdir("/overlay/work", 0755);
    if (mnt("overlay", "/newroot", "overlay", 0,
            "lowerdir=/rom,upperdir=/overlay/upper,workdir=/overlay/work") != 0) {
        /* the fallback boot mounts mtd:data itself */
        umount("/overlay");
        umount("/rom");
        return -1;
    }
    move_mount("/rom", "/newroot/rom");
    move_mount("/overlay", "/newroot/overlay");
    move_mount("/cache", "/newroot/cache");
    move_mount("/dev", "/newroot/dev");
    move_mount("/proc", "/newroot/proc");
    move_mount("/sys", "/newroot/sys");
    move_mount("/tmp", "/newroot/tmp");
    /* switch_root: the new root becomes /, the initramfs stays behind (a few KB) */
    if (chdir("/newroot") != 0 || mount(".", "/", NULL, MS_MOVE, NULL) != 0 || chroot(".") != 0 ||
        chdir("/") != 0) {
        msg("cannot switch to", "/newroot", errno);
        return -1;
    }
    execl("/bin/sh", "sh", "/etc/init.d/rcS", (char *)NULL);
    msg("cannot run", "/etc/init.d/rcS", errno);
    return -1;
}

int main(void) {
    int fd, root_mtd;

    mnt("proc", "/proc", "proc", 0, NULL);
    mnt("sys", "/sys", "sysfs", 0, NULL);
    mnt("dev", "/dev", "devtmpfs", 0, NULL);
    mnt("tmp", "/tmp", "tmpfs", 0, NULL);

    fd = open("/dev/console", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) close(fd);
    }

    mnt("mtd:cache", "/cache", "jffs2", 0, NULL);
    if (access(RESET_FLAG, F_OK) == 0) {
        msg("factory reset:", "erasing data and cache", 0);
        umount("/cache");
        erase_mtd("data");
        erase_mtd("cache");
        mnt("mtd:cache", "/cache", "jffs2", 0, NULL);
    }

    /* a new root file system waiting in /cache: the fallback boot writes it (rc.sh) */
    root_mtd = access(PENDING_ROOT, F_OK) == 0 ? -1 : mtd_num("root");
    if (root_mtd >= 0 && our_root(root_mtd) && root_boot(root_mtd) == 0)
        return 0;   /* not reached: rcS runs */

    /* fallback boot */
    mkdir("/mnt", 0755);
    mnt("mtd:data", "/mnt/data", "jffs2", 0, NULL);
    fd = open("/proc/sys/kernel/modprobe", O_WRONLY);
    if (fd >= 0) {
        if (write(fd, "/bin/modprobe\n", 14) < 0) msg("cannot set", "modprobe path", errno);
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
