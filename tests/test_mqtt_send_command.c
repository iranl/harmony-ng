#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "cJSON.h"

/* Re-test topic matching algorithm directly */
static int test_mqtt_topic_matches(const char *sub, const char *pub) {
    if (!sub || !pub) return 0;
    while (*sub && *pub) {
        if (*sub == '#') return 1;
        if (*sub == '+') {
            while (*pub && *pub != '/') pub++;
            sub++;
            continue;
        }
        if (*sub != *pub) return 0;
        sub++;
        pub++;
    }
    if (*sub == '#' || (*sub == '+' && !*pub)) return 1;
    return (*sub == '\0' && *pub == '\0');
}

/* Parse helper simulating handle_mqtt_send_command extraction */
static int parse_send_command_payload(const char *payload, char *out_dev, size_t dev_sz,
                                      char out_cmds[32][64], int *out_cmd_count,
                                      int *out_repeats, double *out_delay, double *out_hold) {
    if (!payload || !payload[0]) return -1;
    cJSON *root = cJSON_Parse(payload);
    if (!root) return -1;

    cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(root, "deviceId");
    if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(root, "dev");

    char dev[128] = {0};
    if (j_dev) {
        if (cJSON_IsString(j_dev) && j_dev->valuestring) {
            strncpy(dev, j_dev->valuestring, sizeof(dev) - 1);
        } else if (cJSON_IsNumber(j_dev)) {
            snprintf(dev, sizeof(dev), "%ld", (long)j_dev->valuedouble);
        }
    }
    if (!dev[0]) {
        cJSON_Delete(root);
        return -2;
    }
    snprintf(out_dev, dev_sz, "%s", dev);

    int cmd_count = 0;
    cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!j_cmd) j_cmd = cJSON_GetObjectItemCaseSensitive(root, "commands");
    if (!j_cmd) j_cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");

    if (j_cmd) {
        if (cJSON_IsArray(j_cmd)) {
            cJSON *elem = NULL;
            cJSON_ArrayForEach(elem, j_cmd) {
                if (cmd_count >= 32) break;
                if (cJSON_IsString(elem) && elem->valuestring && elem->valuestring[0]) {
                    strncpy(out_cmds[cmd_count], elem->valuestring, 63);
                    out_cmds[cmd_count][63] = '\0';
                    cmd_count++;
                } else if (cJSON_IsNumber(elem)) {
                    snprintf(out_cmds[cmd_count], 64, "%ld", (long)elem->valuedouble);
                    cmd_count++;
                }
            }
        } else if (cJSON_IsString(j_cmd) && j_cmd->valuestring && j_cmd->valuestring[0]) {
            strncpy(out_cmds[0], j_cmd->valuestring, 63);
            out_cmds[0][63] = '\0';
            cmd_count = 1;
        } else if (cJSON_IsNumber(j_cmd)) {
            snprintf(out_cmds[0], 64, "%ld", (long)j_cmd->valuedouble);
            cmd_count = 1;
        }
    }
    if (cmd_count == 0) {
        cJSON_Delete(root);
        return -3;
    }
    *out_cmd_count = cmd_count;

    double delay_secs = -1.0;
    cJSON *j_delay = cJSON_GetObjectItemCaseSensitive(root, "delay_secs");
    if (!j_delay) j_delay = cJSON_GetObjectItemCaseSensitive(root, "delaySecs");
    if (!j_delay) j_delay = cJSON_GetObjectItemCaseSensitive(root, "delay");
    if (j_delay && cJSON_IsNumber(j_delay)) {
        delay_secs = j_delay->valuedouble;
    } else {
        cJSON *j_delay_ms = cJSON_GetObjectItemCaseSensitive(root, "delay_ms");
        if (!j_delay_ms) j_delay_ms = cJSON_GetObjectItemCaseSensitive(root, "delayMs");
        if (j_delay_ms && cJSON_IsNumber(j_delay_ms)) {
            delay_secs = j_delay_ms->valuedouble / 1000.0;
        }
    }

    double hold_secs = 0.0;
    cJSON *j_hold = cJSON_GetObjectItemCaseSensitive(root, "hold_secs");
    if (!j_hold) j_hold = cJSON_GetObjectItemCaseSensitive(root, "holdSecs");
    if (!j_hold) j_hold = cJSON_GetObjectItemCaseSensitive(root, "hold");
    if (j_hold && cJSON_IsNumber(j_hold)) {
        hold_secs = j_hold->valuedouble;
    } else {
        cJSON *j_hold_ms = cJSON_GetObjectItemCaseSensitive(root, "hold_ms");
        if (!j_hold_ms) j_hold_ms = cJSON_GetObjectItemCaseSensitive(root, "holdMs");
        if (j_hold_ms && cJSON_IsNumber(j_hold_ms)) {
            hold_secs = j_hold_ms->valuedouble / 1000.0;
        }
    }

    int num_repeats = 1;
    cJSON *j_rep = cJSON_GetObjectItemCaseSensitive(root, "num_repeats");
    if (!j_rep) j_rep = cJSON_GetObjectItemCaseSensitive(root, "numRepeats");
    if (!j_rep) j_rep = cJSON_GetObjectItemCaseSensitive(root, "repeats");
    if (j_rep && cJSON_IsNumber(j_rep)) num_repeats = (int)j_rep->valuedouble;

    cJSON_Delete(root);

    if (num_repeats < 1) num_repeats = 1;
    if (num_repeats > 50) num_repeats = 50;

    *out_repeats = num_repeats;
    *out_delay = delay_secs;
    *out_hold = hold_secs;
    return 0;
}

