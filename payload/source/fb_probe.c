#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <stdint.h>
#include <string.h>

int main() {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("open fb0"); return 1; }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    ioctl(fd, FBIOGET_FSCREENINFO, &finfo);
    ioctl(fd, FBIOGET_VSCREENINFO, &vinfo);

    printf("Panned before: yoffset=%u\n", vinfo.yoffset);

    uint16_t *mem = (uint16_t *)mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }

    /* Fill active buffer (at yoffset) with Cyan (0x07FF) */
    uint16_t *active_buf = mem + (vinfo.yoffset * finfo.line_length / 2);
    for (int i = 0; i < 240 * 320; i++) {
        active_buf[i] = 0x07FF; /* Cyan */
    }

    /* Also fill buffer 0 with Magenta (0xF81F) and pan to 0 */
    for (int i = 0; i < 240 * 320; i++) {
        mem[i] = 0xF81F;
    }

    vinfo.yoffset = 0;
    vinfo.xoffset = 0;
    if (ioctl(fd, FBIOPAN_DISPLAY, &vinfo) < 0) perror("FBIOPAN_DISPLAY");

    printf("Panned to 0 successfully!\n");
    close(fd);
    return 0;
}
