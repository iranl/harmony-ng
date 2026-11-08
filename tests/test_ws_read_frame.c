/*
 * ws_read_frame() return values on a socket pair: nothing to read (spurious wakeup) is
 * WS_READ_AGAIN and keeps the client, an empty pong is 0, a masked text frame is its length,
 * a closed peer is -1.
 */
#include "ws_server.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fail = 0;

static void check(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) g_fail++;
}

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return 1;
    }
    char buf[64];
    int op = -1;

    int r = ws_read_frame(sv[0], buf, sizeof(buf), &op);
    check("nothing to read: WS_READ_AGAIN", r == WS_READ_AGAIN);

    const unsigned char pong[] = {0x8A, 0x80, 1, 2, 3, 4};
    write(sv[1], pong, sizeof(pong));
    r = ws_read_frame(sv[0], buf, sizeof(buf), &op);
    check("empty pong: 0, opcode pong", r == 0 && op == WS_OP_PONG);

    const char *txt = "hi";
    unsigned char fr[8] = {0x81, 0x80 | 2, 1, 2, 3, 4};
    fr[6] = (unsigned char)(txt[0] ^ 1);
    fr[7] = (unsigned char)(txt[1] ^ 2);
    write(sv[1], fr, sizeof(fr));
    r = ws_read_frame(sv[0], buf, sizeof(buf), &op);
    check("masked text: length and payload", r == 2 && op == WS_OP_TEXT && strcmp(buf, "hi") == 0);

    close(sv[1]);
    r = ws_read_frame(sv[0], buf, sizeof(buf), &op);
    check("peer closed: -1", r == -1);
    close(sv[0]);

    printf("%s\n", g_fail ? "ws_read_frame: FAILED" : "ws_read_frame: all passed");
    return g_fail ? 1 : 0;
}