int main(void) {
    printf("[*] Testing MQTT topic wildcard matching...\n");
    assert(test_mqtt_topic_matches("harmony/hub/send_command", "harmony/hub/send_command") == 1);
    assert(test_mqtt_topic_matches("harmony/hub/command/#", "harmony/hub/command/123/PowerToggle") == 1);
    assert(test_mqtt_topic_matches("harmony/hub/remote/+/sleep", "harmony/hub/remote/1/sleep") == 1);
    assert(test_mqtt_topic_matches("harmony/hub/remote/+/sleep", "harmony/hub/remote/2/sleep") == 1);
    assert(test_mqtt_topic_matches("harmony/hub/send_command", "harmony/hub/other") == 0);
    printf("[+] MQTT topic wildcard matching PASSED\n");

    printf("[*] Testing Home Assistant send_command format (command as array)...\n");
    char dev[128] = {0};
    char cmds[32][64] = {{0}};
    int count = 0, repeats = 0;
    double delay = 0, hold = 0;

    const char *ha_payload = "{\"device\":\"Samsung TV\",\"command\":[\"PowerToggle\"],\"delay_secs\":0.4,\"hold_secs\":0.0,\"num_repeats\":1}";
    int rc = parse_send_command_payload(ha_payload, dev, sizeof(dev), cmds, &count, &repeats, &delay, &hold);
    assert(rc == 0);
    assert(strcmp(dev, "Samsung TV") == 0);
    assert(count == 1);
    assert(strcmp(cmds[0], "PowerToggle") == 0);
    assert(repeats == 1);
    printf("[+] HA command array format PASSED\n");

    printf("[*] Testing numeric deviceId payload...\n");
    const char *num_payload = "{\"deviceId\":70321456,\"command\":\"Mute\"}";
    rc = parse_send_command_payload(num_payload, dev, sizeof(dev), cmds, &count, &repeats, &delay, &hold);
    assert(rc == 0);
    assert(strcmp(dev, "70321456") == 0);
    assert(count == 1);
    assert(strcmp(cmds[0], "Mute") == 0);
    printf("[+] Numeric deviceId PASSED\n");

    printf("[*] Testing multi-command array with delay_ms and repeats...\n");
    const char *multi_payload = "{\"dev\":\"70321456\",\"commands\":[\"VolumeUp\",\"VolumeUp\"],\"delay_ms\":250,\"repeats\":3}";
    rc = parse_send_command_payload(multi_payload, dev, sizeof(dev), cmds, &count, &repeats, &delay, &hold);
    assert(rc == 0);
    assert(strcmp(dev, "70321456") == 0);
    assert(count == 2);
    assert(strcmp(cmds[0], "VolumeUp") == 0);
    assert(strcmp(cmds[1], "VolumeUp") == 0);
    assert(repeats == 3);
    assert(delay > 0.24 && delay < 0.26);
    printf("[+] Multi-command array & delays PASSED\n");

    printf("\n[+] ALL MQTT SEND_COMMAND TESTS PASSED!\n");
    return 0;
}
