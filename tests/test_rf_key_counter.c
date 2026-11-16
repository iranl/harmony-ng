/* A key frame from the touchscreen remote (RF_MSG_BUTTON_PRESS) is sent once, with no
 * acknowledgement and no resend. A remote that numbers its key frames (payload[19], 1..255 then 1
 * again, 0 = no counter) lets the Hub count the ones that did not arrive. Runs the real hub_rf
 * frame handler (main renamed) on key frames with chosen counters and reads the per-slot counters,
 * the log lines and the RF status object. */
#undef main
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "rf_state.h"
#include "rf_spi.h"
#include "rf_api.h"
#include "cJSON.h"

#define K_RIGHT 0x004F /* DirectionRight */
#define K_LEFT  0x0050 /* DirectionLeft */
#define K_VOLUP 0x00E9 /* VolumeUp */
#define PRESS   0x01
#define HOLD    0x02
#define RELEASE 0x00

static FILE *s_out; /* the test's own output; stdout (hub_rf's log) goes to a file */
static int s_failures = 0;
#define CHECK(cond, ...) do { \
    if (cond) { fprintf(s_out, "  [ok] " __VA_ARGS__); } \
    else { fprintf(s_out, "  [FAIL] line %d: ", __LINE__); fprintf(s_out, __VA_ARGS__); s_failures++; } \
    fprintf(s_out, "\n"); \
} while (0)

static void frame_from(uint8_t slot, uint8_t type, const uint8_t *payload, size_t len) {
    rf_frame_t f;
    memset(&f, 0, sizeof(f));
    f.report_id = 0x20;
    f.dev_slot = slot;
    f.sub_id = 0x12;
    f.hdr_flag = 0x41;
    f.seq = 1;
    f.msg_type = type;
    memcpy(f.payload, payload, len);
    handle_rf_frame(&f);
}

/* A key frame as remote_rf.c rf_send_button_event() builds it */
static void key(uint8_t slot, uint8_t counter, uint16_t code, uint8_t state) {
    uint8_t p[25] = {0};
    p[0] = (uint8_t)(code >> 8);
    p[1] = (uint8_t)(code & 0xFF);
    p[2] = state;
    strncpy((char *)&p[3], rf_short_button_name(code), 16);
    p[19] = counter;
    frame_from(slot, RF_MSG_BUTTON_PRESS, p, sizeof(p));
}

static void version_report(uint8_t slot) {
    const uint8_t ver[5] = {2, 4, 2, 7, 0x07};
    frame_from(slot, RF_MSG_VERSION_REPORT, ver, sizeof(ver));
}

/* The key counters of a slot as GET /api/rf/status gives them (slots[].link) */
typedef struct {
    int present; /* all five fields are in the link object */
    uint32_t key_events, key_events_lost, key_lost_press, key_lost_release, key_lost_other;
} key_stats_t;

static uint32_t link_num(const cJSON *link, const char *name, int *present) {
    const cJSON *v = cJSON_GetObjectItem(link, name);
    if (!cJSON_IsNumber(v)) { *present = 0; return 0; }
    return (uint32_t)v->valuedouble;
}

static key_stats_t snap(uint8_t slot) {
    key_stats_t k;
    memset(&k, 0, sizeof(k));
    char body[3584];
    cJSON *j = rf_build_status_json(body, sizeof(body), 0) > 0 ? cJSON_Parse(body) : NULL;
    const cJSON *link = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(j, "slots"), slot - 1), "link");
    if (cJSON_IsObject(link)) {
        k.present = 1;
        k.key_events = link_num(link, "key_events", &k.present);
        k.key_events_lost = link_num(link, "key_events_lost", &k.present);
        k.key_lost_press = link_num(link, "key_lost_press", &k.present);
        k.key_lost_release = link_num(link, "key_lost_release", &k.present);
        k.key_lost_other = link_num(link, "key_lost_other", &k.present);
    }
    cJSON_Delete(j);
    return k;
}

/* hub_rf's log so far */
static char s_log[65536];
static const char *read_log(const char *path) {
    fflush(stdout);
    FILE *lf = fopen(path, "r");
    size_t n = lf ? fread(s_log, 1, sizeof(s_log) - 1, lf) : 0;
    if (lf) fclose(lf);
    s_log[n] = '\0';
    return s_log;
}

