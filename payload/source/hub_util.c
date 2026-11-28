/*
 * Harmony Hub Multi-Call Utility (hub_util)
 * Combines small standalone utilities into a single binary to eliminate
 * duplicate C runtime overhead on constrained SPI Flash.
 *
 * Supported subcommands / symlinks:
 *   - hub_sync / sync
 *   - mknod
 *   - register_ehci
 *   - eth_fix / hub_eth_fix
 *   - hub_sntp / sntp
 *   - hub_usbwifi / usbwifi (USB Wi-Fi setup, usb_wifi.c)
 *   - flash-image (kernel uImage to the kernel partitions, flash_image.c)
 *   - mfg skin (the factory skin number: 115 = Hub Pro 2400; mfg_id.c)
 *   - usbhid (USB Wi-Fi setup transport on the Linux 6.18 system; usb_hid_relay.c)
 */

#include <stdio.h>
#include "ota_verify.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <mtd/mtd-user.h>

#include "flash_image.h"
#include "mfg_id.h"
#include "usb_hid_relay.h"

#include "hub_ntp.h"
#include "usb_wifi.h"

#ifndef __NR_sethostname
#define __NR_sethostname (4000 + 148)
#endif

#ifndef ETHTOOL_SRXCSUM
#define ETHTOOL_SRXCSUM 0x00000015
#endif
#ifndef ETHTOOL_STXCSUM
#define ETHTOOL_STXCSUM 0x00000017
#endif
#ifndef ETHTOOL_SSG
#define ETHTOOL_SSG     0x00000019
#endif
#ifndef ETHTOOL_STSO
#define ETHTOOL_STSO    0x0000001f
#endif

/* =========================================================================
 * Subcommand: sync / hub_sync
 * ========================================================================= */
static int run_sync(int argc, char **argv) {
    (void)argc;
    (void)argv;
    sync();
    return 0;
}

/* =========================================================================
 * Subcommand: mknod
 * ========================================================================= */
static int run_mknod(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <path> <c|b> <major> <minor>\n", argv[0]);
        return 1;
    }
    mode_t mode = 0666;
    if (strcmp(argv[2], "c") == 0) mode |= S_IFCHR;
    else if (strcmp(argv[2], "b") == 0) mode |= S_IFBLK;
    else {
        /* any other type used to create a regular file in place of the device node */
        fprintf(stderr, "%s: type must be c or b, not '%s'\n", argv[0], argv[2]);
        return 1;
    }
    unsigned int maj = (unsigned int)atoi(argv[3]);
    unsigned int min = (unsigned int)atoi(argv[4]);
    dev_t dev = ((maj & 0xfff) << 8) | (min & 0xff);
    if (mknod(argv[1], mode, dev) < 0) {
        perror("mknod");
        return 1;
    }
    return 0;
}

/* =========================================================================
 * Subcommand: register_ehci
 * ========================================================================= */
static int run_register_ehci(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    uint32_t slot_phys = 0x0000c990;
    uint32_t orig_fn = 0;

    if (lseek(fd, slot_phys, SEEK_SET) < 0 ||
        read(fd, &orig_fn, sizeof(orig_fn)) != sizeof(orig_fn)) {
        perror("read slot");
        close(fd);
        return 1;
    }

    printf("sys_call_table[148] at 0x%08x = 0x%08x (expected 0x8002fb4c)\n", slot_phys, orig_fn);
    if (orig_fn != 0x8002fb4c) {
        fprintf(stderr, "Slot mismatch! Aborting for safety.\n");
        close(fd);
        return 1;
    }

    uint32_t new_fn = 0x801267cc;
    printf("Swapping slot to platform_device_register (0x%08x)...\n", new_fn);
    if (lseek(fd, slot_phys, SEEK_SET) < 0 ||
        write(fd, &new_fn, sizeof(new_fn)) != sizeof(new_fn)) {
        perror("write slot");
        close(fd);
        return 1;
    }

    printf("Invoking syscall with &ar7240_usb_device (0x8023b808)...\n");
    int res = syscall(__NR_sethostname, (void *)0x8023b808);
    int err = errno;

    printf("platform_device_register returned: %d, errno: %d (%s)\n", res, err, strerror(err));

    printf("Restoring original sys_call_table slot...\n");
    if (lseek(fd, slot_phys, SEEK_SET) < 0 ||
        write(fd, &orig_fn, sizeof(orig_fn)) != sizeof(orig_fn)) {
        perror("restore slot");
        close(fd);
        return 1;
    }

    close(fd);
    printf("Done!\n");
    return (res == 0) ? 0 : 1;
}

