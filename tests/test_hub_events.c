/*
 * Unit test of the hub_daemon event bus (payload/source/hub_events.c):
 * two sinks in subscription order, emit from a second thread, dispatch on the first,
 * seq / ts / type names, the last payload per type, and the overflow counter.
 *
 * Build: gcc -Wall -Wextra -O2 -Ipayload/source tests/test_hub_events.c \
 *            payload/source/hub_events.c payload/source/cJSON.c -lpthread -lm
 */
#include "hub_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        g_failures++; \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

/* Every call of every sink, in order */
typedef struct {
    int sink;            /* 1 or 2 */
    hub_event_type_t type;
    uint32_t seq;
    time_t ts;
    char json[256];
} call_t;

static call_t g_calls[256];
static int g_ncalls;

static void record(int sink, const hub_event_t *ev) {
    if (g_ncalls >= (int)(sizeof(g_calls) / sizeof(g_calls[0]))) return;
    call_t *c = &g_calls[g_ncalls++];
    c->sink = sink;
    c->type = ev->type;
    c->seq = ev->seq;
    c->ts = ev->ts;
    char *txt = cJSON_PrintUnformatted(ev->payload);
    snprintf(c->json, sizeof(c->json), "%s", txt ? txt : "");
    free(txt);
}

static void sink_one(const hub_event_t *ev, void *user) {
    CHECK(user == (void *)0x1, "sink 1 user pointer");
    record(1, ev);
}

static void sink_two(const hub_event_t *ev, void *user) {
    CHECK(user == (void *)0x2, "sink 2 user pointer");
    record(2, ev);
}

static void *emitter_thread(void *arg) {
    int n = (int)(intptr_t)arg;
    for (int i = 0; i < n; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "i", i);
        /* An emitter-side "type" must not survive: the bus owns that key */
        cJSON_AddStringToObject(p, "type", "bogus");
        hub_events_emit(HUB_EV_BUTTON, p);
    }
    return NULL;
}

static void test_names(void) {
    printf("[-] type names\n");
    CHECK(strcmp(hub_event_type_name(HUB_EV_ACTIVITY_STATE), "activity_state") == 0, "activity_state");
    CHECK(strcmp(hub_event_type_name(HUB_EV_ACTIVITY_PROGRESS), "activity_progress") == 0, "activity_progress");
    CHECK(strcmp(hub_event_type_name(HUB_EV_CONFIG_CHANGED), "config_changed") == 0, "config_changed");
    CHECK(strcmp(hub_event_type_name(HUB_EV_BT_STATUS), "bt_status") == 0, "bt_status");
    CHECK(strcmp(hub_event_type_name(HUB_EV_RF_STATUS), "rf_status") == 0, "rf_status");
    CHECK(strcmp(hub_event_type_name(HUB_EV_BUTTON), "button") == 0, "button");
    CHECK(strcmp(hub_event_type_name(HUB_EV_COUNT), "unknown") == 0, "out of range name");
}

static void test_order_seq_ts(void) {
    printf("[-] order, seq, ts, last payload\n");
    hub_events_init();
    CHECK(hub_events_subscribe(sink_one, (void *)0x1) == 0, "subscribe 1");
    CHECK(hub_events_subscribe(sink_two, (void *)0x2) == 0, "subscribe 2");
    g_ncalls = 0;

    /* Nothing queued: dispatch is a no-op */
    hub_events_dispatch();
    CHECK(g_ncalls == 0, "no calls without events");
    CHECK(hub_events_last_seq() == 0, "seq starts at 0 before the first event");
    CHECK(hub_events_last(HUB_EV_ACTIVITY_STATE) == NULL, "no last payload yet");

    time_t before = time(NULL);
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "activity", "-1");
    hub_events_emit(HUB_EV_ACTIVITY_STATE, a);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "files", "devices");
    hub_events_emit(HUB_EV_CONFIG_CHANGED, b);
    hub_events_emit(HUB_EV_BT_STATUS, NULL); /* NULL payload becomes an empty object */
    CHECK(hub_events_last_seq() == 0, "seq is assigned at dispatch, not at emit");

    hub_events_dispatch();
    time_t after = time(NULL);

    CHECK(g_ncalls == 6, "3 events x 2 sinks = 6 calls, got %d", g_ncalls);
    if (g_ncalls == 6) {
        /* Per event: sink 1 then sink 2, same seq and payload */
        for (int e = 0; e < 3; e++) {
            call_t *c1 = &g_calls[e * 2], *c2 = &g_calls[e * 2 + 1];
            CHECK(c1->sink == 1 && c2->sink == 2, "event %d: sinks in subscription order", e);
            CHECK(c1->seq == (uint32_t)(e + 1), "event %d: seq %u", e, (unsigned)c1->seq);
            CHECK(c2->seq == c1->seq, "event %d: both sinks see the same seq", e);
            CHECK(strcmp(c1->json, c2->json) == 0, "event %d: both sinks see the same payload", e);
            CHECK(c1->ts >= before && c1->ts <= after, "event %d: ts within the test", e);
        }
        CHECK(g_calls[0].type == HUB_EV_ACTIVITY_STATE, "event 0 type");
        CHECK(g_calls[2].type == HUB_EV_CONFIG_CHANGED, "event 1 type");
        CHECK(g_calls[4].type == HUB_EV_BT_STATUS, "event 2 type");
        /* "type" first, then seq and ts, then the emitter's fields */
        char expect[128];
        snprintf(expect, sizeof(expect), "{\"type\":\"activity_state\",\"seq\":1,\"ts\":%ld,\"activity\":\"-1\"}",
                 (long)g_calls[0].ts);
        CHECK(strcmp(g_calls[0].json, expect) == 0, "payload layout: %s", g_calls[0].json);
        snprintf(expect, sizeof(expect), "{\"type\":\"bt_status\",\"seq\":3,\"ts\":%ld}", (long)g_calls[4].ts);
        CHECK(strcmp(g_calls[4].json, expect) == 0, "empty payload layout: %s", g_calls[4].json);
    }
    CHECK(hub_events_last_seq() == 3, "last seq 3, got %u", (unsigned)hub_events_last_seq());

    const cJSON *last = hub_events_last(HUB_EV_ACTIVITY_STATE);
    CHECK(last != NULL, "last activity_state kept");
    if (last) {
        const cJSON *act = cJSON_GetObjectItemCaseSensitive(last, "activity");
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(last, "seq");
        CHECK(cJSON_IsString(act) && strcmp(act->valuestring, "-1") == 0, "last payload content");
        CHECK(cJSON_IsNumber(seq) && seq->valueint == 1, "last payload seq");
    }
    CHECK(hub_events_last(HUB_EV_BUTTON) == NULL, "no button event yet");

    /* A newer event of the same type replaces the last one */
    cJSON *a2 = cJSON_CreateObject();
    cJSON_AddStringToObject(a2, "activity", "12");
    hub_events_emit(HUB_EV_ACTIVITY_STATE, a2);
    hub_events_dispatch();
    last = hub_events_last(HUB_EV_ACTIVITY_STATE);
    const cJSON *act2 = last ? cJSON_GetObjectItemCaseSensitive(last, "activity") : NULL;
    CHECK(act2 && cJSON_IsString(act2) && strcmp(act2->valuestring, "12") == 0, "last payload replaced");
    CHECK(hub_events_last_seq() == 4, "seq 4 after the fourth event");
}

