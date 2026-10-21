#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

int main() {
    int fd = open("/dev/rfspi", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    uint8_t close_pkt[7] = {0x10, 0xFF, 0x80, 0xB2, 0x02, 0x00, 0x00};
    write(fd, close_pkt, 7);
    close(fd);
    printf("Sent close lock\n");
    return 0;
}