/* =========================================================================
 * Subcommand: eth_fix / hub_eth_fix
 * ========================================================================= */
static int run_eth_fix(int argc, char **argv) {
    const char *ifname = (argc > 1) ? argv[1] : "eth2";
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    /* every offload is switched off on its own; the result says which ones failed (the
     * success line used to be printed whatever the ioctl calls returned) */
    static const struct { uint32_t cmd; const char *name; } offloads[] = {
        { ETHTOOL_STXCSUM, "TX checksum offload" },
        { ETHTOOL_SRXCSUM, "RX checksum offload" },
        { ETHTOOL_SSG, "scatter-gather" },
        { ETHTOOL_STSO, "TCP segmentation offload" },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(offloads) / sizeof(offloads[0]); i++) {
        struct ethtool_value eval;
        eval.cmd = offloads[i].cmd;
        eval.data = 0;
        ifr.ifr_data = (caddr_t)&eval;
        if (ioctl(fd, SIOCETHTOOL, &ifr) == 0) {
            printf("[+] %s: %s disabled\n", ifname, offloads[i].name);
        } else {
            fprintf(stderr, "[-] %s: could not disable %s: %s\n", ifname, offloads[i].name, strerror(errno));
            failed++;
        }
    }

    close(fd);
    if (failed) {
        fprintf(stderr, "[-] %s: %d of 4 offload settings could not be changed\n", ifname, failed);
        return 1;
    }
    printf("[+] %s: All checksum and segmentation offloads disabled successfully\n", ifname);
    return 0;
}

/* =========================================================================
 * Subcommand: sntp / hub_sntp
 * ========================================================================= */
static int run_sntp(int argc, char **argv) {
    const char *srv = "pool.ntp.org";
    int apply = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--set") == 0 || strcmp(argv[i], "-s") == 0) {
            apply = 1;
        } else if (argv[i][0] != '-') {
            srv = argv[i];
        }
    }
    time_t t = sntp_sync_time(srv, apply);
    if (t > 0) {
        printf("SNTP sync success: %lu\n", (unsigned long)t);
        return 0;
    }
    fprintf(stderr, "SNTP sync failed\n");
    return 1;
}

/* =========================================================================
 * Main Dispatcher
 * ========================================================================= */
static const char *get_progname(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* hub_util ota-verify <manifest.json> <manifest.json.sig> [key=path ...]
 * Verifies the Ed25519 signature of an OTA manifest with the release keys built into the
 * firmware, then the SHA-256 of each named file against its manifest entry (key = the entry
 * name, e.g. hub_ota.tar.gz or bin/mips/hub_daemon). Prints the manifest version on success
 * (exit 0); prints the reason and exits 1 otherwise. update.sh installs nothing without it. */
static int run_ota_verify(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: hub_util ota-verify <manifest> <signature> [key=path ...]\n");
        return 2;
    }
    char err[160];
    char *manifest = ota_load_signed_manifest(argv[1], argv[2], err, sizeof(err));
    if (!manifest) {
        fprintf(stderr, "ota-verify: %s\n", err);
        return 1;
    }
    char version[32];
    if (ota_manifest_version(manifest, version, sizeof(version)) != 0) {
        fprintf(stderr, "ota-verify: manifest has no valid version\n");
        free(manifest);
        return 1;
    }
    for (int i = 3; i < argc; i++) {
        char spec[512];
        snprintf(spec, sizeof(spec), "%s", argv[i]);
        char *eq = strchr(spec, '=');
        if (!eq || eq == spec || !eq[1]) {
            fprintf(stderr, "ota-verify: bad file argument '%s' (want key=path)\n", argv[i]);
            free(manifest);
            return 2;
        }
        *eq = '\0';
        if (ota_manifest_check_file(manifest, spec, eq + 1, err, sizeof(err)) != 0) {
            fprintf(stderr, "ota-verify: %s\n", err);
            free(manifest);
            return 1;
        }
    }
    free(manifest);
    printf("%s\n", version);
    return 0;
}

