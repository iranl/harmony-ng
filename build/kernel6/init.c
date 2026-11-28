/*
 * /init built into the Linux 6.18 kernel image of the Hub.
 *
 * Root file system boot (mtd "root" holds an XZ squashfs, build/build_rootfs6.sh):
 *   /rom      the squashfs, read-only (lower layer)
 *   /overlay  mtd:data, JFFS2 (upper layer: /overlay/upper, work dir /overlay/work; /cache is
 *             its cache/ directory, bound there)
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
 * Flash layout: the stock data and cache partitions are one JFFS2 partition, "data" (10 MB less
 * the 64 KB "state" block at its end). A JFFS2 mount reads the whole partition, so the stock
 * cache file system in its second half must go before the first mount: while "state" does not
 * hold STATE_MAGIC, init erases that half and "state" once, then writes the magic. The install
 * tool (tools/install_kernel6.py) puts what must survive in cache-import/ at the data partition's
 * top (on a stock Hub: /cache-import of the union root); init moves it into cache/ (renames on
 * the same file system), the staged root file system included.
 *
 * Factory reset: with /cache/kernel6/factory-reset present (rc.sh sets it when the Pair button is
 * held at boot) the data partition is erased before anything is mounted from it.
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
/* the static BusyBox rc.sh keeps for this fallback once /cache/bin/busybox links to the root
 * file system's (dynamic) one, which the flash initramfs does not have */
#define RESCUE_BUSYBOX "/cache/kernel6/busybox"
#define RC "/cache/kernel6/rc.sh"
#define RESET_FLAG "/cache/kernel6/factory-reset"
#define PENDING_ROOT "/cache/kernel6/rootfs.sqfs"
/* what tools/install_kernel6.py puts for /cache while the cache partition is still separate (a
 * stock Hub, or one on the old layout): moved into cache/ at the next boot */
#define CACHE_IMPORT "/overlay/cache-import"
#define USB_FLAG "/cache/kernel6/usb-storage"
#define USB_MISSING "/cache/kernel6/usb-missing"
#define STATE_MAGIC "HarmonyNG data+cache 1"
#define OLD_DATA_SIZE 0x500000   /* the stock data partition: the merged one's first half */

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

/* erase a partition from offset from to its end (empty JFFS2 there: the next mount starts
 * fresh). 0 when every block was erased. */
static int erase_mtd(const char *name, unsigned int from) {
    char dev[32];
    struct mtd_info_user mi;
    int n = mtd_num(name), fd, rc = 0;
    if (n < 0) return -1;
    snprintf(dev, sizeof(dev), "/dev/mtd%d", n);
    fd = open(dev, O_RDWR);
    if (fd < 0 || ioctl(fd, MEMGETINFO, &mi) != 0) {
        msg("cannot open", dev, errno);
        if (fd >= 0) close(fd);
        return -1;
    }
    for (unsigned int off = from; off < mi.size; off += mi.erasesize) {
        struct erase_info_user ei = { .start = off, .length = mi.erasesize };
        if (ioctl(fd, MEMERASE, &ei) != 0) {
            msg("erase failed:", dev, errno);
            rc = -1;
        }
    }
    close(fd);
    msg("erased", name, 0);
    return rc;
}

/* The stock cache file system in the second half of "data" goes once (see the header). Without
 * a "state" partition (an older device tree) nothing is done. */
static void merge_cache_partition(void) {
    char dev[32], buf[sizeof(STATE_MAGIC) - 1];
    int n = mtd_num("state"), fd;
    if (n < 0 || mtd_num("data") < 0) return;
    snprintf(dev, sizeof(dev), "/dev/mtd%d", n);
    fd = open(dev, O_RDONLY);
    if (fd < 0) return;
    if (read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf) && memcmp(buf, STATE_MAGIC, sizeof(buf)) == 0) {
        close(fd);
        return;
    }
    close(fd);
    msg("flash layout:", "erasing the stock cache file system (data and cache become one)", 0);
    if (erase_mtd("data", OLD_DATA_SIZE) != 0 || erase_mtd("state", 0) != 0) return;
    fd = open(dev, O_WRONLY);
    if (fd < 0 || write(fd, STATE_MAGIC, sizeof(buf)) != (ssize_t)sizeof(buf))
        msg("cannot write", dev, errno);
    if (fd >= 0) close(fd);
    sync();
}

/* moves from/ into to/ entry by entry (renames: one file system); an entry both have is
 * replaced, a directory both have is merged */
