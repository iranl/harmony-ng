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
#include <sys/syscall.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>

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
        if (strcmp(cmd, "usbwifi") == 0 || strcmp(cmd, "hub_usbwifi") == 0)
            return usbwifi_main(argc - 1, argv + 1);
    }

    fprintf(stderr, "Harmony Hub Multi-Call Utility (hub_util)\n");
    fprintf(stderr, "Usage: %s <command> [args...]\n\n", name);
    fprintf(stderr, "Available commands:\n");
    fprintf(stderr, "  sync          Flush filesystem buffers\n");
    fprintf(stderr, "  mknod         Create block/character special devices\n");
    fprintf(stderr, "  register_ehci Register EHCI host controller on Atheros AR9344\n");
    fprintf(stderr, "  eth_fix       Disable offloads for USB Ethernet\n");
    fprintf(stderr, "  sntp          SNTP time synchronization client\n");
    fprintf(stderr, "  ota-verify    Check a signed OTA manifest and files against it\n");
    fprintf(stderr, "  usbwifi       USB Wi-Fi setup: answers the stock usbhid on 127.0.0.1:16716\n");
    return 1;
}
