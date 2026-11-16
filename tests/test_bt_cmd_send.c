/* How hub_daemon hands commands to hub_btstack (bt_common.h), on the host with its own socket and
 * file paths:
 *   - hub_send_btstack_cmd_live(): over the socket only; -1 when no hub_btstack listens (POST
 *     /api/bt-forget-host then answers 500 instead of leaving the command in a file that a later
 *     start of hub_btstack drops), 0 and the command delivered when one listens
 *   - hub_send_btstack_cmd(): the same socket, else the command file
 *   - dry run: printed, nothing sent */
#define TEST_DIR "/tmp/hng_bt_cmd_send_test"
#define HUB_BTSTACK_SOCK TEST_DIR "/hub_btstack.sock"
#define HUB_BTSTACK_CMD_FILE TEST_DIR "/hub_btstack_cmd"
#include <stdlib.h>
#include <sys/stat.h>
#include "bt_common.h"

int g_dry_run = 0;
static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("[-] FAIL: " __VA_ARGS__); printf("\n"); s_fail = 1; } else { printf("  ok  " __VA_ARGS__); printf("\n"); } } while (0)

static int listen_sock(void) {
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, HUB_BTSTACK_SOCK, sizeof(sun.sun_path) - 1);
    unlink(HUB_BTSTACK_SOCK);
    if (s < 0 || bind(s, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
        printf("[-] FAIL: cannot bind the test socket\n");
        s_fail = 1;
        return -1;
    }
    return s;
}

static int file_has(const char *text) {
    FILE *f = fopen(HUB_BTSTACK_CMD_FILE, "r");
    char buf[128] = "";
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = '\0';
    return strstr(buf, text) != NULL;
}

int main(void) {
    if (system("rm -rf " TEST_DIR " && mkdir -p " TEST_DIR) != 0) return 1;

    printf("[-] no hub_btstack listening\n");
    CHECK(hub_send_btstack_cmd_live("host_forget 00:11:22:33:44:01\n") == -1, "the live send fails");
    CHECK(access(HUB_BTSTACK_CMD_FILE, F_OK) != 0, "and leaves no command file");
    CHECK(hub_send_btstack_cmd("reload_map") == 0 && file_has("reload_map"), "the other send falls back to the command file");
    unlink(HUB_BTSTACK_CMD_FILE);

    printf("[-] hub_btstack listening\n");
    int s = listen_sock();
    if (s < 0) return 1;
    char buf[128];
    CHECK(hub_send_btstack_cmd_live("host_forget 00:11:22:33:44:01\n") == 0, "the live send succeeds");
    ssize_t n = recv(s, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    buf[n > 0 ? n : 0] = '\0';
    CHECK(strcmp(buf, "host_forget 00:11:22:33:44:01\n") == 0, "and hub_btstack gets the command: '%s'", buf);
    CHECK(hub_send_btstack_cmd("reload_map") == 0 && access(HUB_BTSTACK_CMD_FILE, F_OK) != 0, "the other send uses the socket too");
    n = recv(s, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    CHECK(n == 10, "and it arrives");
    close(s);
    unlink(HUB_BTSTACK_SOCK);

    printf("[-] dry run\n");
    g_dry_run = 1;
    CHECK(hub_send_btstack_cmd_live("host_forget 00:11:22:33:44:01\n") == 0 && access(HUB_BTSTACK_CMD_FILE, F_OK) != 0, "printed, nothing sent");
    CHECK(hub_send_btstack_cmd_live("") == -1, "an empty command is refused");
    CHECK(hub_btstack_cmd_dry("") == 1 && hub_btstack_cmd_dry(NULL) == 1, "the dry-run printer takes an empty or missing command (it read before the string)");

    if (system("rm -rf " TEST_DIR) != 0) return 1;
    if (s_fail) return 1;
    printf("[+] test_bt_cmd_send: commands reach hub_btstack, or the caller knows they did not\n");
    return 0;
}