/* =========================================================================
 * Subcommand: flash-image [--dry-run] <uImage> /dev/mtdN [/dev/mtdN+1]
 * Writes a kernel uImage over the kernel partitions (only those named "kernel" and "kernel2",
 * in flash order), block by block with read-back (flash_image.c).
 * ========================================================================= */
/* partition name of /dev/mtdN from /proc/mtd ("mtdN: size erasesize \"name\"") */
static int mtd_name(int num, char *name, size_t len) {
    FILE *f = fopen("/proc/mtd", "r");
    if (!f) return -1;
    char line[128];
    int rc = -1;
    while (fgets(line, sizeof(line), f)) {
        int n;
        char *q = strchr(line, '"');
        if (sscanf(line, "mtd%d:", &n) == 1 && n == num && q) {
            char *e = strchr(q + 1, '"');
            if (e) {
                snprintf(name, len, "%.*s", (int)(e - q - 1), q + 1);
                rc = 0;
            }
            break;
        }
    }
    fclose(f);
    return rc;
}

/* a process with this name runs (/proc/PID/stat; the stock 2.6.31 kernel has no comm file) */
static int process_running(const char *want) {
    char path[64], buf[128];
    for (int pid = 2; pid < 32768; pid++) {
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        char *a = strchr(buf, '('), *b = strrchr(buf, ')');
        if (a && b && (size_t)(b - a - 1) == strlen(want) && strncmp(a + 1, want, b - a - 1) == 0)
            return 1;
    }
    return 0;
}

/* the mtd partition is mounted (its mtdblock device is in /proc/mounts) */
static int mtd_mounted(int num) {
    char want[32], line[256];
    int found = 0;
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    snprintf(want, sizeof(want), "/dev/mtdblock%d ", num);
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, want, strlen(want)) == 0) found = 1;
    fclose(f);
    return found;
}

/* Kernel: a uImage over "kernel" (and "kernel2"). Root file system: an XZ squashfs over "root",
 * which may not be mounted (the Linux 6.18 system writes a new root from its fallback boot:
 * build/kernel6/init.c). Back to the stock system: hub_util restore. */
