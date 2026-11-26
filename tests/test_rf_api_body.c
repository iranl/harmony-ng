/*
 * hub_rf's local API reads a request into a 4 KiB buffer. A body that did not fit, or that
 * stopped before its Content-Length, was handled as it was (POST /api/rf/remote-direct-buttons
 * then saved it cut). rf_api_body_status() tells the server to answer 413 or 400 instead.
 *
 * Build: gcc -Wall -O2 -Ipayload/source tests/test_rf_api_body.c payload/source/rf_state.c \
 *            payload/source/rf_api.c payload/source/cJSON.c -lpthread -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "rf_state.h"
#include "rf_api.h"
#include "cJSON.h"

#include "rf_api_stubs.h"

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s\n", msg); } else printf("  PASS %s\n", msg); } while (0)

static int status_of(const char *req, size_t cap) {
    return rf_api_body_status(req, strlen(req), cap);
}

int main(void) {
    printf("[-] hub_rf API request bodies\n");
    const char *whole = "POST /api/rf/remote-direct-buttons?slot=1 HTTP/1.1\r\nContent-Length: 7\r\n\r\n{\"a\":1}";
    CHECK(status_of(whole, 4095) == 0, "whole body accepted");
    const char *cut = "POST /api/rf/remote-direct-buttons?slot=1 HTTP/1.1\r\nContent-Length: 70\r\n\r\n{\"a\":1";
    CHECK(status_of(cut, 4095) == 400, "body that stopped early: 400");
    const char *big = "POST /api/rf/remote-direct-buttons?slot=1 HTTP/1.1\r\nContent-Length: 9000\r\n\r\n{\"a\":";
    CHECK(status_of(big, 4095) == 413, "body larger than the buffer: 413");
    CHECK(status_of("GET /api/rf/status HTTP/1.1\r\nHost: x\r\n\r\n", 4095) == 0, "no body: accepted");
    CHECK(status_of("GET /api/rf/status HTTP/1.1\r\nHost: x\r\n", 4095) == 0, "headers not finished: left to the routes");
    const char *neg = "POST /api/rf/remote-direct-buttons?slot=1 HTTP/1.1\r\nContent-Length: -5\r\n\r\n{\"a\":1}";
    CHECK(status_of(neg, 4095) == 400, "negative Content-Length: 400");
    const char *word = "POST /api/rf/remote-direct-buttons?slot=1 HTTP/1.1\r\nContent-Length: abc\r\n\r\n{\"a\":1}";
    CHECK(status_of(word, 4095) == 400, "non-numeric Content-Length: 400");
    const char *tail = "POST /api/rf/remote-direct-buttons?slot=1 HTTP/1.1\r\nContent-Length: 7x\r\n\r\n{\"a\":1}";
    CHECK(status_of(tail, 4095) == 400, "Content-Length with trailing junk: 400");
    const char *zero = "POST /api/rf/remote-direct-buttons?slot=1 HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    CHECK(status_of(zero, 4095) == 0, "Content-Length 0: accepted");

    /* POST /api/rf/layer: one slot, "on" 0 or 1, and a name the keymap takes; else 400 and
     * nothing set */
#define LAYER(q) rf_api_layer_post("POST /api/rf/layer" q " HTTP/1.1\r\nHost: x\r\n\r\n")
    CHECK(LAYER("?slot=2&on=1&name=amp") == 0 && g_stub_layer_slot == 2 && g_stub_layer_on == 1 &&
          strcmp(g_stub_layer_name, "amp") == 0, "layer: slot, on and name reach the keymap");
    CHECK(LAYER("?slot=1&on=0") == 0 && g_stub_layer_slot == 1 && g_stub_layer_on == 0 &&
          g_stub_layer_name[0] == '\0', "layer: off, no name");
    int calls = g_stub_layer_calls;
    CHECK(LAYER("?on=1") == 400, "layer: no slot: 400");
    CHECK(LAYER("?slot=1") == 400, "layer: no on: 400");
    CHECK(LAYER("") == 400, "layer: no query: 400");
    CHECK(LAYER("?slot=257&on=1") == 400, "layer: slot 257 is not slot 1: 400");
    CHECK(LAYER("?slot=1x&on=1") == 400, "layer: slot 1x: 400");
    CHECK(LAYER("?slot=0&on=1") == 400 && LAYER("?slot=all&on=1") == 400, "layer: every slot at once: 400");
    CHECK(LAYER("?slot=7&on=1") == 400, "layer: slot past the last: 400");
    CHECK(LAYER("?slot=1&on=2") == 400 && LAYER("?slot=1&on=yes") == 400 && LAYER("?slot=1&on=") == 400,
          "layer: on other than 0 or 1: 400");
    CHECK(LAYER("?slot=1&on=1&name=a-name-longer-than-any-layer-name-can-be") == 400, "layer: a name too long: 400");
    CHECK(g_stub_layer_calls == calls, "layer: none of those reached the keymap");
    CHECK(LAYER("?slot=1&on=1&name=refused") == 400, "layer: a name the keymap refuses: 400");

    /* The answer of GET and POST /api/rf/layer is JSON whatever the activity id read from the
     * file holds: a quote, a backslash and a line feed come back as they were */
    const char *odd_id = "1\"\\x\n2";
    snprintf(g_stub_layer_act2, sizeof(g_stub_layer_act2), "%s", odd_id);
    char *layer_body = rf_api_layer_json(1);
    cJSON *lj = layer_body ? cJSON_Parse(layer_body) : NULL;
    cJSON *lslots = lj ? cJSON_GetObjectItem(lj, "slots") : NULL;
    cJSON *lslot2 = lslots ? cJSON_GetArrayItem(lslots, 1) : NULL;
    cJSON *lact = lslot2 ? cJSON_GetObjectItem(lslot2, "activity") : NULL;
    CHECK(lj != NULL, "layer: the answer parses as JSON with a quote and a backslash in the activity id");
    CHECK(lact && cJSON_IsString(lact) && strcmp(lact->valuestring, odd_id) == 0 &&
          cJSON_IsTrue(cJSON_GetObjectItem(lslot2, "on")), "layer: slot 2's activity id comes back whole, on");
    CHECK(lslots && cJSON_GetArraySize(lslots) == MAX_PAIRED_REMOTES && cJSON_IsTrue(cJSON_GetObjectItem(lj, "ok")) &&
          cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(lslots, 0), "on")),
          "layer: ok, and an entry for every slot, slot 1 off");
    cJSON_Delete(lj);
    free(layer_body);
    layer_body = rf_api_layer_json(0);
    CHECK(layer_body && strstr(layer_body, "\"ok\":false") != NULL, "layer: a refused POST answers ok false");
    free(layer_body);
    g_stub_layer_act2[0] = '\0';

    if (g_failures) return 1;
    printf("[+] hub_rf API request bodies: all checks passed\n");
    return 0;
}
