/* config_push_worker() and the icons of on-screen buttons: a remote that reports
 * RF_CAP_BUTTON_ICONS gets an RF_MSG_BUTTON_ICON right after the BUTTON_LIST_CHUNK of each
 * on-screen button that has a stock icon, with that chunk's index and the icon's stock id; a
 * button without an icon, or with a name no stock icon has, gets none; an older remote gets none
 * at all. Same harness as test_rf_push_steps.c: the real hub_rf sources, /dev/rfspi a socket
 * pair, a radio thread that ACKs every frame. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>

#include "rf_state.h"
#include "rf_spi.h"
#include "rf_config_push.h"
#include "remote_stock_icons.h"

typedef struct {
    uint8_t type;
    uint8_t idx;     /* payload[0]: the button's index in the list */
    uint8_t ctype;   /* BUTTON_LIST_CHUNK: 0x01 on-screen button */
    char label[13];  /* BUTTON_LIST_CHUNK */
    uint16_t icon;   /* BUTTON_ICON */
} frame_t;

static int s_radio_fd = -1;
static volatile int s_stop;
static frame_t s_frames[3][128]; /* per slot, the button frames in the order sent */
static int s_frame_count[3];

static void *radio_worker(void *arg) {
    (void)arg;
    uint8_t buf[64];
    while (!s_stop) {
        ssize_t n = recv(s_radio_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (n >= 30 && buf[0] == 0x12) {
            const uint8_t *p = &buf[5];
            int slot = buf[1] & 0x0F;
            if ((buf[3] == RF_MSG_BUTTON_LIST_CHUNK || buf[3] == RF_MSG_BUTTON_ICON) && slot >= 1 && slot <= 2 &&
                s_frame_count[slot] < 128) {
                frame_t *fr = &s_frames[slot][s_frame_count[slot]++];
                memset(fr, 0, sizeof(*fr));
                fr->type = buf[3];
                fr->idx = p[0];
                if (buf[3] == RF_MSG_BUTTON_LIST_CHUNK) {
                    fr->ctype = p[2];
                    memcpy(fr->label, &p[12], 12);
                } else {
                    fr->icon = (uint16_t)(p[1] | (p[2] << 8));
                }
            }
            usleep(1000);
            rf_signal_ack(buf[2] & 0x0F, 0, NULL);
        }
    }
    return NULL;
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static void run_push(void) {
    memset(s_frame_count, 0, sizeof(s_frame_count));
    s_stop = 0;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) { perror("socketpair"); exit(1); }
    g_rf_fd = sv[0];
    s_radio_fd = sv[1];
    pthread_t th;
    pthread_create(&th, NULL, radio_worker, NULL);
    config_push_worker(NULL);
    s_stop = 1;
    shutdown(sv[0], SHUT_RDWR);
    pthread_join(th, NULL);
    close(sv[0]);
    close(sv[1]);
}

/* The frame after the on-screen button labelled label sent to slot, NULL when it is the last */
static const frame_t *after_button(int slot, const char *label, const frame_t **button) {
    for (int i = 0; i < s_frame_count[slot]; i++) {
        const frame_t *fr = &s_frames[slot][i];
        if (fr->type == RF_MSG_BUTTON_LIST_CHUNK && fr->ctype == 0x01 && strcmp(fr->label, label) == 0) {
            if (button) *button = fr;
            return i + 1 < s_frame_count[slot] ? &s_frames[slot][i + 1] : NULL;
        }
    }
    if (button) *button = NULL;
    return NULL;
}

static int icon_frames(int slot) {
    int n = 0;
    for (int i = 0; i < s_frame_count[slot]; i++) n += s_frames[slot][i].type == RF_MSG_BUTTON_ICON;
    return n;
}

static int s_fail;
#define CHECK(cond, msg) do { if (!(cond)) { printf("[-] %s\n", msg); s_fail = 1; } else printf("[+] %s\n", msg); } while (0)

int main(void) {
    write_file("/data/harmony/config/devices.json",
               "{\"devices\": [{\"id\": \"4\", \"name\": \"Lamp\", \"commands\": []}]}");
    write_file("/data/harmony/config/activities.json",
               "{\"activities\": [{\"id\": \"1\", \"name\": \"Watch\", \"deviceIds\": [\"4\"]}]}");
    const char *map =
        "{\"activities\": {\"1\": {\"screen_buttons\": ["
        "{\"label\": \"Salon\", \"icon\": \"bulb.png\", \"deviceId\": \"4\", \"command\": \"PowerToggle\"},"
        "{\"label\": \"Plain\", \"deviceId\": \"4\", \"command\": \"PowerToggle\"},"
        "{\"label\": \"Odd\", \"icon\": \"no-such-icon.png\", \"deviceId\": \"4\", \"command\": \"PowerToggle\"},"
        "{\"label\": \"Table\", \"icon\": \"bulb.png\", \"deviceId\": \"4\", \"command\": \"PowerToggle\"}"
        "]}}}";
    write_file("/data/harmony/remote_slot1_map.json", map);
    write_file("/data/harmony/remote_slot2_map.json", map);

    for (int i = 0; i < 2; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = true;
        g_paired_remotes[i].heard = true; /* both reported their capabilities */
        g_paired_remotes[i].model = REMOTE_MODEL_JUNIPER;
        g_paired_remotes[i].equad_id = RF_EQUAD_JUNIPER;
    }
    g_remote_versions[0].caps = RF_CAP_LONG_NAMES | RF_CAP_BUTTON_ICONS; /* slot 1: new remote */
    g_remote_versions[1].caps = RF_CAP_LONG_NAMES;                       /* slot 2: older remote */
    g_has_paired_device = true;
    g_remote_slot = 1;
    g_config_version = 7;

    run_push();

    uint16_t bulb = lookup_stock_icon_id("bulb.png");
    CHECK(bulb != 0, "bulb.png is a stock icon (the test needs one)");

    const frame_t *btn = NULL;
    const frame_t *next = after_button(1, "Salon", &btn);
    CHECK(btn && next && next->type == RF_MSG_BUTTON_ICON && next->idx == btn->idx && next->icon == bulb,
          "a new remote gets the icon right after the button, with the button's index and the stock id");
    next = after_button(1, "Table", &btn);
    CHECK(btn && next && next->type == RF_MSG_BUTTON_ICON && next->idx == btn->idx && next->icon == bulb,
          "each button with an icon gets its own frame (the second one too)");
    next = after_button(1, "Plain", &btn);
    CHECK(btn && (!next || next->type != RF_MSG_BUTTON_ICON), "a button without an icon gets no icon frame");
    next = after_button(1, "Odd", &btn);
    CHECK(btn && (!next || next->type != RF_MSG_BUTTON_ICON), "an icon name no stock icon has gets no icon frame");
    CHECK(icon_frames(1) == 2, "two icon frames in all for the new remote");

    after_button(2, "Salon", &btn);
    CHECK(btn != NULL && icon_frames(2) == 0, "an older remote gets the buttons and no icon frame");

    if (s_fail) {
        printf("[-] test_rf_push_button_icons: FAILED\n");
        return 1;
    }
    printf("[+] test_rf_push_button_icons: all checks passed\n");
    return 0;
}
