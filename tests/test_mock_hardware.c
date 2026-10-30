#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdint.h>
#include <assert.h>

int main(void) {
    printf("[*] Running Mock Hardware Low-Level Verification...\n");

    /* 1. Framebuffer Verification */
    printf("[-] Testing /dev/fb0 mock & ioctl...\n");
    int fb_fd = open("/dev/fb0", O_RDWR);
    assert(fb_fd >= 0);

    struct fb_fix_screeninfo finfo;
    assert(ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0);
    assert(strcmp(finfo.id, "HarmonyEliteFB") == 0);
    assert(finfo.smem_len == 240 * 320 * 2);

    struct fb_var_screeninfo vinfo;
    assert(ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0);
    assert(vinfo.xres == 240);
    assert(vinfo.yres == 320);
    assert(vinfo.bits_per_pixel == 16);

    uint16_t *mem = (uint16_t *)mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    assert(mem != MAP_FAILED);

    /* Test drawing a pixel */
    mem[120 * 240 + 60] = 0x067A; /* Harmony Teal */
    assert(mem[120 * 240 + 60] == 0x067A);
    munmap(mem, finfo.smem_len);
    close(fb_fd);
    printf("[+] Framebuffer mock verified successfully!\n");

    /* 2. Input Device Identification Verification */
    printf("[-] Testing /dev/input device names & identification...\n");
    int touch_fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
    if (touch_fd >= 0) {
        char name[128] = {0};
        assert(ioctl(touch_fd, EVIOCGNAME(sizeof(name)), name) >= 0);
        assert(strstr(name, "Clearpad") != NULL);
        close(touch_fd);
    }

    int btn_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (btn_fd >= 0) {
        char name[128] = {0};
        assert(ioctl(btn_fd, EVIOCGNAME(sizeof(name)), name) >= 0);
        assert(strstr(name, "tps6507x") != NULL);
        close(btn_fd);
    }

    int gyro_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (gyro_fd >= 0) {
        char name[128] = {0};
        assert(ioctl(gyro_fd, EVIOCGNAME(sizeof(name)), name) >= 0);
        assert(strstr(name, "lis3dh") != NULL);
        close(gyro_fd);
    }
    printf("[+] Input device naming verified successfully!\n");

    /* 3. Sysfs Virtualization Verification */
    printf("[-] Testing Sysfs power & backlight virtualization...\n");
    FILE *fp = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/powersource", "r");
    assert(fp != NULL);
    char buf[64] = {0};
    assert(fgets(buf, sizeof(buf), fp) != NULL);
    assert(strlen(buf) > 0);
    fclose(fp);

    FILE *fv = fopen("/sys/devices/platform/i2c_davinci.1/i2c-1/1-0048/tps6507x-juniper/vbat", "r");
    assert(fv != NULL);
    char vbuf[64] = {0};
    assert(fgets(vbuf, sizeof(vbuf), fv) != NULL);
    int mv = atoi(vbuf);
    assert(mv >= 3000 && mv <= 4500);
    fclose(fv);

    FILE *fbk = fopen("/sys/class/graphics/fb0/blank", "w");
    assert(fbk != NULL);
    fputs("1\n", fbk);
    fclose(fbk);
    printf("[+] Sysfs virtualization verified successfully!\n");

    printf("\n[+] ALL MOCK HARDWARE LOW-LEVEL CHECKS PASSED!\n");
    return 0;
}
