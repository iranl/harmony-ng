#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/fb.h>
#include <linux/input.h>

#define DEFAULT_MOCK_DIR "/tmp/harmony_mock"

static int (*real_open)(const char *pathname, int flags, ...) = NULL;
static int (*real_open64)(const char *pathname, int flags, ...) = NULL;
static int (*real_close)(int fd) = NULL;
static int (*real_ioctl)(int fd, unsigned long request, ...) = NULL;
static void *(*real_mmap)(void *addr, size_t length, int prot, int flags, int fd, off_t offset) = NULL;
static FILE *(*real_fopen)(const char *path, const char *mode) = NULL;
static FILE *(*real_fopen64)(const char *path, const char *mode) = NULL;
static int (*real_unlink)(const char *pathname) = NULL;

static int g_fb_mock_fd = -1;
static int g_touch_mock_fd = -1;
static int g_btn_mock_fd = -1;
static int g_gyro_mock_fd = -1;
static int g_rf_mock_fd = -1;

static void init_real_functions(void) {
    if (!real_open) {
        real_open = dlsym(RTLD_NEXT, "open");
        real_open64 = dlsym(RTLD_NEXT, "open64");
        real_close = dlsym(RTLD_NEXT, "close");
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
        real_mmap = dlsym(RTLD_NEXT, "mmap");
        real_fopen = dlsym(RTLD_NEXT, "fopen");
        real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
        real_unlink = dlsym(RTLD_NEXT, "unlink");
        if (!real_open64) real_open64 = real_open;
        if (!real_fopen64) real_fopen64 = real_fopen;
    }
}

static const char *get_mock_dir(void) {
    const char *d = getenv("HARMONY_MOCK_DIR");
    return (d && d[0]) ? d : DEFAULT_MOCK_DIR;
}

static void ensure_mock_dirs(void) {
    const char *md = get_mock_dir();
    char path[512];
    snprintf(path, sizeof(path), "%s", md);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/sysfs", md);
    mkdir(path, 0777);
}

static int open_unix_socket(const char *sock_path) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    return s;
}

static const char *resolve_mock_path(const char *pathname, char *resolved, size_t max_len, int *out_special_type) {
    *out_special_type = 0;
    if (!pathname) return pathname;

    const char *md = get_mock_dir();

    /* 1. RF SPI Device */
    if (strcmp(pathname, "/dev/rfspi") == 0) {
        *out_special_type = 1; /* Special RF handling */
        const char *role = getenv("HARMONY_MOCK_ROLE");
        if (role && strcmp(role, "elite") == 0) {
            snprintf(resolved, max_len, "%s/elite_rf.sock", md);
        } else {
            snprintf(resolved, max_len, "%s/hub_rf.sock", md);
        }
        return resolved;
    }

    /* 2. Framebuffer Device */
    if (strcmp(pathname, "/dev/fb0") == 0) {
        *out_special_type = 2;
        snprintf(resolved, max_len, "%s/fb0", md);
        return resolved;
    }

    /* 3. Input Devices */
    if (strcmp(pathname, "/dev/input/event0") == 0) {
        *out_special_type = 3; /* Buttons */
        snprintf(resolved, max_len, "%s/btn_pipe", md);
        return resolved;
    }
    if (strcmp(pathname, "/dev/input/event1") == 0) {
        *out_special_type = 4; /* Gyro */
        snprintf(resolved, max_len, "%s/gyro_pipe", md);
        return resolved;
    }
    if (strcmp(pathname, "/dev/input/event2") == 0 ||
        strcmp(pathname, "/dev/input/touchscreen0") == 0) {
        *out_special_type = 5; /* Touchscreen */
        snprintf(resolved, max_len, "%s/touch_pipe", md);
        return resolved;
    }

    /* 4. IR Blaster Node */
    if (strcmp(pathname, "/dev/harmony_ir") == 0 || strcmp(pathname, "/dev/lirc0") == 0) {
        *out_special_type = 6;
        snprintf(resolved, max_len, "%s/ir_blaster.log", md);
        return resolved;
    }

    /* 5. Sysfs Power, Backlight, Display */
    if (strcmp(pathname, "/sys/class/graphics/fb0/blank") == 0) {
        snprintf(resolved, max_len, "%s/sysfs/fb0_blank", md);
        return resolved;
    }
    if (strstr(pathname, "/sys/class/backlight/") != NULL) {
        snprintf(resolved, max_len, "%s/sysfs/backlight", md);
        return resolved;
    }
    if (strstr(pathname, "/sys/class/leds/") != NULL) {
        snprintf(resolved, max_len, "%s/sysfs/leds", md);
        return resolved;
    }
    if (strstr(pathname, "/powersource") != NULL) {
        snprintf(resolved, max_len, "%s/sysfs/powersource", md);
        return resolved;
    }
    if (strstr(pathname, "/vbat") != NULL) {
        snprintf(resolved, max_len, "%s/sysfs/vbat", md);
        return resolved;
    }
    if (strstr(pathname, "/charger") != NULL) {
        snprintf(resolved, max_len, "%s/sysfs/charger", md);
        return resolved;
    }
    if (strstr(pathname, "/xyz") != NULL) {
        snprintf(resolved, max_len, "%s/sysfs/gyro_xyz", md);
        return resolved;
    }
    if (strcmp(pathname, "/sys/power/state") == 0) {
        snprintf(resolved, max_len, "%s/sysfs/power_state", md);
        return resolved;
    }
    if (strstr(pathname, "/wakeup_event") != NULL) {
        snprintf(resolved, max_len, "%s/sysfs/wakeup_event", md);
        return resolved;
    }
    if (strstr(pathname, "/wakeup") != NULL) {
        snprintf(resolved, max_len, "%s/sysfs/wakeup", md);
        return resolved;
    }
    if (strstr(pathname, "/time") != NULL && strstr(pathname, "/sys/bus/i2c/") != NULL) {
        snprintf(resolved, max_len, "%s/sysfs/rtc_time", md);
        return resolved;
    }
    if (strstr(pathname, "/cpufreq/") != NULL) {
        snprintf(resolved, max_len, "%s/sysfs/cpufreq", md);
        return resolved;
    }

    /* 6. Virtualize /data/ directory if physical /data is not writable */
    if (strncmp(pathname, "/data/", 6) == 0 && access("/data", W_OK) != 0) {
        snprintf(resolved, max_len, "%s/data/%s", md, pathname + 6);
        char dtmp[512];
        snprintf(dtmp, sizeof(dtmp), "%s/data", md);
        mkdir(dtmp, 0777);
        snprintf(dtmp, sizeof(dtmp), "%s/data/harmony", md);
        mkdir(dtmp, 0777);
        return resolved;
    }

    return pathname;
}