static int run_flash_image(int argc, char **argv) {
    int dry = 0, first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        if (strcmp(argv[first], "--dry-run") == 0) dry = 1;
        else break;
    }
    if (argc - first < 2 || argc - first > 3) {
        fprintf(stderr, "usage: hub_util flash-image [--dry-run] <uImage> /dev/mtdN [/dev/mtdN+1]\n"
                        "       hub_util flash-image [--dry-run] <squashfs> /dev/mtdN\n");
        return 2;
    }
    const char *image = argv[first];
    int nparts = argc - first - 1;

    /* the CC2544 shares the SPI bus with the flash (AGENTS.md) */
    if (!dry && (process_running("hub_rf") || process_running("hub_btstack"))) {
        fprintf(stderr, "flash-image: stop hub_rf and hub_btstack first (killall -9)\n");
        return 1;
    }

    FILE *f = fopen(image, "rb");
    if (!f) { perror(image); return 1; }
    uint8_t *img = malloc(4u << 20);
    size_t len = img ? fread(img, 1, 4u << 20, f) : 0;
    int more = fgetc(f) != EOF;
    fclose(f);
    char err[160];
    if (!img || len == 0 || more) {
        fprintf(stderr, "flash-image: %s is empty or over 4 MB\n", image);
        free(img);
        return 1;
    }
    int root = memcmp(img, "hsqs", 4) == 0;
    if (root ? squashfs_check(img, len, SQUASHFS_XZ, err, sizeof(err))
             : uimage_check(img, len, err, sizeof(err))) {
        fprintf(stderr, "flash-image: %s: %s\n", image, err);
        free(img);
        return 1;
    }
    if (root && nparts != 1) {
        fprintf(stderr, "flash-image: a root file system goes to one partition\n");
        free(img);
        return 1;
    }

    struct flash_part parts[2];
    int prev = -1, rc = 1;
    for (int i = 0; i < nparts; i++) parts[i].fd = -1;
    for (int i = 0; i < nparts; i++) {
        const char *dev = argv[first + 1 + i];
        int num;
        char name[32];
        if (sscanf(dev, "/dev/mtd%d", &num) != 1 || mtd_name(num, name, sizeof(name)) != 0) {
            fprintf(stderr, "flash-image: %s is not an mtd device\n", dev);
            goto out;
        }
        if (root) {
            if (strcmp(name, "root") != 0) {
                fprintf(stderr, "flash-image: %s is \"%s\"; a squashfs goes to \"root\" only\n", dev, name);
                goto out;
            }
            if (mtd_mounted(num)) {
                fprintf(stderr, "flash-image: %s is mounted (the running root file system)\n", dev);
                goto out;
            }
        } else if (strcmp(name, i == 0 ? "kernel" : "kernel2") != 0 || (prev >= 0 && num != prev + 1)) {
            fprintf(stderr, "flash-image: %s is \"%s\"; only \"kernel\" then the next partition "
                    "\"kernel2\" may be written\n", dev, name);
            goto out;
        }
        prev = num;
        parts[i].fd = open(dev, dry ? O_RDONLY : O_RDWR);
        struct mtd_info_user mi;
        if (parts[i].fd < 0 || ioctl(parts[i].fd, MEMGETINFO, &mi) != 0) {
            perror(dev);
            goto out;
        }
        if (!dry && !(mi.flags & MTD_WRITEABLE)) {
            fprintf(stderr, "flash-image: %s is read-only\n", dev);
            goto out;
        }
        parts[i].size = mi.size;
        parts[i].erasesize = mi.erasesize;
    }
    uint64_t total = 0;
    for (int i = 0; i < nparts; i++) total += parts[i].size;
    printf("%s: %u bytes, valid %s; %d partition(s), %llu bytes\n", image, (unsigned)len,
           root ? "squashfs" : "uImage", nparts, (unsigned long long)total);
    if (len > total) {
        fprintf(stderr, "flash-image: image does not fit\n");
        goto out;
    }
    if (dry) {
        rc = 0;
        goto out;
    }
    const struct flash_ops ops = flash_mtd_ops;
    int written;
    if (flash_write_image(parts, nparts, &ops, img, len, &written, err, sizeof(err)) != 0) {
        fprintf(stderr, "flash-image: %s (%d blocks were rewritten)\n", err, written);
        goto out;
    }
    sync();
    printf("flash-image: done, %d blocks rewritten and verified\n", written);
    rc = 0;
out:
    for (int i = 0; i < nparts; i++)
        if (parts[i].fd >= 0) close(parts[i].fd);
    free(img);
    return rc;
}


/* =========================================================================
 * Subcommand: restore [--dry-run] <kernels.bin> <root.bin> <datacache.bin>
 * Back to the stock system (tools/rollback_kernel6.py): the backup tools/install_kernel6.py made
 * at the migration, written from RAM. kernels.bin (stock kernel + kernel2, a uImage first) goes
 * over "kernel" + "kernel2", root.bin (the stock LZMA squashfs) over "root", datacache.bin (the
 * raw stock data then cache partitions, 10 MB) over "data" and the partition after it ("state"
 * on the merged layout, "cache" on the stock one). Run a static hub_util from /tmp: the files
 * are mapped, not copied (tmpfs pages), every JFFS2 mount is made read-only first (no garbage
 * collection writes into the restored data), and the Hub restarts right after.
 * ========================================================================= */
