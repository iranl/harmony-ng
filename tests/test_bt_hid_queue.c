/* HID report queue (hub_btstack/bt_hid_queue.c): a report the Bluetooth stack refuses is sent
 * later instead of being lost, reports keep their order, a full queue drops a press and never a
 * release, and a closed link clears its queue. The stack is a fake that refuses on request. */
#include <stdio.h>
#include <string.h>
#include "bt_hid_queue.h"

static int failed;
#define CHECK(c, name) do { if (c) printf("  PASS %s\n", name); else { printf("  FAIL %s\n", name); failed++; } } while (0)

/* Fake stack */
static int s_refuse;             /* refuse this many sends, then take them */
static int s_refuse_forever;
static uint8_t s_sent[64][16];
static uint16_t s_sent_cid[64];
static int s_nsent;
static int s_requests;

static uint8_t fake_send(uint16_t cid, const uint8_t *report, uint16_t len) {
    if (s_refuse_forever) return 0x57; /* BTSTACK_ACL_BUFFERS_FULL */
    if (s_refuse > 0) {
        s_refuse--;
        return 0x57;
    }
    if (s_nsent < 64) {
        memset(s_sent[s_nsent], 0, 16);
        memcpy(s_sent[s_nsent], report, len);
        s_sent_cid[s_nsent] = cid;
        s_nsent++;
    }
    return 0;
}
static void fake_request(uint16_t cid) { (void)cid; s_requests++; }
static void fake_log(const char *line) { printf("    log: %s\n", line); }

static void reset(void) {
    hidq_init(fake_send, fake_request, fake_log);
    s_refuse = 0;
    s_refuse_forever = 0;
    s_nsent = 0;
    s_requests = 0;
}

static const uint8_t PRESS_RIGHT[10] = {0xa1, 0x01, 0, 0, 0x4f, 0, 0, 0, 0, 0};
static const uint8_t PRESS_LEFT[10] = {0xa1, 0x01, 0, 0, 0x50, 0, 0, 0, 0, 0};
static const uint8_t RELEASE[10] = {0xa1, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t CONSUMER_RELEASE[4] = {0xa1, 0x02, 0, 0};

static int sent_is(int i, const uint8_t *r, uint16_t len) {
    return i < s_nsent && memcmp(s_sent[i], r, len) == 0;
}

int main(void) {
    const uint16_t SHIELD = 0x0006, TV = 0x0004;
    printf("[-] HID report queue\n");

    reset();
    hidq_send(SHIELD, PRESS_RIGHT, sizeof(PRESS_RIGHT), 1000);
    hidq_send(SHIELD, RELEASE, sizeof(RELEASE), 1100);
    CHECK(s_nsent == 2 && sent_is(0, PRESS_RIGHT, 10) && sent_is(1, RELEASE, 10) && s_requests == 0 &&
          hidq_pending(SHIELD) == 0 && hidq_waits() == 0,
          "free stack: press and release go out at once, nothing waits");

    /* The case that left keys stuck: the stack refuses the release */
    reset();
    hidq_send(SHIELD, PRESS_RIGHT, sizeof(PRESS_RIGHT), 1000);
    s_refuse = 1;
    hidq_send(SHIELD, RELEASE, sizeof(RELEASE), 1100);
    CHECK(s_nsent == 1 && hidq_pending(SHIELD) == 1 && s_requests == 1,
          "refused release waits and a can-send-now notice is asked for");
    hidq_service(SHIELD, 1105);
    CHECK(s_nsent == 2 && sent_is(1, RELEASE, 10) && hidq_pending(SHIELD) == 0,
          "the release goes out on the notice");

    /* Order: a press refused, more keys behind it */
    reset();
    s_refuse = 1;
    hidq_send(SHIELD, PRESS_RIGHT, sizeof(PRESS_RIGHT), 1000);
    hidq_send(SHIELD, RELEASE, sizeof(RELEASE), 1001);
    hidq_send(SHIELD, PRESS_LEFT, sizeof(PRESS_LEFT), 1002);
    hidq_send(SHIELD, RELEASE, sizeof(RELEASE), 1003);
    CHECK(s_nsent == 4 && sent_is(0, PRESS_RIGHT, 10) && sent_is(1, RELEASE, 10) && sent_is(2, PRESS_LEFT, 10) &&
          sent_is(3, RELEASE, 10),
          "reports keep their order: the next send flushes what waits first");

    /* A notice that never comes: the retry timer path sends it */
    reset();
    s_refuse = 3;
    hidq_send(SHIELD, RELEASE, sizeof(RELEASE), 2000);
    CHECK(hidq_pending_any() && hidq_pending(SHIELD) == 1, "release waits while the stack refuses");
    hidq_service_all(2020);
    hidq_service_all(2040);
    CHECK(hidq_pending(SHIELD) == 1, "still waiting while refused (two retries)");
    hidq_service_all(2060);
    CHECK(s_nsent == 1 && sent_is(0, RELEASE, 10) && !hidq_pending_any(), "sent by a retry without any notice");

    /* Hosts are separate: a busy Shield does not hold the TV's reports */
    reset();
    s_refuse = 1;
    hidq_send(SHIELD, RELEASE, sizeof(RELEASE), 3000);
    hidq_send(TV, CONSUMER_RELEASE, sizeof(CONSUMER_RELEASE), 3001);
    CHECK(s_nsent == 1 && s_sent_cid[0] == TV && hidq_pending(SHIELD) == 1 && hidq_pending(TV) == 0,
          "one host waiting does not hold another host's reports");

    /* Full queue: presses go, releases stay */
    reset();
    s_refuse_forever = 1;
    for (int i = 0; i < HIDQ_DEPTH; i++) {
        hidq_send(SHIELD, (i % 2) ? RELEASE : PRESS_RIGHT, 10, 4000 + i);
    }
    hidq_send(SHIELD, RELEASE, sizeof(RELEASE), 4100);
    CHECK(hidq_pending(SHIELD) == HIDQ_DEPTH && hidq_dropped() == 1, "full queue: one report dropped for the new one");
    s_refuse_forever = 0;
    hidq_service(SHIELD, 4200);
    int releases = 0;
    for (int i = 0; i < s_nsent; i++) releases += hidq_is_release(s_sent[i], 10);
    CHECK(s_nsent == HIDQ_DEPTH && releases == HIDQ_DEPTH / 2 + 1 && sent_is(s_nsent - 1, RELEASE, 10),
          "the dropped report was a press: every release went out, the last report is a release");

    /* Closed link: its queue goes */
    reset();
    s_refuse_forever = 1;
    hidq_send(SHIELD, PRESS_RIGHT, sizeof(PRESS_RIGHT), 5000);
    hidq_drop(SHIELD);
    s_refuse_forever = 0;
    hidq_service_all(5100);
    CHECK(s_nsent == 0 && !hidq_pending_any(), "a closed link's waiting reports are dropped, not sent to a later link");

    CHECK(hidq_is_release(RELEASE, 10) && hidq_is_release(CONSUMER_RELEASE, 4) && !hidq_is_release(PRESS_RIGHT, 10),
          "release reports are recognised");

    if (failed) {
        printf("[-] %d check(s) failed\n", failed);
        return 1;
    }
    printf("[+] HID report queue: all checks passed\n");
    return 0;
}
