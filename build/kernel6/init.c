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
 * A new root file system waiting in /cache/kernel6/rootfs.sqfs (update.sh, the migration and
 * deploy tools put it there) is written to mtd "root" first, by init itself: static, it needs no
 * C library, while everything else on the Hub links the root file system's shared uClibc-ng.
 * Written, the file goes; refused or failed, it becomes rootfs.sqfs.failed.
 *
 * Fallback boot (the root partition holds no XZ squashfs, or its boot failed): /cache and
 * /mnt/data mounted, /cache/bin/busybox sh /cache/kernel6/rc.sh (a static BusyBox: rescue only).
 *
 * Data and cache on a USB stick (build/kernel6/usb_storage.sh, set up by the user): with
 * /cache/kernel6/usb-storage (the stick's UUID) the root file system's usb_storage.sh, chrooted in
 * /rom, brings up USB host mode and mounts the stick, which init moves to /usb. Its data/ is the
 * overlay's upper layer and its cache/ becomes /cache, with the flash's /cache (now /flash) bound
 * over /cache/kernel6: the boot state stays on flash. Without the stick: the flash data and
 * /cache/kernel6/usb-missing, for which rc.sh starts no HarmonyNG (rescue).
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
#include <sys/wait.h>
#include <unistd.h>

#include "flash_image.h"

#define BUSYBOX "/cache/bin/busybox"
#define RC "/cache/kernel6/rc.sh"
#define RESET_FLAG "/cache/kernel6/factory-reset"
#define PENDING_ROOT "/cache/kernel6/rootfs.sqfs"
#define USB_FLAG "/cache/kernel6/usb-storage"
#define USB_MISSING "/cache/kernel6/usb-missing"

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

static int mtd_erase(int fd, uint32_t off, uint32_t len) {
    struct erase_info_user ei = { .start = off, .length = len };
    return ioctl(fd, MEMERASE, &ei);
}

static long mtd_pread(int fd, void *buf, uint32_t len, uint32_t off) {
    return pread(fd, buf, len, off);
}

static long mtd_pwrite(int fd, const void *buf, uint32_t len, uint32_t off) {
    return pwrite(fd, buf, len, off);
}

/* writes PENDING_ROOT (an XZ squashfs that fits) to mtd "root", block by block with read-back */
static void write_pending_root(void) {
    static const struct flash_ops ops = { mtd_erase, mtd_pread, mtd_pwrite };
    char dev[32], err[128] = "";
    struct mtd_info_user mi;
    struct flash_part part;
    struct stat st;
    uint8_t *img = NULL;
    int n = mtd_num("root"), fd = -1, mfd = -1, written = 0, ok = 0;

    if (stat(PENDING_ROOT, &st) != 0) return;
    msg("writing", PENDING_ROOT, 0);
    snprintf(dev, sizeof(dev), "/dev/mtd%d", n);
    if (n < 0 || (mfd = open(dev, O_RDWR)) < 0 || ioctl(mfd, MEMGETINFO, &mi) != 0) {
        snprintf(err, sizeof(err), "no root partition");
    } else if ((uint32_t)st.st_size > mi.size) {
        snprintf(err, sizeof(err), "larger than the root partition");
    } else if (!(img = malloc(st.st_size)) || (fd = open(PENDING_ROOT, O_RDONLY)) < 0 ||
               read(fd, img, st.st_size) != st.st_size) {
        snprintf(err, sizeof(err), "cannot read it");
    } else if (squashfs_check(img, st.st_size, SQUASHFS_XZ, err, sizeof(err)) == 0) {
        part.fd = mfd;
        part.size = mi.size;
        part.erasesize = mi.erasesize;
        ok = flash_write_image(&part, 1, &ops, img, st.st_size, &written, err, sizeof(err)) == 0;
    }
    if (fd >= 0) close(fd);
    if (mfd >= 0) close(mfd);
    free(img);
    if (ok) {
        unlink(PENDING_ROOT);
        dprintf(2, "init: root file system written (%d blocks)\n", written);
    } else {
        rename(PENDING_ROOT, PENDING_ROOT ".failed");
        msg("root file system not written:", err, 0);
    }
    sync();
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

/* the stick of USB_FLAG on /usb, through usb_storage.sh boot chrooted in /rom (its BusyBox,
 * modprobe and drivers): 0 when it is there, -1 when not */
static int usb_storage(void) {
    static char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
    const char *binds[] = { "/proc", "/sys", "/dev" };
    char uuid[64] = "", dst[16];
    int st = -1, fd;
    pid_t pid;

    fd = open(USB_FLAG, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, uuid, sizeof(uuid) - 1);
        uuid[n > 0 ? n : 0] = 0;
        close(fd);
    }
    uuid[strcspn(uuid, "\r\n")] = 0;
    if (!uuid[0]) return -1;
    for (int i = 0; i < 3; i++) {
        snprintf(dst, sizeof(dst), "/rom%s", binds[i]);
        if (mount(binds[i], dst, NULL, MS_BIND, NULL) != 0) msg("cannot bind", dst, errno);
    }
    pid = fork();
    if (pid == 0) {
        if (chroot("/rom") == 0 && chdir("/") == 0)
            execle("/bin/sh", "sh", "/usr/lib/kernel6/usb_storage.sh", "boot", uuid, (char *)NULL, env);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, &st, 0);
    for (int i = 2; i >= 0; i--) {
        snprintf(dst, sizeof(dst), "/rom%s", binds[i]);
        umount2(dst, MNT_DETACH);
    }
    if (pid < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        msg("USB storage:", "the stick is not there, rescue boot from the flash", 0);
        return -1;
    }
    move_mount("/rom/mnt", "/usb");
    return 0;
}

static int root_boot(int root_mtd) {
    char dev[32];
    struct stat st;
    snprintf(dev, sizeof(dev), "/dev/mtdblock%d", root_mtd);
    const char *layers = "lowerdir=/rom,upperdir=/overlay/upper,workdir=/overlay/work";
    int usb = -1;
    if (mnt(dev, "/rom", "squashfs", MS_RDONLY, NULL) != 0) return -1;
    if (access(USB_FLAG, F_OK) == 0) {
        usb = usb_storage();
        if (usb == 0) {
            unlink(USB_MISSING);
        } else {
            int fd = open(USB_MISSING, O_WRONLY | O_CREAT, 0644);
            if (fd >= 0) close(fd);
        }
        sync();
    }
    if (usb == 0) {
        layers = "lowerdir=/rom,upperdir=/usb/data/upper,workdir=/usb/data/work";
    } else {
        if (mnt("mtd:data", "/overlay", "jffs2", 0, NULL) != 0) {
            umount("/rom");
            return -1;
        }
        if (stat("/overlay/upper", &st) != 0) migrate_data();
        mkdir("/overlay/upper", 0755);
        mkdir("/overlay/work", 0755);
    }
    if (mnt("overlay", "/newroot", "overlay", 0, layers) != 0) {
        /* the fallback boot mounts mtd:data itself */
        if (usb == 0) umount("/usb");
        else umount("/overlay");
        umount("/rom");
        return -1;
    }
    if (usb == 0) {
        /* the stick's cache/ is /cache, the flash's boot state stays at /cache/kernel6 */
        move_mount("/cache", "/flash");
        mkdir("/usb/cache/kernel6", 0755);
        if (mount("/usb/cache", "/cache", NULL, MS_BIND, NULL) != 0 ||
            mount("/flash/kernel6", "/cache/kernel6", NULL, MS_BIND, NULL) != 0)
            msg("cannot bind", "/cache", errno);
        move_mount("/usb", "/newroot/usb");
        move_mount("/flash", "/newroot/flash");
    } else {
        move_mount("/overlay", "/newroot/overlay");
    }
    move_mount("/rom", "/newroot/rom");
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

    write_pending_root();
    root_mtd = mtd_num("root");
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
