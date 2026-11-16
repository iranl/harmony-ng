/*
 * ws_read_frame() return values on a socket pair: nothing to read (spurious wakeup) is
 * WS_READ_AGAIN and keeps the client, an empty pong is 0, a masked text frame is its length,
 * a frame that arrives in pieces is WS_READ_AGAIN until it is complete (the main loop never
 * waits for the rest), the 64-bit length form is read, a frame larger than the buffer is -1,
 * a closed peer is -1, an fd that is not a client is -1.
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
    check("not a registered client: -1", r == -1);

    /* register sv[0] as a client: the 101 goes to sv[1] */
    ws_server_init();
    const char *upg = "GET /ws HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    check("upgrade registers the client", ws_handle_upgrade(sv[0], upg, 2, 0) == 0);
    char resp[256];
    ssize_t rn = read(sv[1], resp, sizeof(resp) - 1);
    check("101 sent", rn > 12 && strncmp(resp, "HTTP/1.1 101", 12) == 0);

    r = ws_read_frame(sv[0], buf, sizeof(buf), &op);
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

    /* the same frame in three pieces: AGAIN, AGAIN, then the frame */
    write(sv[1], fr, 1);
    r = ws_read_frame(sv[0], buf, sizeof(buf), &op);
    int r2 = 0;
    write(sv[1], fr + 1, 4);
    r2 = ws_read_frame(sv[0], buf, sizeof(buf), &op);
    write(sv[1], fr + 5, 3);
    int r3 = ws_read_frame(sv[0], buf, sizeof(buf), &op);
    check("frame in pieces: AGAIN until complete", r == WS_READ_AGAIN && r2 == WS_READ_AGAIN && r3 == 2 &&
          strcmp(buf, "hi") == 0);

    /* two frames in one write: the second is left in the socket for the next call */
    unsigned char two[16];
    memcpy(two, fr, 8);
    memcpy(two + 8, fr, 8);
    write(sv[1], two, sizeof(two));
    r = ws_read_frame(sv[0], buf, sizeof(buf), &op);
    r2 = ws_read_frame(sv[0], buf, sizeof(buf), &op);
    check("two frames in one write: read one by one", r == 2 && r2 == 2);

    /* 64-bit length form */
    unsigned char lf[16] = {0x81, 0x80 | 127, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 3, 4};
    lf[14] = (unsigned char)(txt[0] ^ 1);
    lf[15] = (unsigned char)(txt[1] ^ 2);
    write(sv[1], lf, sizeof(lf));
    r = ws_read_frame(sv[0], buf, sizeof(buf), &op);
    check("64-bit length form: read", r == 2 && strcmp(buf, "hi") == 0);

    /* 100 bytes announced for a 64-byte buffer: dropped, not cut */
    unsigned char big[6] = {0x81, 0x80 | 100, 1, 2, 3, 4};
    write(sv[1], big, sizeof(big));
    r = ws_read_frame(sv[0], buf, sizeof(buf), &op);
    check("frame larger than the buffer: -1", r == -1);

    close(sv[1]);
    r = ws_read_frame(sv[0], buf, sizeof(buf), &op);
    check("peer closed: -1", r == -1);
    close(sv[0]);

    printf("%s\n", g_fail ? "ws_read_frame: FAILED" : "ws_read_frame: all passed");
    return g_fail ? 1 : 0;
}