int unlink(const char *pathname) {
    init_real_functions();
    char resolved[512];
    int special_type = 0;
    const char *target = resolve_mock_path(pathname, resolved, sizeof(resolved), &special_type);
    return real_unlink(target);
}

int open(const char *pathname, int flags, ...) {
    init_real_functions();
    ensure_mock_dirs();

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }

    char resolved[512];
    int special_type = 0;
    const char *target = resolve_mock_path(pathname, resolved, sizeof(resolved), &special_type);

    /* Special Handling: RF SPI via Unix Socket */
    if (special_type == 1) {
        int sock_fd = open_unix_socket(target);
        if (sock_fd >= 0) {
            g_rf_mock_fd = sock_fd;
            return sock_fd;
        }
        /* Fallback: named pipe / file if socket server not yet up */
    }

    /* Special Handling: Framebuffer Virtual Memory Backing */
    if (special_type == 2) {
        /* Ensure fb0 file exists and has minimum 240 * 320 * 2 = 153600 bytes */
        int fd = real_open(target, O_RDWR | O_CREAT, 0666);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size < 153600) {
                if (ftruncate(fd, 153600) != 0) {
                    /* pass */
                }
            }
            g_fb_mock_fd = fd;
            return fd;
        }
    }

    if (strstr(target, "/sysfs/") != NULL || special_type == 6) {
        flags |= O_CREAT;
        mode = 0666;
    }

    int fd = real_open(target, flags, mode);
    if (fd >= 0) {
        if (special_type == 3) g_btn_mock_fd = fd;
        else if (special_type == 4) g_gyro_mock_fd = fd;
        else if (special_type == 5) g_touch_mock_fd = fd;
    }
    return fd;
}

int open64(const char *pathname, int flags, ...) {
    init_real_functions();
    ensure_mock_dirs();

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }

    char resolved[512];
    int special_type = 0;
    const char *target = resolve_mock_path(pathname, resolved, sizeof(resolved), &special_type);

    if (special_type == 1) {
        int s = open_unix_socket(target);
        if (s >= 0) {
            g_rf_mock_fd = s;
            return s;
        }
    }

    if (special_type == 2) {
        int fd = real_open64(target, O_RDWR | O_CREAT, 0666);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size < 153600) {
                if (ftruncate(fd, 153600) != 0) {}
            }
            g_fb_mock_fd = fd;
            return fd;
        }
    }

    if (strstr(target, "/sysfs/") != NULL || special_type == 6) {
        flags |= O_CREAT;
        mode = 0666;
    }

    int fd = real_open64(target, flags, mode);
    if (fd >= 0) {
        if (special_type == 3) g_btn_mock_fd = fd;
        else if (special_type == 4) g_gyro_mock_fd = fd;
        else if (special_type == 5) g_touch_mock_fd = fd;
    }
    return fd;
}

