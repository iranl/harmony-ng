/*
 * Values in the configuration files:
 *  - ids given as JSON numbers out of range are bad ids, not undefined behaviour (built with
 *    -fsanitize=float-cast-overflow -fno-sanitize-recover=all, so an out-of-range cast ends it)
 *  - a device that is Bluetooth only by "transport": 32 stays Bluetooth over two passes
 *  - powerOnDelay, Delay steps, command port and repeats are kept in range
 *  - an unknown Area/City timezone is reported as unknown (it ran as Central European time)
 *
 * Build: gcc -Wall -O2 -fsanitize=float-cast-overflow -fno-sanitize-recover=all -Ipayload/source \
 *            tests/test_config_values.c payload/source/config_sanitize.c payload/source/timezone_db.c \
 *            payload/source/cJSON.c -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config_sanitize.h"
#include "timezone_db.h"
#include "json_number.h"

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s\n", msg); } else printf("  PASS %s\n", msg); } while (0)

static cJSON *dev_of(cJSON *root, int i) {
    return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "devices"), i);
}

static int num_of(const cJSON *o, const char *k) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(j) ? j->valueint : -12345;
}

int main(void) {
    printf("[-] ids out of range\n");
    cJSON *root = cJSON_Parse("{\"devices\":[{\"id\":1e300,\"name\":\"Huge\"},{\"id\":7,\"name\":\"Ok\","
                              "\"powerOnSequence\":[{\"type\":\"IRCommand\",\"deviceId\":1e19,\"command\":\"PowerOn\"}]}]}");
    sanitize_devices_json(root);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "devices")) == 1, "device with id 1e300 left out");
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(dev_of(root, 0), "id"));
    CHECK(id && strcmp(id, "7") == 0, "device 7 kept");
    cJSON_Delete(root);
    char buf[32];
    cJSON *n = cJSON_CreateNumber(3000000000.0);
    CHECK(json_number_text(n, buf, sizeof(buf)) == 0 && buf[0] == '\0', "3e9 is not a 32-bit id");
    cJSON_SetNumberValue(n, -1);
    CHECK(json_number_text(n, buf, sizeof(buf)) == 1 && strcmp(buf, "-1") == 0, "-1 kept");
    cJSON_Delete(n);

    printf("[-] Bluetooth by transport, two passes\n");
    root = cJSON_Parse("{\"devices\":[{\"id\":\"7\",\"name\":\"Shield\",\"transport\":32,\"bdaddr\":\"00:11:22:33:44:55\","
                       "\"btAutoReconnect\":0,\"btReconnectDurationSec\":45,"
                       "\"commands\":[{\"name\":\"Home\",\"keyCode\":\"HID:0x0C:0x223\"}]}]}");
    sanitize_devices_json(root);
    sanitize_devices_json(root);
    cJSON *d = dev_of(root, 0);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(d, "type"));
    CHECK(type && strcmp(type, "BT") == 0, "type is BT");
    CHECK(num_of(d, "btAutoReconnect") == 0 && num_of(d, "btReconnectDurationSec") == 45, "Bluetooth settings kept");
    CHECK(cJSON_GetObjectItemCaseSensitive(d, "controlPort") == NULL, "no IR port added");
    cJSON_Delete(root);

    printf("[-] limits\n");
    root = cJSON_Parse("{\"devices\":[{\"id\":\"1\",\"name\":\"TV\",\"controlPort\":3,\"powerOnDelay\":2000000000,"
                       "\"powerOnSequence\":[{\"type\":\"Delay\",\"delay\":2000000000}],"
                       "\"commands\":[{\"name\":\"A\",\"keyCode\":\"x\",\"port\":99,\"repeats\":-7},"
                       "{\"name\":\"B\",\"keyCode\":\"x\",\"port\":5,\"repeats\":1}]}]}");
    sanitize_devices_json(root);
    d = dev_of(root, 0);
    CHECK(num_of(d, "powerOnDelay") == POWER_ON_DELAY_MAX_MS, "powerOnDelay lowered to its limit");
    cJSON *step = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(d, "powerOnSequence"), 0);
    CHECK(num_of(step, "delay") == STEP_DELAY_MAX_MS, "Delay step lowered to its limit");
    cJSON *cmds = cJSON_GetObjectItemCaseSensitive(d, "commands");
    CHECK(num_of(cJSON_GetArrayItem(cmds, 0), "port") == 3, "port 99 becomes the device port");
    CHECK(num_of(cJSON_GetArrayItem(cmds, 0), "repeats") == 3, "repeats -7 becomes 3");
    CHECK(num_of(cJSON_GetArrayItem(cmds, 1), "port") == 5 && num_of(cJSON_GetArrayItem(cmds, 1), "repeats") == 1,
          "valid port and repeats kept");
    cJSON_Delete(root);

    printf("[-] timezones\n");
    CHECK(timezone_is_known("Europe/Rome"), "Europe/Rome known");
    CHECK(timezone_is_known("CET-1CEST,M3.5.0,M10.5.0/3"), "POSIX string known");
    CHECK(timezone_is_known("UTC"), "UTC known");
    CHECK(timezone_is_known(""), "empty means the default");
    CHECK(!timezone_is_known("America/Argentina/Buenos_Aires"), "unlisted Area/City reported");
    CHECK(timezone_is_known("JST-9") && timezone_is_known("MST7") && timezone_is_known("UTC0") &&
          timezone_is_known("<+0530>-5:30") && timezone_is_known("EST5EDT"), "fixed offset POSIX strings known");
    CHECK(!timezone_is_known("unknown"), "plain word reported");
    CHECK(!timezone_is_known("AB-1"), "zone name shorter than three letters reported");
    CHECK(!timezone_is_known("CET-99"), "offset over 24 hours reported");
    CHECK(!timezone_is_known("x,y,z"), "rules without a zone name and offset reported");
    char out[64];
    timezone_to_posix("America/New_York", out, sizeof(out));
    CHECK(strcmp(out, "EST5EDT,M3.2.0,M11.1.0") == 0, "conversion unchanged");

    if (g_failures) return 1;
    printf("[+] Configuration values: all checks passed\n");
    return 0;
}
