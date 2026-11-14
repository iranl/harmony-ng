#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, argv[1], sizeof(sun.sun_path) - 1);
    const char *msg = "{\"command\":\"Home\",\"ip_address\":\"192.168.1.50\",\"action\":\"tap\"}";
    ssize_t n = sendto(s, msg, strlen(msg), 0, (struct sockaddr*)&sun, sizeof(sun));
    if (n < 0) perror("sendto");
    else printf("sendto %s returned %ld\n", argv[1], (long)n);
    close(s);
    return 0;
}