#define STOCK_PART 0x500000u   /* the stock data and cache partitions */

struct restore_file {
    const char *path;
    const uint8_t *data;
    size_t len;
};

static int restore_map(struct restore_file *f) {
    struct stat st;
    int fd = open(f->path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size == 0) {
        perror(f->path);
        if (fd >= 0) close(fd);
        return -1;
    }
    f->len = (size_t)st.st_size;
    f->data = mmap(NULL, f->len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f->data == MAP_FAILED) {
        perror(f->path);
        return -1;
    }
    return 0;
}

/* opens /dev/mtdN of the partition named name; fills part and *num_out */
static int restore_part(const char *name, struct flash_part *part, int dry, int *num_out) {
    char dev[32], have[32];
    struct mtd_info_user mi;
    for (int n = 0; n < 16; n++) {
        if (mtd_name(n, have, sizeof(have)) != 0 || strcmp(have, name) != 0) continue;
        snprintf(dev, sizeof(dev), "/dev/mtd%d", n);
        part->fd = open(dev, dry ? O_RDONLY : O_RDWR);
        if (part->fd < 0 || ioctl(part->fd, MEMGETINFO, &mi) != 0) {
            perror(dev);
            return -1;
        }
        if (!dry && !(mi.flags & MTD_WRITEABLE)) {
            fprintf(stderr, "restore: %s is read-only\n", dev);
            return -1;
        }
        part->size = mi.size;
        part->erasesize = mi.erasesize;
        if (num_out) *num_out = n;
        return 0;
    }
    fprintf(stderr, "restore: no partition \"%s\"\n", name);
    return -1;
}

/* every JFFS2 mount read-only; 0 when all of them are */
static int restore_remount_ro(void) {
    char line[256], src[96], dir[128], type[32];
    int rc = 0;
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%95s %127s %31s", src, dir, type) != 3 || strcmp(type, "jffs2") != 0) continue;
        if (mount(NULL, dir, NULL, MS_REMOUNT | MS_RDONLY, NULL) != 0) {
            fprintf(stderr, "restore: cannot make %s read-only: %s\n", dir, strerror(errno));
            rc = -1;
        }
    }
    fclose(f);
    return rc;
}