static void merge_into(const char *from, const char *to) {
    char a[256], b[256];
    struct stat sa, sb;
    struct dirent *e;
    DIR *d = opendir(from);
    if (!d) return;
    mkdir(to, 0755);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
        snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
        if (lstat(b, &sb) == 0) {
            if (lstat(a, &sa) == 0 && S_ISDIR(sa.st_mode) && S_ISDIR(sb.st_mode)) {
                merge_into(a, b);
                continue;
            }
            if (S_ISDIR(sb.st_mode)) {
                msg("not replaced (a directory):", b, 0);
                continue;
            }
            unlink(b);
        }
        if (rename(a, b) != 0) msg("cannot move", a, errno);
    }
    closedir(d);
    if (rmdir(from) != 0) msg("left behind:", from, errno);
}

/* the data partition on /overlay, its cache/ bound on /cache */
static int mount_flash(void) {
    if (mnt("mtd:data", "/overlay", "jffs2", 0, NULL) != 0) return -1;
    mkdir("/overlay/cache", 0755);
    return mnt("/overlay/cache", "/cache", NULL, MS_BIND, NULL);
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

/* writes a staged root file system (an XZ squashfs that fits) to mtd "root", block by block with
 * read-back. An image that is not valid is set aside (.failed); a flash error keeps it for the
 * next boot (block 0 is erased first, so the half-written root is not mounted meanwhile: the
 * fallback boot runs) */
static void write_pending_root(const char *pending) {
    char dev[32], err[128] = "";
    struct mtd_info_user mi;
    struct flash_part part;
    struct stat st;
    uint8_t *img = NULL;
    int n = mtd_num("root"), fd = -1, mfd = -1, written = 0, ok = 0, flash_error = 0;

    if (stat(pending, &st) != 0) return;
    msg("writing", pending, 0);
    snprintf(dev, sizeof(dev), "/dev/mtd%d", n);
    if (n < 0 || (mfd = open(dev, O_RDWR)) < 0 || ioctl(mfd, MEMGETINFO, &mi) != 0) {
        snprintf(err, sizeof(err), "no root partition");
    } else if ((uint32_t)st.st_size > mi.size) {
        snprintf(err, sizeof(err), "larger than the root partition");
    } else if (!(img = malloc(st.st_size)) || (fd = open(pending, O_RDONLY)) < 0 ||
               read(fd, img, st.st_size) != st.st_size) {
        snprintf(err, sizeof(err), "cannot read it");
    } else if (squashfs_check(img, st.st_size, SQUASHFS_XZ, err, sizeof(err)) == 0) {
        part.fd = mfd;
        part.size = mi.size;
        part.erasesize = mi.erasesize;
        ok = flash_write_image(&part, 1, &flash_mtd_ops, img, st.st_size, &written, err, sizeof(err)) == 0;
        flash_error = !ok;
    }
    if (fd >= 0) close(fd);
    if (mfd >= 0) close(mfd);
    free(img);
    if (ok) {
        unlink(pending);
        dprintf(2, "init: root file system written (%d blocks)\n", written);
    } else if (flash_error) {
        msg("root file system not written (kept for the next boot):", err, 0);
    } else {
        char failed[96];
        snprintf(failed, sizeof(failed), "%s.failed", pending);
        rename(pending, failed);
        msg("root file system not written:", err, 0);
    }
    sync();
}

/* first boot of the root file system: the data partition still has the stock union layout
 * (data/, etc/, ... at its top): all of it goes to upper/ (renames, no copy) */
/* name (absolute, or relative to dir) exists in the upper layer or in /rom */
static int link_target_exists(const char *upper, const char *dir, const char *target) {
    char p[512];
    struct stat st;
    if (target[0] == '/') {
        snprintf(p, sizeof(p), "%s%s", upper, target);
        if (stat(p, &st) == 0) return 1;
        snprintf(p, sizeof(p), "/rom%s", target);
    } else {
        snprintf(p, sizeof(p), "%s/%s/%s", upper, dir, target);
        if (stat(p, &st) == 0) return 1;
        snprintf(p, sizeof(p), "/rom/%s/%s", dir, target);
    }
    return stat(p, &st) == 0;
}

/* The system directories belong to the root file system: in the upper layer (not mounted yet)
 * an entry the root file system also has goes (stock-era copies, whiteouts that hide its
 * programs), and so does a link that points nowhere (stock links to /data/dropbearmulti).
 * What HarmonyNG adds there (its /usr/sbin/dropbear wrapper) stays. */
static void clean_system_dirs(const char *upper) {
    static const char *const dirs[] = { "bin", "sbin", "usr/bin", "usr/sbin" };
    char path[512], low[512], target[256];
    struct stat st;
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "%s/%s", upper, dirs[i]);
        DIR *d = opendir(path);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2])))
                continue;
            snprintf(path, sizeof(path), "%s/%s/%s", upper, dirs[i], e->d_name);
            snprintf(low, sizeof(low), "/rom/%s/%s", dirs[i], e->d_name);
            if (lstat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
            int drop = lstat(low, &(struct stat){0}) == 0;
            if (!drop && S_ISLNK(st.st_mode)) {
                ssize_t n = readlink(path, target, sizeof(target) - 1);
                if (n > 0) {
                    target[n] = 0;
                    drop = !link_target_exists(upper, dirs[i], target);
                }
            }
            if (!drop && S_ISCHR(st.st_mode) && st.st_rdev == 0) drop = 1;   /* lone whiteout */
            if (drop && unlink(path) == 0) msg("removed", path, 0);
        }
        closedir(d);
    }
}