static void test_subscribe_limit(void) {
    printf("[-] sink limit\n");
    hub_events_init();
    for (int i = 0; i < HUB_EVENTS_MAX_SINKS; i++) {
        CHECK(hub_events_subscribe(sink_one, (void *)0x1) == 0, "subscribe %d", i);
    }
    CHECK(hub_events_subscribe(sink_one, (void *)0x1) == -1, "fifth sink refused");
    CHECK(hub_events_subscribe(NULL, NULL) == -1, "NULL sink refused");
}

static void test_thread_emit(void) {
    printf("[-] emit from a second thread\n");
    hub_events_init();
    hub_events_subscribe(sink_one, (void *)0x1);
    g_ncalls = 0;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, emitter_thread, (void *)(intptr_t)20) == 0, "thread");
    pthread_join(th, NULL);
    hub_events_dispatch();
    CHECK(g_ncalls == 20, "20 events dispatched, got %d", g_ncalls);
    int in_order = 1;
    for (int i = 0; i < g_ncalls; i++) {
        char expect[64];
        snprintf(expect, sizeof(expect), "\"i\":%d}", i);
        if (!strstr(g_calls[i].json, expect)) in_order = 0;
        if (strstr(g_calls[i].json, "bogus")) in_order = 0;
        if (g_calls[i].seq != (uint32_t)(i + 1)) in_order = 0;
    }
    CHECK(in_order, "events in emit order with consecutive seq and the bus's own type");
    CHECK(hub_events_overflows() == 0, "no overflow");
}

static void test_overflow(void) {
    printf("[-] overflow\n");
    hub_events_init();
    hub_events_subscribe(sink_one, (void *)0x1);
    g_ncalls = 0;
    pthread_t th;
    pthread_create(&th, NULL, emitter_thread, (void *)(intptr_t)(HUB_EVENTS_RING + 5));
    pthread_join(th, NULL);
    CHECK(hub_events_overflows() == 5, "5 dropped, got %u", (unsigned)hub_events_overflows());
    hub_events_dispatch();
    CHECK(g_ncalls == HUB_EVENTS_RING, "%d dispatched, got %d", HUB_EVENTS_RING, g_ncalls);
    /* The oldest were dropped: the first dispatched event is i=5, the last i=36 */
    CHECK(strstr(g_calls[0].json, "\"i\":5}") != NULL, "first kept event is i=5: %s", g_calls[0].json);
    CHECK(strstr(g_calls[g_ncalls - 1].json, "\"i\":36}") != NULL, "last event is i=36");
    /* seq numbers only count dispatched events */
    CHECK(g_calls[0].seq == 1 && g_calls[g_ncalls - 1].seq == (uint32_t)HUB_EVENTS_RING, "seq 1..32");
    /* init clears the counter and the queue */
    hub_events_init();
    CHECK(hub_events_overflows() == 0 && hub_events_last_seq() == 0, "init resets");
}

int main(void) {
    printf("hub_events test\n");
    test_names();
    test_order_seq_ts();
    test_subscribe_limit();
    test_thread_emit();
    test_overflow();
    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All hub_events checks passed\n");
    return 0;
}