static int run_restore(int argc, char **argv) {
    const struct flash_ops ops = flash_mtd_ops;
    struct restore_file f[3];
    struct flash_part kparts[2], dparts[2], rpart;
    char err[160], next[32];
    int dry = 0, first = 1, rc = 1, written, dnum = -1;

    /* line by line: the rollback tool runs it with the output in a file and reads its progress */
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1 && strcmp(argv[1], "--dry-run") == 0) {
        dry = 1;
        first = 2;
    }
    if (argc - first != 3) {
        fprintf(stderr, "usage: hub_util restore [--dry-run] <kernels.bin> <root.bin> <datacache.bin>\n");
        return 2;
    }
    for (int i = 0; i < 3; i++) {
        f[i].path = argv[first + i];
        if (restore_map(&f[i]) != 0) return 1;
    }
    if (uimage_check(f[0].data, f[0].len, err, sizeof(err)) != 0) {
        fprintf(stderr, "restore: %s: %s\n", f[0].path, err);
        return 1;
    }
    if (squashfs_check(f[1].data, f[1].len, SQUASHFS_LZMA, err, sizeof(err)) != 0) {
        fprintf(stderr, "restore: %s: %s\n", f[1].path, err);
        return 1;
    }
    if (f[2].len != 2 * STOCK_PART) {
        fprintf(stderr, "restore: the data + cache backup must be %u bytes\n", 2 * STOCK_PART);
        return 1;
    }
    kparts[0].fd = kparts[1].fd = dparts[0].fd = dparts[1].fd = rpart.fd = -1;
    if (restore_part("kernel", &kparts[0], dry, NULL) || restore_part("kernel2", &kparts[1], dry, NULL) ||
        restore_part("root", &rpart, dry, NULL) || restore_part("data", &dparts[0], dry, &dnum))
        goto out;
    /* the partition after "data": "state" (merged layout) or "cache" (stock layout) */
    if (mtd_name(dnum + 1, next, sizeof(next)) != 0 ||
        (strcmp(next, "state") != 0 && strcmp(next, "cache") != 0) ||
        restore_part(next, &dparts[1], dry, NULL)) {
        fprintf(stderr, "restore: no \"state\" or \"cache\" partition after \"data\"\n");
        goto out;
    }
    if (f[0].len > kparts[0].size + kparts[1].size || f[1].len > rpart.size ||
        dparts[0].size + dparts[1].size != f[2].len) {
        fprintf(stderr, "restore: the images do not match the partitions\n");
        goto out;
    }
    printf("restore: kernels %u, root %u, data + cache %u bytes: valid\n", (unsigned)f[0].len,
           (unsigned)f[1].len, (unsigned)f[2].len);
    if (dry) {
        rc = 0;
        goto out;
    }
    if (process_running("hub_rf") || process_running("hub_btstack")) {
        fprintf(stderr, "restore: stop hub_rf and hub_btstack first (killall -9)\n");
        goto out;
    }
    sync();
    if (restore_remount_ro() != 0) {
        fprintf(stderr, "restore: nothing written\n");
        goto out;
    }
    /* from here the flash changes: on a failure carry on, stopping halfway helps nothing */
    printf("restore: writing the kernels\n");
    if (flash_write_image(kparts, 2, &ops, f[0].data, f[0].len, &written, err, sizeof(err)) != 0)
        fprintf(stderr, "restore: kernels: %s\n", err);
    printf("restore: writing root\n");
    if (flash_write_image(&rpart, 1, &ops, f[1].data, f[1].len, &written, err, sizeof(err)) != 0)
        fprintf(stderr, "restore: root: %s\n", err);
    printf("restore: writing data + cache (the longest part)\n");
    if (flash_write_image(dparts, 2, &ops, f[2].data, f[2].len, &written, err, sizeof(err)) != 0)
        fprintf(stderr, "restore: data + cache: %s\n", err);
    sync();
    printf("restore: written, restarting\n");
    fflush(stdout);
    reboot(RB_AUTOBOOT);
    rc = 0;
out:
    for (int i = 0; i < 2; i++) {
        if (kparts[i].fd >= 0) close(kparts[i].fd);
        if (dparts[i].fd >= 0) close(dparts[i].fd);
    }
    if (rpart.fd >= 0) close(rpart.fd);
    return rc;
}

/* hub_util mfg skin: prints the factory skin number (115 = Harmony Hub Pro 2400). The Linux 6.18
 * boot script (build/kernel6/rc.sh) reads it before hub_daemon has written /data/harmony/skin. */
static int run_mfg(int argc, char **argv) {
    uint8_t skin;
    if (argc != 2 || strcmp(argv[1], "skin") != 0) {
        fprintf(stderr, "usage: hub_util mfg skin\n");
        return 2;
    }
    if (mfg_read_skin(NULL, &skin) != 0) {
        fprintf(stderr, "mfg: no skin record\n");
        return 1;
    }
    printf("%u\n", skin);
    return 0;
}

/* hub_util df <path>: the KB free for root in the file system of path. The stock BusyBox has no
 * df (tools/install_kernel6.py checks the room on a freshly rooted stock Hub with it). */
static int run_df(int argc, char **argv) {
    struct statvfs sv;
    if (argc != 2) {
        fprintf(stderr, "usage: hub_util df <path>\n");
        return 2;
    }
    if (statvfs(argv[1], &sv) != 0) {
        perror(argv[1]);
        return 1;
    }
    printf("%llu\n", (unsigned long long)sv.f_bavail * sv.f_frsize / 1024);
    return 0;
}

