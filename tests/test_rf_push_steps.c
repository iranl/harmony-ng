/* config_push_worker() step list: an activity starts with a "Warmup" step of the longest
 * powerOnDelay of its devices (the Hub's orchestrator waits that long before the start sequence),
 * the default steps stay when the activity has no start sequence, and a Delay before the first
 * command is a step of its own. Same harness as test_rf_push_targets.c: the real hub_rf sources,
 * /dev/rfspi a socket pair, a radio thread that ACKs every frame. */
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
} step_t;

static int s_radio_fd = -1;
static volatile int s_stop;
static step_t s_steps[64];
static int s_step_count;

static void *radio_worker(void *arg) {
    (void)arg;
    uint8_t buf[64];
    while (!s_stop) {
        ssize_t n = recv(s_radio_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (n >= 30 && buf[0] == 0x12) {
            const uint8_t *p = &buf[5];
            /* Slot 1 only: every target gets the same list */
            if (buf[3] == RF_MSG_STEP_LIST_CHUNK && (buf[1] & 0x0F) == 1 && s_step_count < 64) {
                step_t *s = &s_steps[s_step_count++];
                memcpy(s->act_id, &p[3], 9);
                s->act_id[9] = '\0';
                memcpy(s->name, &p[14], 11);
                s->name[11] = '\0';
                s->delay = (unsigned)p[12] | ((unsigned)p[13] << 8);
                s->step_idx = p[1];
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

/* The start steps (step_idx without bit 7) of act_id, in order */
static int start_steps(const char *act_id, const step_t **out, int max) {
    int n = 0;
    for (int i = 0; i < s_step_count && n < max; i++) {
        if (strcmp(s_steps[i].act_id, act_id) == 0 && !(s_steps[i].step_idx & 0x80)) out[n++] = &s_steps[i];
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
               "{\"id\": \"3\", \"name\": \"Amp\", \"powerOnDelay\": 0, \"commands\": []}"
               "]}");
    write_file("/data/harmony/config/activities.json",
               "{\"activities\": ["
               "{\"id\": \"10\", \"name\": \"Watch\", \"deviceIds\": [\"1\", \"2\"],"
               " \"startSequence\": [{\"type\": \"delay\", \"delay\": 700},"
               "                     {\"command\": \"InputHdmi1\", \"delay\": 300}]},"
               "{\"id\": \"11\", \"name\": \"Music\", \"deviceIds\": [\"3\"]}"
               "]}");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) { perror("socketpair"); return 1; }
    g_rf_fd = sv[0];
    s_radio_fd = sv[1];
    pthread_t th;
    pthread_create(&th, NULL, radio_worker, NULL);

    g_paired_remotes[0].slot = 1;
    g_paired_remotes[0].active = true;
    g_paired_remotes[0].model = REMOTE_MODEL_JUNIPER;
    g_paired_remotes[0].equad_id = RF_EQUAD_JUNIPER;
    g_has_paired_device = true;
    g_remote_slot = 1;
    g_config_version = 7;

    config_push_worker(NULL);

    s_stop = 1;
    shutdown(sv[0], SHUT_RDWR);
    pthread_join(th, NULL);

    const step_t *st[16];
    int n = start_steps("10", st, 16);
    CHECK(n == 3, "activity 10 has warmup, delay and command steps");
    CHECK(n >= 1 && strcmp(st[0]->name, "Warmup") == 0 && st[0]->delay == 9000,
          "the first step waits the longest powerOnDelay of the activity's devices");
    CHECK(n >= 2 && strcmp(st[1]->name, "Delay") == 0 && st[1]->delay == 700,
          "a delay before the first command (any case) is a step of its own");
    CHECK(n >= 3 && strcmp(st[2]->name, "InputHdmi1") == 0 && st[2]->delay == 300, "the command step follows");

    n = start_steps("11", st, 16);
    CHECK(n == 2 && strcmp(st[0]->name, "Power On") == 0 && strcmp(st[1]->name, "Set Inputs") == 0,
          "no warmup for devices without a powerOnDelay; the default steps stay");

    n = start_steps("-1", st, 16);
    CHECK(n >= 1 && strcmp(st[0]->name, "Warmup") != 0, "no warmup before PowerOff");

    if (s_fail) {
        printf("[-] test_rf_push_steps: FAILED\n");
        return 1;
    }
    printf("[+] test_rf_push_steps: all checks passed\n");
    return 0;
}
