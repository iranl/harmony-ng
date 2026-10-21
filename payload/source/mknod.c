#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 5) {
        printf("Usage: mknod <path> <c|b> <major> <minor>\n");
        return 1;
    }
    mode_t mode = 0666;
    if (argv[2][0] == 'c') mode |= S_IFCHR;
    else if (argv[2][0] == 'b') mode |= S_IFBLK;
    unsigned int maj = (unsigned int)atoi(argv[3]);
    unsigned int min = (unsigned int)atoi(argv[4]);
    dev_t dev = ((maj & 0xfff) << 8) | (min & 0xff);
    if (mknod(argv[1], mode, dev) < 0) {
        perror("mknod");
        return 1;
    }
    return 0;
}