static int count_lines_with(const char *log, const char *needle) {
    int n = 0;
    for (const char *p = strstr(log, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

int main(void) {
    /* hub_rf logs to stdout: keep it in a file to read the lines back */
    char log_path[] = "/tmp/rfkeycntXXXXXX";
    int log_fd = mkstemp(log_path);
    s_out = fdopen(dup(1), "w");
    if (log_fd < 0 || !s_out) return 1;
    fflush(stdout);
    dup2(log_fd, 1);
    close(log_fd);
    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(s_out, "[*] test_rf_key_counter\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    g_rf_fd = sv[0]; /* the replies go to the other end, which nobody reads */
    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        g_paired_remotes[i].slot = (uint8_t)(i + 1);
        g_paired_remotes[i].active = true;
    }
    g_has_paired_device = true;

    /* Both remotes have reported their version, as after a start of remote_ui */
    version_report(1);
    version_report(2);

    /* The status push to hub_daemon compares this form: a key must not change it */
    char stable_before[3584], stable_after[3584], full[3584];
    rf_build_status_json(stable_before, sizeof(stable_before), RF_STATUS_STABLE_ONLY);

    /* 1. A remote without the counter (0): nothing is counted, nothing is logged */
    key(1, 0, K_RIGHT, PRESS);
    key(1, 0, K_RIGHT, RELEASE);
    key(1, 0, K_LEFT, RELEASE);
    key_stats_t a = snap(1);
    CHECK(a.present, "the RF status has the key counters in slots[].link");
    CHECK(a.key_events == 0 && a.key_events_lost == 0 && a.key_lost_press == 0 && a.key_lost_release == 0 &&
          a.key_lost_other == 0, "counters 0,0,0: nothing counted (events %u, lost %u)", a.key_events, a.key_events_lost);
    CHECK(strstr(read_log(log_path), "RF key event") == NULL, "counters 0,0,0: nothing logged");

    /* 2. The first numbered frame only sets the reference; then two frames are missing */
    key(1, 54, K_RIGHT, PRESS);
    a = snap(1);
    CHECK(a.key_events == 1 && a.key_events_lost == 0, "the first numbered frame (#54) is the reference (events %u, lost %u)",
          a.key_events, a.key_events_lost);
    key(1, 57, K_RIGHT, RELEASE);
    a = snap(1);
    CHECK(a.key_events == 2 && a.key_events_lost == 2 && a.key_lost_other == 2 && a.key_lost_press == 0 &&
          a.key_lost_release == 0, "#54 then #57: 2 lost, holds or unknown (lost %u, other %u)", a.key_events_lost, a.key_lost_other);
    CHECK(strstr(read_log(log_path), "[!] RF key events lost: slot 1, 2 missing before #57 (release DirectionRight)\n") != NULL,
          "one log line for the gap, with the frame that showed it");

    /* 3. A restart: after a version report the counter begins at 1 again, nothing is lost.
     *    Then 1,2,3,5: one lost */
    version_report(1);
    key(1, 1, K_VOLUP, PRESS);
    key_stats_t b = snap(1);
    CHECK(b.key_events_lost == a.key_events_lost, "#57, a version report, then #1: none lost (lost %u)", b.key_events_lost);
    key(1, 2, K_VOLUP, HOLD);
    key(1, 3, K_VOLUP, HOLD);
    key(1, 5, K_VOLUP, HOLD);
    a = snap(1);
    CHECK(a.key_events_lost == b.key_events_lost + 1 && a.key_lost_other == b.key_lost_other + 1,
          "counters 1,2,3,5: one lost, a hold (lost +%u)", a.key_events_lost - b.key_events_lost);
    CHECK(a.key_events == b.key_events + 3, "every numbered frame is counted in key_events");
    key(1, 6, K_VOLUP, RELEASE);

    /* 4. A release for a key whose press was not seen: lost_press */
    b = snap(1);
    key(1, 7, K_RIGHT, PRESS);
    key(1, 8, K_RIGHT, RELEASE);
    /* #9, the press of Left, is lost */
    key(1, 10, K_LEFT, RELEASE);
    a = snap(1);
    CHECK(a.key_events_lost == b.key_events_lost + 1 && a.key_lost_press == b.key_lost_press + 1 &&
          a.key_lost_release == b.key_lost_release && a.key_lost_other == b.key_lost_other,
          "a release without its press: lost_press (press +%u, release +%u, other +%u)", a.key_lost_press - b.key_lost_press,
          a.key_lost_release - b.key_lost_release, a.key_lost_other - b.key_lost_other);

    /* 5. A press while the key before never got its release: lost_release */
    b = a;
    key(1, 11, K_RIGHT, PRESS);
    /* #12, the release of Right, is lost */
    key(1, 13, K_LEFT, PRESS);
    a = snap(1);
    CHECK(a.key_events_lost == b.key_events_lost + 1 && a.key_lost_release == b.key_lost_release + 1 &&
          a.key_lost_press == b.key_lost_press && a.key_lost_other == b.key_lost_other,
          "a press after an unreleased press: lost_release (press +%u, release +%u, other +%u)",
          a.key_lost_press - b.key_lost_press, a.key_lost_release - b.key_lost_release, a.key_lost_other - b.key_lost_other);
    key(1, 14, K_LEFT, RELEASE);

    /* 6. Both in one gap: the release of Right and the press of Left are lost */
    b = snap(1);
    key(1, 15, K_RIGHT, PRESS);
    key(1, 18, K_LEFT, RELEASE);
    a = snap(1);
    CHECK(a.key_events_lost == b.key_events_lost + 2 && a.key_lost_release == b.key_lost_release + 1 &&
          a.key_lost_press == b.key_lost_press + 1 && a.key_lost_other == b.key_lost_other,
          "a lost release and a lost press in one gap (press +%u, release +%u, other +%u)",
          a.key_lost_press - b.key_lost_press, a.key_lost_release - b.key_lost_release, a.key_lost_other - b.key_lost_other);
    CHECK(a.key_lost_press + a.key_lost_release + a.key_lost_other == a.key_events_lost,
          "the three kinds add up to key_events_lost (%u)", a.key_events_lost);

    /* 7. Two keys down at once with no gap is not a loss */
    b = a;
    key(1, 19, K_RIGHT, PRESS);
    key(1, 20, K_LEFT, PRESS);
    key(1, 21, K_RIGHT, RELEASE);
    key(1, 22, K_LEFT, RELEASE);
    a = snap(1);
    CHECK(a.key_events_lost == b.key_events_lost && a.key_lost_press == b.key_lost_press &&
          a.key_lost_release == b.key_lost_release, "consecutive numbers are never a loss, whatever the keys");

    /* 8. The same frame twice is not 254 lost ones */
    b = a;
    key(1, 22, K_LEFT, RELEASE);
    a = snap(1);
    CHECK(a.key_events_lost == b.key_events_lost, "the same number twice: none lost (lost +%u)", a.key_events_lost - b.key_events_lost);
    CHECK(strstr(read_log(log_path), "[!] RF key event received twice: slot 1, #22 (release DirectionLeft)\n") != NULL,
          "a frame received twice is logged");
    /* also right after a version report: the same number is not a restart with 21 frames lost */
    version_report(1);
    key(1, 22, K_LEFT, RELEASE);
    a = snap(1);
    CHECK(a.key_events_lost == b.key_events_lost, "the same number twice after a version report: none lost (lost +%u)",
          a.key_events_lost - b.key_events_lost);

    /* 9. Slot 2 has its own count. 254,255,1,2: the counter wraps to 1, none lost */
    key(2, 254, K_VOLUP, PRESS);
    key(2, 255, K_VOLUP, HOLD);
    key(2, 1, K_VOLUP, HOLD);
    key(2, 2, K_VOLUP, RELEASE);
    key_stats_t s2 = snap(2);
    CHECK(s2.key_events == 4 && s2.key_events_lost == 0, "counters 254,255,1,2: none lost (events %u, lost %u)",
          s2.key_events, s2.key_events_lost);
    CHECK(snap(1).key_events == a.key_events, "slot 2 frames do not count for slot 1");
    key(2, 3, K_VOLUP, PRESS);
    key(2, 4, K_VOLUP, RELEASE);
    key(2, 5, K_VOLUP, PRESS);
    version_report(2);
    key(2, 1, K_VOLUP, RELEASE);
    s2 = snap(2);
    CHECK(s2.key_events_lost == 0 && s2.key_lost_press == 0, "counter 5, a version report, then 1: none lost (lost %u)", s2.key_events_lost);
    /* a restart whose first two frames are lost: 2 missing, not a gap around the ring */
    for (unsigned c = 2; c <= 9; c++) key(2, (uint8_t)c, K_VOLUP, (c & 1) ? RELEASE : PRESS);
    CHECK(snap(2).key_events_lost == 0, "counters 1..9: none lost");
    version_report(2);
    key(2, 3, K_VOLUP, PRESS);
    s2 = snap(2);
    CHECK(s2.key_events_lost == 2, "#9, a version report, then #3: frames 1 and 2 lost (lost %u)", s2.key_events_lost);
    /* a version report without a restart (sent every 24 h) changes nothing */
    version_report(2);
    key(2, 4, K_VOLUP, RELEASE);
    key(2, 6, K_VOLUP, RELEASE);
    s2 = snap(2);
    CHECK(s2.key_events_lost == 3, "a version report with the count going on: gaps counted as before (lost %u)", s2.key_events_lost);
    /* over the wrap with a gap: 254, then 2 (255 and 1 missing) */
    for (unsigned c = 7; c <= 254; c++) key(2, (uint8_t)c, K_VOLUP, HOLD);
    key(2, 2, K_VOLUP, HOLD);
    s2 = snap(2);
    CHECK(s2.key_events_lost == 5, "counters 254 then 2: 255 and 1 lost (lost %u)", s2.key_events_lost);
    /* a remote that restarts after its first key frame sends #1 again: a restart, not a repeat */
    version_report(2);
    key(2, 1, K_RIGHT, PRESS);
    version_report(2);
    key(2, 1, K_RIGHT, PRESS);
    s2 = snap(2);
    CHECK(s2.key_events_lost == 5 && strstr(read_log(log_path), "received twice: slot 2") == NULL,
          "#1, a version report, #1 again: a restart, nothing lost (lost %u)", s2.key_events_lost);

    /* 10. One log line per gap */
    a = snap(1);
    const char *log = read_log(log_path);
    CHECK(count_lines_with(log, "[!] RF key events lost: slot 1,") == 5, "slot 1: one log line per gap (%d)",
          count_lines_with(log, "[!] RF key events lost: slot 1,"));
    CHECK(count_lines_with(log, "[!] RF key events lost: slot 2,") == 3, "slot 2: one log line per gap (%d)",
          count_lines_with(log, "[!] RF key events lost: slot 2,"));
    CHECK(count_lines_with(log, "[*] BUTTON_PRESS: slot=1 ") == 24, "every key frame of slot 1 is still handled (%d)",
          count_lines_with(log, "[*] BUTTON_PRESS: slot=1 "));

    /* 11. The RF status carries the counters next to the other link counters; the form the
     *     status push compares does not */
    int n = rf_build_status_json(full, sizeof(full), 0);
    CHECK(n > 0 && n == (int)strlen(full), "RF status built (%d bytes)", n);
    cJSON *j = n > 0 ? cJSON_Parse(full) : NULL;
    CHECK(j != NULL, "RF status is valid JSON");
    for (int i = 0; i < 2; i++) {
        key_stats_t ks = snap((uint8_t)(i + 1));
        const cJSON *link = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(j, "slots"), i), "link");
        CHECK(ks.present, "slot %d: link has key_events %u, key_events_lost %u, key_lost_press %u, key_lost_release %u, key_lost_other %u",
              i + 1, ks.key_events, ks.key_events_lost, ks.key_lost_press, ks.key_lost_release, ks.key_lost_other);
        CHECK(cJSON_IsNumber(cJSON_GetObjectItem(link, "frames_rx")) && cJSON_IsNumber(cJSON_GetObjectItem(link, "naks")) &&
              cJSON_IsNumber(cJSON_GetObjectItem(link, "last_seen_s")), "slot %d: the other link counters are still there", i + 1);
    }
    cJSON_Delete(j);
    a = snap(1);
    CHECK(a.key_events == 21 && a.key_events_lost == 7 && a.key_lost_press == 2 && a.key_lost_release == 2 && a.key_lost_other == 3,
          "slot 1 totals: 21 numbered frames, 7 lost = 2 press + 2 release + 3 other");
    s2 = snap(2);
    CHECK(s2.key_events == 270 && s2.key_events_lost == 5 && s2.key_lost_press == 1 && s2.key_lost_release == 0 && s2.key_lost_other == 4,
          "slot 2 totals: 270 numbered frames, 5 lost = 1 press + 0 release + 4 other");
    rf_build_status_json(stable_after, sizeof(stable_after), RF_STATUS_STABLE_ONLY);
    CHECK(strcmp(stable_before, stable_after) == 0, "key frames and lost ones are not a status change: no push per key");

    unlink(log_path);
    if (s_failures) {
        fprintf(s_out, "[-] test_rf_key_counter: %d failure(s)\n", s_failures);
        return 1;
    }
    fprintf(s_out, "[+] test_rf_key_counter: lost key frames are counted from the key event counter\n");
    return 0;
}
