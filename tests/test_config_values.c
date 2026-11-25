/*
 * Values in the configuration files:
 *  - ids given as JSON numbers out of range are bad ids, not undefined behaviour (built with
 *    -fsanitize=float-cast-overflow -fno-sanitize-recover=all, so an out-of-range cast ends it)
 *  - a device that is Bluetooth only by "transport": 32 stays Bluetooth over two passes
 *  - powerOnDelay, Delay steps, command port and repeats are kept in range
 *  - pressMinRepeats is kept (0..20, numeric text), dropped when not a count, never added
 *  - an unknown Area/City timezone is reported as unknown (it ran as Central European time)
 *  - the infrared alternative of a plugin command (irKeyCode, irWhen) is kept when valid
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

/* No plugins installed: the sanitizer only asks which device types belong to a plugin */

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
    root = cJSON_Parse("{\"commandPorts\":1,\"devices\":[{\"id\":\"1\",\"name\":\"TV\",\"controlPort\":3,\"powerOnDelay\":2000000000,"
                       "\"powerOnSequence\":[{\"type\":\"Delay\",\"delay\":2000000000}],"
                       "\"commands\":[{\"name\":\"A\",\"keyCode\":\"x\",\"port\":99,\"repeats\":-7},"
                       "{\"name\":\"B\",\"keyCode\":\"x\",\"port\":5,\"repeats\":1}]}]}");
    sanitize_devices_json(root);
    d = dev_of(root, 0);
    CHECK(num_of(d, "powerOnDelay") == POWER_ON_DELAY_MAX_MS, "powerOnDelay lowered to its limit");
    cJSON *step = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(d, "powerOnSequence"), 0);
    CHECK(num_of(step, "delay") == STEP_DELAY_MAX_MS, "Delay step lowered to its limit");
    cJSON *cmds = cJSON_GetObjectItemCaseSensitive(d, "commands");
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 0), "port") == NULL,
          "port 99 removed: the command uses the device port");
    CHECK(num_of(cJSON_GetArrayItem(cmds, 0), "repeats") == 3, "repeats -7 becomes 3");
    CHECK(num_of(cJSON_GetArrayItem(cmds, 1), "port") == 5 && num_of(cJSON_GetArrayItem(cmds, 1), "repeats") == 1,
          "valid port and repeats kept");
    cJSON_Delete(root);

    printf("[-] pressMinRepeats (frames an IR press sends after the first)\n");
    root = cJSON_Parse("{\"devices\":[{\"id\":\"1\",\"name\":\"A\",\"pressMinRepeats\":0},"
                       "{\"id\":\"2\",\"name\":\"B\",\"pressMinRepeats\":\" 2 \"},"
                       "{\"id\":\"3\",\"name\":\"C\",\"pressMinRepeats\":25},"
                       "{\"id\":\"4\",\"name\":\"D\",\"pressMinRepeats\":\"x2\"},"
                       "{\"id\":\"5\",\"name\":\"E\",\"pressMinRepeats\":-1},"
                       "{\"id\":\"6\",\"name\":\"F\",\"pressMinRepeats\":true},"
                       "{\"id\":\"7\",\"name\":\"G\"}]}");
    sanitize_devices_json(root);
    sanitize_devices_json(root);
    CHECK(num_of(dev_of(root, 0), "pressMinRepeats") == 0, "0 kept");
    CHECK(num_of(dev_of(root, 1), "pressMinRepeats") == 2, "numeric text \" 2 \" kept as 2");
    CHECK(num_of(dev_of(root, 2), "pressMinRepeats") == 20, "25 lowered to 20");
    CHECK(!cJSON_GetObjectItemCaseSensitive(dev_of(root, 3), "pressMinRepeats"), "text that is no number dropped");
    CHECK(!cJSON_GetObjectItemCaseSensitive(dev_of(root, 4), "pressMinRepeats"), "negative dropped (not set)");
    CHECK(!cJSON_GetObjectItemCaseSensitive(dev_of(root, 5), "pressMinRepeats"), "true dropped");
    CHECK(!cJSON_GetObjectItemCaseSensitive(dev_of(root, 6), "pressMinRepeats"), "none added to a device without it");
    cJSON_Delete(root);

    printf("[-] command ports copied from the device before commandPorts 1\n");
    root = cJSON_Parse("{\"devices\":[{\"id\":\"1\",\"name\":\"TV\",\"controlPort\":3,"
                       "\"commands\":[{\"name\":\"A\",\"keyCode\":\"x\",\"port\":3}]}]}");
    sanitize_devices_json(root);
    cmds = cJSON_GetObjectItemCaseSensitive(dev_of(root, 0), "commands");
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 0), "port") == NULL, "copied port dropped once");
    CHECK(num_of(root, "commandPorts") == 1, "file marked commandPorts 1");
    cJSON_AddNumberToObject(cJSON_GetArrayItem(cmds, 0), "port", 2);
    sanitize_devices_json(root);
    cmds = cJSON_GetObjectItemCaseSensitive(dev_of(root, 0), "commands");
    CHECK(num_of(cJSON_GetArrayItem(cmds, 0), "port") == 2, "a port set after that is kept");
    cJSON_Delete(root);

    printf("[-] command range\n");
    root = cJSON_Parse("{\"devices\":[{\"id\":\"2\",\"name\":\"Fan\",\"plugin\":\"mantra\",\"commands\":["
                       "{\"name\":\"Speed\",\"keyCode\":\"PLUGIN:mantra:SPEED\",\"range\":[0,31]},"
                       "{\"name\":\"Bad\",\"keyCode\":\"PLUGIN:mantra:X\",\"range\":[9,1]}]}]}");
    sanitize_devices_json(root);
    sanitize_devices_json(root);
    cmds = cJSON_GetObjectItemCaseSensitive(dev_of(root, 0), "commands");
    cJSON *rg = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 0), "range");
    CHECK(cJSON_GetArraySize(rg) == 2 && cJSON_GetArrayItem(rg, 0)->valueint == 0 &&
          cJSON_GetArrayItem(rg, 1)->valueint == 31, "valid range kept over two passes");
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 1), "range") == NULL, "min > max range left out");
    cJSON_Delete(root);

    printf("[-] infrared alternative of a plugin command (irKeyCode, irWhen)\n");
    root = cJSON_Parse("{\"commandPorts\":1,\"devices\":[{\"id\":\"3\",\"name\":\"TV\",\"plugin\":\"samsung\",\"commands\":["
                       "{\"name\":\"HDMI 2\",\"keyCode\":\"PLUGIN:samsung:KEY_HDMI2\","
                       "\"irKeyCode\":\"G:GoVideoO1 32 Bit:()(0xE0E07D82)():3\",\"irWhen\":\"hdmi_via_ir\",\"port\":2},"
                       "{\"name\":\"Bad\",\"keyCode\":\"PLUGIN:samsung:X\",\"irKeyCode\":7,\"irWhen\":\"a b\"},"
                       "{\"name\":\"Long\",\"keyCode\":\"PLUGIN:samsung:Y\",\"irKeyCode\":\"F9470P0123S0456\","
                       "\"irWhen\":\"k123456789012345678901234567890123456789012345678901234567890123\"},"
                       "{\"name\":\"Plain\",\"keyCode\":\"PLUGIN:samsung:KEY_HOME\"}]},"
                       "{\"id\":\"4\",\"name\":\"Amp\",\"commands\":["
                       "{\"name\":\"Vol\",\"keyCode\":\"G:NEC:(0x1)():3\",\"irKeyCode\":\"x\",\"irWhen\":\"y\"}]}]}");
    sanitize_devices_json(root);
    sanitize_devices_json(root);
    cmds = cJSON_GetObjectItemCaseSensitive(dev_of(root, 0), "commands");
    const cJSON *ikc = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 0), "irKeyCode");
    const cJSON *iw = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 0), "irWhen");
    CHECK(cJSON_IsString(ikc) && strcmp(ikc->valuestring, "G:GoVideoO1 32 Bit:()(0xE0E07D82)():3") == 0 &&
          cJSON_IsString(iw) && strcmp(iw->valuestring, "hdmi_via_ir") == 0, "irKeyCode and irWhen kept over two passes");
    CHECK(num_of(cJSON_GetArrayItem(cmds, 0), "port") == 2 && num_of(cJSON_GetArrayItem(cmds, 0), "repeats") == 3,
          "a command with an irKeyCode keeps its IR port and gets repeats");
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 1), "irKeyCode") == NULL &&
          cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 1), "irWhen") == NULL, "invalid irKeyCode and irWhen dropped");
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 2), "irKeyCode") != NULL &&
          cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 2), "irWhen") == NULL, "irWhen over 63 characters dropped");
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 3), "port") == NULL &&
          cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 3), "repeats") == NULL, "a plain plugin command still has no IR fields");
    cmds = cJSON_GetObjectItemCaseSensitive(dev_of(root, 1), "commands");
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cmds, 0), "irKeyCode") == NULL, "no irKeyCode on an IR device's command");
    cJSON_Delete(root);

    printf("[-] hideOnRemote / hideInDevicesTab\n");
    root = cJSON_Parse("{\"devices\":[{\"id\":\"1\",\"name\":\"TV\",\"hideOnRemote\":true},"
                       "{\"id\":\"2\",\"name\":\"AVR\",\"hideOnRemote\":false,\"hideInDevicesTab\":true}]}");
    sanitize_devices_json(root);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(dev_of(root, 0), "hideOnRemote")), "hideOnRemote true kept");
    CHECK(cJSON_GetObjectItemCaseSensitive(dev_of(root, 1), "hideOnRemote") == NULL, "hideOnRemote false not stored");
    CHECK(cJSON_GetObjectItemCaseSensitive(dev_of(root, 0), "hideInDevicesTab") == NULL, "hideInDevicesTab absent not added");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(dev_of(root, 1), "hideInDevicesTab")),
          "hideInDevicesTab kept apart from hideOnRemote");
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