FILE *fopen(const char *path, const char *mode) {
    init_real_functions();
    ensure_mock_dirs();

    char resolved[512];
    int special_type = 0;
    const char *target = resolve_mock_path(path, resolved, sizeof(resolved), &special_type);

    FILE *f = real_fopen(target, mode);
    if (!f && strstr(target, "/sysfs/") != NULL && mode[0] == 'r') {
        /* Create default fallback sysfs values on read if file missing */
        FILE *w = real_fopen(target, "w");
        if (w) {
            if (strstr(target, "powersource")) fputs("AC\n", w);
            else if (strstr(target, "vbat")) fputs("4150\n", w);
            else if (strstr(target, "charger")) fputs("CHARGING\n", w);
            else if (strstr(target, "backlight")) fputs("100\n", w);
            else if (strstr(target, "fb0_blank")) fputs("0\n", w);
            else if (strstr(target, "gyro_xyz")) fputs("0 0 980\n", w);
            else fputs("0\n", w);
            fclose(w);
            f = real_fopen(target, mode);
        }
    }
    return f;
}

FILE *fopen64(const char *path, const char *mode) {
    init_real_functions();
    ensure_mock_dirs();

    char resolved[512];
    int special_type = 0;
    const char *target = resolve_mock_path(path, resolved, sizeof(resolved), &special_type);
    FILE *f = real_fopen64(target, mode);
    if (!f && strstr(target, "/sysfs/") != NULL && mode[0] == 'r') {
        FILE *w = real_fopen(target, "w");
        if (w) {
            if (strstr(target, "powersource")) fputs("AC\n", w);
            else if (strstr(target, "vbat")) fputs("4150\n", w);
            else if (strstr(target, "charger")) fputs("CHARGING\n", w);
            else fputs("0\n", w);
            fclose(w);
            f = real_fopen64(target, mode);
        }
    }
    return f;
}

int ioctl(int fd, unsigned long request, ...) {
    init_real_functions();

    void *arg = NULL;
    va_list va;
    va_start(va, request);
    arg = va_arg(va, void *);
    va_end(va);

    /* Framebuffer ioctl emulation */
    if (fd == g_fb_mock_fd && g_fb_mock_fd >= 0) {
        if (request == FBIOGET_FSCREENINFO) {
            struct fb_fix_screeninfo *finfo = (struct fb_fix_screeninfo *)arg;
            if (!finfo) return -EINVAL;
            memset(finfo, 0, sizeof(*finfo));
            strncpy(finfo->id, "HarmonyEliteFB", sizeof(finfo->id));
            finfo->smem_len = 240 * 320 * 2;
            finfo->line_length = 240 * 2;
            return 0;
        }
        if (request == FBIOGET_VSCREENINFO) {
            struct fb_var_screeninfo *vinfo = (struct fb_var_screeninfo *)arg;
            if (!vinfo) return -EINVAL;
            memset(vinfo, 0, sizeof(*vinfo));
            vinfo->xres = 240;
            vinfo->yres = 320;
            vinfo->xres_virtual = 240;
            vinfo->yres_virtual = 320;
            vinfo->bits_per_pixel = 16;
            vinfo->red.offset = 11;
            vinfo->red.length = 5;
            vinfo->green.offset = 5;
            vinfo->green.length = 6;
            vinfo->blue.offset = 0;
            vinfo->blue.length = 5;
            return 0;
        }
        if (request == FBIOPAN_DISPLAY || request == FBIOBLANK) {
            return 0;
        }
    }

    /* Input device identification ioctl emulation */
    if ((request & ~IOCSIZE_MASK) == (EVIOCGNAME(0) & ~IOCSIZE_MASK)) {
        size_t len = _IOC_SIZE(request);
        char *name = (char *)arg;
        if (name && len > 0) {
            if (fd == g_touch_mock_fd) {
                strncpy(name, "Synaptics Clearpad Touchscreen", len - 1);
                name[len - 1] = '\0';
                return strlen(name);
            }
            if (fd == g_btn_mock_fd) {
                strncpy(name, "tps6507x_keys", len - 1);
                name[len - 1] = '\0';
                return strlen(name);
            }
            if (fd == g_gyro_mock_fd) {
                strncpy(name, "lis3dh_acc", len - 1);
                name[len - 1] = '\0';
                return strlen(name);
            }
        }
    }

    return real_ioctl(fd, request, arg);
}

int close(int fd) {
    init_real_functions();
    if (fd == g_fb_mock_fd) g_fb_mock_fd = -1;
    if (fd == g_touch_mock_fd) g_touch_mock_fd = -1;
    if (fd == g_btn_mock_fd) g_btn_mock_fd = -1;
    if (fd == g_gyro_mock_fd) g_gyro_mock_fd = -1;
    if (fd == g_rf_mock_fd) g_rf_mock_fd = -1;
    return real_close(fd);
}
