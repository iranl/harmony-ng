/* Standalone base64 decoder for ARM Linux EABI */
#define __NR_exit  1
#define __NR_read  3
#define __NR_write 4
#define __NR_open  5
#define __NR_close 6

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000

static inline int sys_open(const char *path, int flags, int mode) {
    register int r0 __asm__("r0") = (int)path;
    register int r1 __asm__("r1") = flags;
    register int r2 __asm__("r2") = mode;
    register int r7 __asm__("r7") = __NR_open;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

static inline int sys_read(int fd, void *buf, int count) {
    register int r0 __asm__("r0") = fd;
    register int r1 __asm__("r1") = (int)buf;
    register int r2 __asm__("r2") = count;
    register int r7 __asm__("r7") = __NR_read;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

static inline int sys_write(int fd, const void *buf, int count) {
    register int r0 __asm__("r0") = fd;
    register int r1 __asm__("r1") = (int)buf;
    register int r2 __asm__("r2") = count;
    register int r7 __asm__("r7") = __NR_write;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

static inline void sys_close(int fd) {
    register int r0 __asm__("r0") = fd;
    register int r7 __asm__("r7") = __NR_close;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r7) : "memory");
}

static inline void sys_exit(int code) {
    register int r0 __asm__("r0") = code;
    register int r7 __asm__("r7") = __NR_exit;
    __asm__ volatile("svc 0" : : "r"(r0), "r"(r7) : "memory");
    while (1);
}

void main_start(int argc, const char *infile, const char *outfile) {
    if (argc < 3) sys_exit(1);

    signed char lut[256];
    for (int i = 0; i < 256; i++) lut[i] = -1;
    const char *b = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) lut[(unsigned char)b[i]] = i;

    int infd = sys_open(infile, O_RDONLY, 0);
    if (infd < 0) sys_exit(2);
    int outfd = sys_open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (outfd < 0) sys_exit(3);

    unsigned char inbuf[4096];
    unsigned char outbuf[3072];
    int carry = 0;
    int carry_val = 0;

    while (1) {
        int n = sys_read(infd, inbuf, sizeof(inbuf));
        if (n <= 0) break;
        int out_pos = 0;
        for (int i = 0; i < n; i++) {
            signed char v = lut[inbuf[i]];
            if (v < 0) continue;
            carry_val = (carry_val << 6) | v;
            carry++;
            if (carry == 4) {
                outbuf[out_pos++] = (carry_val >> 16) & 0xFF;
                outbuf[out_pos++] = (carry_val >> 8) & 0xFF;
                outbuf[out_pos++] = carry_val & 0xFF;
                carry = 0;
                carry_val = 0;
            }
        }
        if (out_pos > 0) {
            sys_write(outfd, outbuf, out_pos);
        }
    }
    int out_pos = 0;
    if (carry == 2) {
        outbuf[out_pos++] = (carry_val >> 4) & 0xFF;
    } else if (carry == 3) {
        outbuf[out_pos++] = (carry_val >> 10) & 0xFF;
        outbuf[out_pos++] = (carry_val >> 2) & 0xFF;
    }
    if (out_pos > 0) {
        sys_write(outfd, outbuf, out_pos);
    }
    sys_close(infd);
    sys_close(outfd);
    sys_exit(0);
}

__attribute__((naked)) void _start(void) {
    __asm__ volatile (
        "ldr r0, [sp]\n"       /* argc */
        "ldr r1, [sp, #8]\n"   /* argv[1] */
        "ldr r2, [sp, #12]\n"  /* argv[2] */
        "b main_start\n"
    );
}
