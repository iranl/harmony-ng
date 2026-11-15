/*
 * test_remote_theme_font.c - a theme font file whose data length field is near 4 GiB must be
 * refused before anything is allocated. On the remote (32-bit ARM) long is 32 bits: the old
 * length check converted the length to long, which made it negative, the check passed, the
 * allocation size wrapped to a few hundred bytes and memcpy() copied about 4 GiB into it.
 *
 * remote_theme.c is compiled into this test with long defined as int, so the host runs the
 * checks with the remote's widths; malloc() is replaced to record every request (requests above
 * 1 MiB fail, as the test only wants to see them).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "remote/remote_theme.h"
#include "remote/remote_state.h"

static size_t s_biggest_request;
static void *test_malloc(size_t n) {
    if (n > s_biggest_request) s_biggest_request = n;
    return n > (1u << 20) ? NULL : malloc(n);
}

#define malloc test_malloc
#define long int
#include "../payload/source/remote/remote_theme.c"
#undef long
#undef malloc

int main(void) {
    char path[] = "/tmp/hng_font_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    uint8_t file[4 + 2 + 4 + 95 * 7 + 16] = {0};
    memcpy(file, "FAA1", 4);
    file[4] = 12;
    file[5] = 10;
    uint32_t data_len = 0xFFFFFF00u; /* the file has 16 bytes of data */
    file[6] = data_len & 0xFF;
    file[7] = (data_len >> 8) & 0xFF;
    file[8] = (data_len >> 16) & 0xFF;
    file[9] = (data_len >> 24) & 0xFF;
    if (write(fd, file, sizeof(file)) != (ssize_t)sizeof(file)) return 1;
    close(fd);

    font_aa_t *font = load_font(path);
    unlink(path);
    if (font) {
        printf("[-] test_remote_theme_font: a font with a data length past the file end was loaded\n");
        return 1;
    }
    if (s_biggest_request > (1u << 20)) {
        printf("[-] test_remote_theme_font: the length check passed and %zu bytes were requested\n", s_biggest_request);
        return 1;
    }

    /* A good font still loads */
    fd = mkstemp(strcpy(path, "/tmp/hng_font_XXXXXX"));
    file[6] = 16; file[7] = 0; file[8] = 0; file[9] = 0;
    if (write(fd, file, sizeof(file)) != (ssize_t)sizeof(file)) return 1;
    close(fd);
    font = load_font(path);
    unlink(path);
    if (!font) {
        printf("[-] test_remote_theme_font: a well-formed font was refused\n");
        return 1;
    }
    free(font);
    printf("[+] test_remote_theme_font: a data length past the file end is refused with the remote's 32-bit long\n");
    return 0;
}
