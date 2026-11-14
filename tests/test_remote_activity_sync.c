/*
 * test_remote_activity_sync.c - The remote's ACTIVITY_SYNC decoder must read the activity name
 * only from its field: the Hub writes at most 21 name bytes after a short id and 14 after a long
 * one, and puts the query-reply flag in payload[24]. Each frame is placed at the very end of a
 * heap block, so a read past the frame is an AddressSanitizer error (run_mock_suite.sh builds
 * this test with -fsanitize=address).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remote/remote_state.h"
#include "remote/remote_rf_sync.h"
#include "rf_proto.h"

static int s_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } \
} while (0)

/* Builds the frame the way the Hub's rf_send_hub_activity_sync_slot() does, at the end of a
 * heap block of exactly sizeof(rf_frame_t) bytes */
static void send_sync(const char *act_id, const char *act_name, uint8_t query_reply) {
    rf_frame_t *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));
    f->report_id = 0x20;
    f->dev_slot = 1;
    f->sub_id = 0x12;
    f->hdr_flag = 0x40;
    f->msg_type = RF_MSG_ACTIVITY_SYNC;
    if (strlen(act_id) <= 2 || strcmp(act_id, "-1") == 0) {
        strncpy((char *)&f->payload[0], act_id, 3);
        strncpy((char *)&f->payload[3], act_name, 21);
    } else {
        strncpy((char *)&f->payload[0], act_id, 10);
        strncpy((char *)&f->payload[10], act_name, 14);
    }
    f->payload[24] = query_reply;
    g_initial_sync_received = false; /* every frame takes the catch-up path that stores the name */
    handle_hub_frame_remote(f);
    free(f);
}

int main(void) {
    /* Long id, name that fills its 14-byte field, query reply: the flag byte is not the name */
    send_sync("1001", "Watch Apple TV Now", 1);
    CHECK(strcmp(g_active_activity_id, "1001") == 0, "long id: id \"%s\"", g_active_activity_id);
    CHECK(strcmp(g_active_activity_name, "Watch Apple TV") == 0, "long id: name \"%s\" (len %zu), expected \"Watch Apple TV\"",
          g_active_activity_name, strlen(g_active_activity_name));

    /* Short id, name that fills its 21-byte field, query reply */
    send_sync("5", "Living Room Movie Night", 1);
    CHECK(strcmp(g_active_activity_id, "5") == 0, "short id: id \"%s\"", g_active_activity_id);
    CHECK(strcmp(g_active_activity_name, "Living Room Movie Nig") == 0, "short id: name \"%s\" (len %zu), expected \"Living Room Movie Nig\"",
          g_active_activity_name, strlen(g_active_activity_name));

    /* Long id with an empty name (the Hub found no name for the id), live change */
    send_sync("1002", "", 0);
    CHECK(strcmp(g_active_activity_name, "") == 0, "empty name: name \"%s\"", g_active_activity_name);

    if (s_failures) {
        printf("[-] test_remote_activity_sync: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("[+] test_remote_activity_sync: activity names decoded within their field\n");
    return 0;
}
