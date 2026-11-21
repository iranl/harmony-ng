/* config_push_worker() step list: an activity starts with one "Warmup" step per device the Hub's
 * orchestrator may wait for (its powerOnDelay, 1500 ms when it has none; not for always-on
 * devices or devices the start sequence powers on), longest first, with the device in
 * payload[21]; only a remote that reports RF_CAP_SWITCH_START gets them. The default steps stay
 * when the activity has no start sequence, and a Delay before the first command is a step of its
 * own. Same harness as test_rf_push_targets.c: the real hub_rf sources, /dev/rfspi a socket pair,
 * a radio thread that ACKs every frame. */
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

typedef struct {
    char act_id[10];
    char name[12];
    unsigned delay;
    unsigned step_idx;
    unsigned total;
    unsigned warm_dev;
    unsigned chunk;
} step_t;

static int s_radio_fd = -1;
static volatile int s_stop;
static step_t s_steps[3][64]; /* per slot */
static int s_step_count[3];

static void *radio_worker(void *arg) {
    (void)arg;
    uint8_t buf[64];
    while (!s_stop) {
        ssize_t n = recv(s_radio_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (n >= 30 && buf[0] == 0x12) {
            const uint8_t *p = &buf[5];
            int slot = buf[1] & 0x0F;
            if (buf[3] == RF_MSG_STEP_LIST_CHUNK && slot >= 1 && slot <= 2 && s_step_count[slot] < 64) {
                step_t *s = &s_steps[slot][s_step_count[slot]++];
                memcpy(s->act_id, &p[3], 9);
                s->act_id[9] = '\0';
                memcpy(s->name, &p[14], 11);
                s->name[11] = '\0';
                s->delay = (unsigned)p[12] | ((unsigned)p[13] << 8);
                s->step_idx = p[1];
                s->total = p[2];
                s->warm_dev = p[21];
                s->chunk = p[0];
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

/* The start steps (step_idx without bit 7) of act_id sent to slot, in order */
static int start_steps(int slot, const char *act_id, const step_t **out, int max) {
    int n = 0;
    for (int i = 0; i < s_step_count[slot] && n < max; i++) {
        const step_t *st = &s_steps[slot][i];
        if (strcmp(st->act_id, act_id) == 0 && !(st->step_idx & 0x80)) out[n++] = st;
    }
    return n;
}

static int s_fail;
#define CHECK(cond, msg) do { if (!(cond)) { printf("[-] %s\n", msg); s_fail = 1; } else printf("[+] %s\n", msg); } while (0)

int main(void) {
    write_file("/data/harmony/config/devices.json",
               "{\"devices\": ["
               "{\"id\": \"1\", \"name\": \"TV\", \"powerOnDelay\": 4000, \"commands\": []},"
               "{\"id\": \"2\", \"name\": \"Shield\", \"powerOnDelay\": 9000, \"commands\": []},"
               "{\"id\": \"3\", \"name\": \"Amp\", \"powerOnDelay\": 0, \"commands\": []},"
               "{\"id\": \"4\", \"name\": \"Hue\", \"powerOnDelay\": 30000, \"isPowerAlwaysOn\": true, \"commands\": []},"
               "{\"id\": \"5\", \"name\": \"Radio\", \"commands\": []},"
               "{\"id\": \"6\", \"name\": \"Beamer\", \"powerOnDelay\": 50000, \"commands\": []}"
               "]}");
    write_file("/data/harmony/config/activities.json",
               "{\"activities\": ["
               "{\"id\": \"10\", \"name\": \"Watch\", \"deviceIds\": [\"1\", \"2\", \"4\"],"
               " \"startSequence\": [{\"type\": \"delay\", \"delay\": 700},"
               "                     {\"command\": \"InputHdmi1\", \"delay\": 300}]},"
               "{\"id\": \"11\", \"name\": \"Music\", \"deviceIds\": [\"3\"]},"
               "{\"id\": \"12\", \"name\": \"Radio\", \"deviceIds\": [\"5\", \"6\"],"
               " \"startSequence\": [{\"deviceId\": \"6\", \"command\": \"PowerOn\", \"delay\": 500}]}"
               "]}");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) { perror("socketpair"); return 1; }
    g_rf_fd = sv[0];
    s_radio_fd = sv[1];
    pthread_t th;
    pthread_create(&th, NULL, radio_worker, NULL);

    for (int i = 0; i < 2; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = true;
        g_paired_remotes[i].model = REMOTE_MODEL_JUNIPER;
        g_paired_remotes[i].equad_id = RF_EQUAD_JUNIPER;
    }
    g_remote_versions[0].caps = RF_CAP_LONG_NAMES | RF_CAP_SWITCH_START; /* slot 1: new remote */
    g_remote_versions[1].caps = RF_CAP_LONG_NAMES;                       /* slot 2: older remote */
    g_has_paired_device = true;
    g_remote_slot = 1;
    g_config_version = 7;

    config_push_worker(NULL);

    s_stop = 1;
    shutdown(sv[0], SHUT_RDWR);
    pthread_join(th, NULL);

    const step_t *st[20];
    int n = start_steps(1, "10", st, 20);
    CHECK(n == 4, "activity 10 has two warmup steps, a delay and a command step");
    CHECK(n >= 2 && strcmp(st[0]->name, "Warmup") == 0 && st[0]->delay == 9000 && st[0]->warm_dev == 2 &&
          strcmp(st[1]->name, "Warmup") == 0 && st[1]->delay == 4000 && st[1]->warm_dev == 1,
          "one Warmup step per device, longest first, with 1 + its device index; none for an always-on device");
    CHECK(n >= 3 && strcmp(st[2]->name, "Delay") == 0 && st[2]->delay == 700,
          "a delay before the first command (any case) is a step of its own");
    CHECK(n >= 4 && strcmp(st[3]->name, "InputHdmi1") == 0 && st[3]->delay == 300,
          "the command step follows");
    CHECK(n >= 4 && st[0]->total == 4 && st[3]->step_idx == 3, "step numbers count the warmup steps");

    n = start_steps(1, "11", st, 20);
    CHECK(n == 2 && strcmp(st[0]->name, "Power On") == 0 && strcmp(st[1]->name, "Set Inputs") == 0,
          "no warmup for a device with powerOnDelay 0; the default steps stay");

    n = start_steps(1, "12", st, 20);
    CHECK(n == 2 && strcmp(st[0]->name, "Warmup") == 0 && st[0]->delay == 1500 && st[0]->warm_dev == 5,
          "a device without powerOnDelay waits 1500 ms; none for a device the start sequence powers on");

    n = start_steps(1, "-1", st, 20);
    CHECK(n >= 1 && strcmp(st[0]->name, "Warmup") != 0, "no warmup before PowerOff");

    n = start_steps(2, "10", st, 20);
    CHECK(n == 2 && strcmp(st[0]->name, "Delay") == 0 && st[0]->step_idx == 0 && st[0]->total == 2 &&
          st[1]->step_idx == 1, "an older remote gets no Warmup steps, numbered without them");
    int contiguous = s_step_count[2] > 0;
    for (int i = 0; i < s_step_count[2]; i++) contiguous = contiguous && s_steps[2][i].chunk == (unsigned)i;
    CHECK(contiguous && s_step_count[2] == s_step_count[1] - 3, "its chunk indexes have no gaps");

    if (s_fail) {
        printf("[-] test_rf_push_steps: FAILED\n");
        return 1;
    }
    printf("[+] test_rf_push_steps: all checks passed\n");
    return 0;
}