int main(int argc, char **argv) {
    const char *name = get_progname(argv[0]);

    /* Direct dispatch via symlink name */
    if (strcmp(name, "hub_sync") == 0 || strcmp(name, "sync") == 0)
        return run_sync(argc, argv);
    if (strcmp(name, "mknod") == 0)
        return run_mknod(argc, argv);
    if (strcmp(name, "register_ehci") == 0)
        return run_register_ehci(argc, argv);
    if (strcmp(name, "eth_fix") == 0 || strcmp(name, "hub_eth_fix") == 0)
        return run_eth_fix(argc, argv);
    if (strcmp(name, "hub_sntp") == 0 || strcmp(name, "sntp") == 0)
        return run_sntp(argc, argv);
    if (strcmp(name, "hub_usbwifi") == 0)
        return usbwifi_main(argc, argv);

    /* Dispatch via subcommand: hub_util <command> [args...] */
    if (argc >= 2) {
        const char *cmd = argv[1];
        if (strcmp(cmd, "sync") == 0 || strcmp(cmd, "hub_sync") == 0)
            return run_sync(argc - 1, argv + 1);
        if (strcmp(cmd, "mknod") == 0)
            return run_mknod(argc - 1, argv + 1);
        if (strcmp(cmd, "register_ehci") == 0)
            return run_register_ehci(argc - 1, argv + 1);
        if (strcmp(cmd, "eth_fix") == 0 || strcmp(cmd, "hub_eth_fix") == 0)
            return run_eth_fix(argc - 1, argv + 1);
        if (strcmp(cmd, "sntp") == 0 || strcmp(cmd, "hub_sntp") == 0)
            return run_sntp(argc - 1, argv + 1);
        if (strcmp(cmd, "ota-verify") == 0)
            return run_ota_verify(argc - 1, argv + 1);
        if (strcmp(cmd, "flash-image") == 0)
            return run_flash_image(argc - 1, argv + 1);
        if (strcmp(cmd, "restore") == 0)
            return run_restore(argc - 1, argv + 1);
        if (strcmp(cmd, "mfg") == 0)
            return run_mfg(argc - 1, argv + 1);
        if (strcmp(cmd, "df") == 0)
            return run_df(argc - 1, argv + 1);
        if (strcmp(cmd, "usbhid") == 0)
            return usbhid_relay_main(argc - 1, argv + 1);
        if (strcmp(cmd, "usbwifi") == 0 || strcmp(cmd, "hub_usbwifi") == 0)
            return usbwifi_main(argc - 1, argv + 1);
    }

    fprintf(stderr, "Harmony Hub Multi-Call Utility (hub_util)\n");
    fprintf(stderr, "Usage: %s <command> [args...]\n\n", name);
    fprintf(stderr, "Available commands:\n");
    fprintf(stderr, "  sync          Flush filesystem buffers\n");
    fprintf(stderr, "  mknod         Create block/character special devices\n");
    fprintf(stderr, "  register_ehci Register EHCI host controller on Atheros AR9330\n");
    fprintf(stderr, "  eth_fix       Disable offloads for USB Ethernet\n");
    fprintf(stderr, "  sntp          SNTP time synchronization client\n");
    fprintf(stderr, "  ota-verify    Check a signed OTA manifest and files against it\n");
    fprintf(stderr, "  flash-image   Write a kernel uImage to the kernel partitions\n");
    fprintf(stderr, "  restore       Write the stock system back from a migration backup (Linux 6.18)\n");
    fprintf(stderr, "  mfg skin      Print the factory skin number (115 = Hub Pro 2400)\n");
    fprintf(stderr, "  usbhid        Relay the USB HID gadget to hub_util usbwifi (Linux 6.18)\n");
    fprintf(stderr, "  usbwifi       USB Wi-Fi setup: answers the stock usbhid on 127.0.0.1:16716\n");
    return 1;
}