static void migrate_data(void) {
    DIR *d = opendir("/overlay");
    struct dirent *e;
    char from[300], to[300];
    if (!d) return;
    mkdir("/overlay/upper", 0755);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, "upper") ||
            !strcmp(e->d_name, "work") || !strcmp(e->d_name, "cache"))
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
        clean_system_dirs("/usb/data/upper");
    } else {
        if (stat("/overlay/upper", &st) != 0) migrate_data();
        mkdir("/overlay/upper", 0755);
        mkdir("/overlay/work", 0755);
        clean_system_dirs("/overlay/upper");
    }
    if (mnt("overlay", "/newroot", "overlay", 0, layers) != 0) {
        /* the fallback boot keeps /overlay and /cache */
        if (usb == 0) umount("/usb");
        umount("/rom");
        return -1;
    }
    if (usb == 0) {
        /* the stick's cache/ is /cache, the flash's boot state stays at /cache/kernel6; the
         * flash data partition is /flash */
        umount("/cache");
        mkdir("/usb/cache/kernel6", 0755);
        mkdir("/overlay/cache/kernel6", 0755);
        if (mount("/usb/cache", "/cache", NULL, MS_BIND, NULL) != 0 ||
            mount("/overlay/cache/kernel6", "/cache/kernel6", NULL, MS_BIND, NULL) != 0)
            msg("cannot bind", "/cache", errno);
        move_mount("/usb", "/newroot/usb");
        move_mount("/overlay", "/newroot/flash");
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

    merge_cache_partition();
    mount_flash();
    if (access(CACHE_IMPORT, F_OK) == 0) {
        merge_into(CACHE_IMPORT, "/overlay/cache");
        sync();
    }
    if (access(RESET_FLAG, F_OK) == 0) {
        msg("factory reset:", "erasing the data partition", 0);
        umount("/cache");
        umount("/overlay");
        erase_mtd("data", 0);
        mount_flash();
    }

#ifdef HARMONY_RECOVERY_SERIAL
    /* recovery build (RAM boot over U-Boot): keep the serial console, Bluetooth stays off it */
    {
        int sfd = open("/cache/kernel6/serial-console", O_WRONLY | O_CREAT, 0644);
        if (sfd >= 0) close(sfd);
        sync();
    }
#endif
    write_pending_root(PENDING_ROOT);
    /* HarmonyNG's /cache/bin (first in PATH) may hold applet links of a BusyBox named modprobe
     * or depmod: they would hide the system's /sbin/modprobe, which follows modules.dep */
    {
        static const char *const shadow[] = { "/cache/bin/modprobe", "/cache/bin/depmod" };
        struct stat ls;
        for (int i = 0; i < 2; i++)
            if (lstat(shadow[i], &ls) == 0 && S_ISLNK(ls.st_mode)) unlink(shadow[i]);
    }
    root_mtd = mtd_num("root");
    if (root_mtd >= 0 && our_root(root_mtd) && root_boot(root_mtd) == 0)
        return 0;   /* not reached: rcS runs */

    /* fallback boot: the data partition at /mnt/data (/cache stays its cache/) */
    mkdir("/mnt", 0755);
    move_mount("/overlay", "/mnt/data");
    fd = open("/proc/sys/kernel/modprobe", O_WRONLY);
    if (fd >= 0) {
        if (write(fd, "/bin/modprobe\n", 14) < 0) msg("cannot set", "modprobe path", errno);
        close(fd);
    }
    {
        static const char *const shells[] = { RESCUE_BUSYBOX, BUSYBOX };
        if (access(RC, R_OK) != 0) msg("missing", RC, errno);
        for (int i = 0; i < 2; i++) {
            if (access(RC, R_OK) == 0) execl(shells[i], "sh", RC, (char *)NULL);
            else execl(shells[i], "sh", (char *)NULL);
            msg("cannot run", shells[i], errno);
        }
    }
    for (;;)
        pause();
}
