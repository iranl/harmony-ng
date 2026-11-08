#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "cJSON.h"

static int hub_valid_bdaddr(const char *s) {
    int i;
    if (!s || strlen(s) != 17) return 0;
    for (i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (s[i] != ':') return 0; }
        else { if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return 0; }
    }
    return 1;
}

static int test_evaluate_reconnect(cJSON *devs, const char *executed_dev_id, const char *executed_dev_name, const char *executed_cmd,
                                   char *out_target_bdaddr, size_t bdaddr_sz, int *out_delay, int *out_dur) {
    if (!devs || !executed_cmd || !executed_cmd[0]) return 0;

    /* Do not trigger BT reconnect for power-down or standby commands */
    if (strcasecmp(executed_cmd, "PowerOff") == 0 ||
        strcasecmp(executed_cmd, "Sleep") == 0 ||
        strcasecmp(executed_cmd, "PowerToggle") == 0 ||
        strcasecmp(executed_cmd, "Power") == 0 ||
        strcasecmp(executed_cmd, "Standby") == 0) {
        return 0;
    }

    int matched_any = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devs) {
        cJSON *dev = item;

        int is_bt = 0;
        cJSON *j_type = cJSON_GetObjectItemCaseSensitive(dev, "type");
        if (j_type && cJSON_IsString(j_type) && j_type->valuestring) {
            if (strcasecmp(j_type->valuestring, "BT") == 0 ||
                strcasecmp(j_type->valuestring, "Bluetooth") == 0 ||
                strcasecmp(j_type->valuestring, "BTHID") == 0 ||
                strcasestr(j_type->valuestring, "bluetooth") != NULL) {
                is_bt = 1;
            }
        }

        cJSON *j_bta = cJSON_GetObjectItemCaseSensitive(dev, "bdaddr");
        if (!j_bta) j_bta = cJSON_GetObjectItemCaseSensitive(dev, "btHost");
        if (j_bta && cJSON_IsString(j_bta) && j_bta->valuestring && hub_valid_bdaddr(j_bta->valuestring)) {
            is_bt = 1;
        }

        if (cJSON_GetObjectItemCaseSensitive(dev, "btAutoReconnect") ||
            cJSON_GetObjectItemCaseSensitive(dev, "btTriggerCommandName")) {
            is_bt = 1;
        }

        if (!is_bt) continue;

        cJSON *j_rec = cJSON_GetObjectItemCaseSensitive(dev, "btAutoReconnect");
        if (j_rec) {
            int rec_val = cJSON_IsNumber(j_rec) ? (int)j_rec->valuedouble : (cJSON_IsBool(j_rec) ? (cJSON_IsTrue(j_rec) ? 1 : 0) : (j_rec->valuestring ? atoi(j_rec->valuestring) : 1));
            if (rec_val == 0) continue;
        }

        char cur_id[64] = {0};
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(dev, "id");
        if (jid) {
            if (cJSON_IsNumber(jid)) snprintf(cur_id, sizeof(cur_id), "%ld", (long)jid->valuedouble);
            else if (cJSON_IsString(jid) && jid->valuestring) strncpy(cur_id, jid->valuestring, sizeof(cur_id) - 1);
        }

        char cur_name[64] = {0};
        cJSON *jnm = cJSON_GetObjectItemCaseSensitive(dev, "name");
        if (jnm && cJSON_IsString(jnm) && jnm->valuestring) {
            strncpy(cur_name, jnm->valuestring, sizeof(cur_name) - 1);
        }

        int target_dev_delay = -1;
        cJSON *jd_dly = cJSON_GetObjectItemCaseSensitive(dev, "btReconnectDelaySec");
        if (jd_dly) {
            if (cJSON_IsNumber(jd_dly) && jd_dly->valuedouble >= 0) target_dev_delay = (int)jd_dly->valuedouble;
            else if (cJSON_IsString(jd_dly) && jd_dly->valuestring && jd_dly->valuestring[0]) target_dev_delay = atoi(jd_dly->valuestring);
        }

        int dur = 0;
        cJSON *jd_dur = cJSON_GetObjectItemCaseSensitive(dev, "btReconnectDurationSec");
        if (jd_dur) {
            if (cJSON_IsNumber(jd_dur) && jd_dur->valuedouble > 0) dur = (int)jd_dur->valuedouble;
            else if (cJSON_IsString(jd_dur) && jd_dur->valuestring) dur = atoi(jd_dur->valuestring);
        }
        if (dur <= 0) dur = 30;

        char trig_did_buf[64] = {0};
        cJSON *j_trig_dev = cJSON_GetObjectItemCaseSensitive(dev, "btTriggerDeviceId");
        if (j_trig_dev) {
            if (cJSON_IsString(j_trig_dev) && j_trig_dev->valuestring) {
                strncpy(trig_did_buf, j_trig_dev->valuestring, sizeof(trig_did_buf) - 1);
            } else if (cJSON_IsNumber(j_trig_dev)) {
                snprintf(trig_did_buf, sizeof(trig_did_buf), "%ld", (long)j_trig_dev->valuedouble);
            }
        }
        const char *trig_did = trig_did_buf;

        char trig_cmd_buf[64] = {0};
        cJSON *j_trig_cmd = cJSON_GetObjectItemCaseSensitive(dev, "btTriggerCommandName");
        if (j_trig_cmd) {
            if (cJSON_IsString(j_trig_cmd) && j_trig_cmd->valuestring) {
                strncpy(trig_cmd_buf, j_trig_cmd->valuestring, sizeof(trig_cmd_buf) - 1);
            } else if (cJSON_IsNumber(j_trig_cmd)) {
                snprintf(trig_cmd_buf, sizeof(trig_cmd_buf), "%ld", (long)j_trig_cmd->valuedouble);
            }
        }
        const char *trig_cmd = trig_cmd_buf;

        /* If colon is in trig_cmd (e.g. "deviceId:PowerOn" or "MQTT:PowerOn"), separate them */
        const char *colon = strchr(trig_cmd, ':');
        if (colon) {
            if (!trig_did[0]) {
                size_t did_len = (size_t)(colon - trig_cmd);
                if (did_len >= sizeof(trig_did_buf)) did_len = sizeof(trig_did_buf) - 1;
                strncpy(trig_did_buf, trig_cmd, did_len);
                trig_did_buf[did_len] = '\0';
                trig_did = trig_did_buf;
            }
            trig_cmd = colon + 1;
        }

        int should_reconnect = 0;
        int dly = 0;

        if (trig_cmd[0]) {
            if (strcasecmp(executed_cmd, trig_cmd) == 0) {
                int match_dev = 1;
                if (trig_did && trig_did[0]) {
                    match_dev = 0;
                    if (executed_dev_id && (strcmp(executed_dev_id, trig_did) == 0 || strcasecmp(executed_dev_id, trig_did) == 0)) match_dev = 1;
                    if (!match_dev && executed_dev_name && (strcmp(executed_dev_name, trig_did) == 0 || strcasecmp(executed_dev_name, trig_did) == 0)) match_dev = 1;
                }
                if (match_dev) {
                    should_reconnect = 1;
                    dly = (target_dev_delay >= 0) ? target_dev_delay : 2;
                }
            }
        }

        /* Fallback: any button press sent directly to this BT device */
        if (!should_reconnect) {
            int is_target_dev = 0;
            if (executed_dev_id && cur_id[0] && (strcmp(executed_dev_id, cur_id) == 0 || strcasecmp(executed_dev_id, cur_id) == 0)) is_target_dev = 1;
            if (!is_target_dev && executed_dev_name && cur_name[0] && (strcmp(executed_dev_name, cur_name) == 0 || strcasecmp(executed_dev_name, cur_name) == 0)) is_target_dev = 1;
            if (is_target_dev) {
                should_reconnect = 1;
                dly = 0;
            }
        }

        if (should_reconnect) {
            matched_any = 1;
            if (out_delay) *out_delay = dly;
            if (out_dur) *out_dur = dur;
            if (out_target_bdaddr && j_bta && cJSON_IsString(j_bta)) {
                strncpy(out_target_bdaddr, j_bta->valuestring, bdaddr_sz - 1);
            }
            break;
        }
    }
    return matched_any;
}

int main(void) {
    printf("[*] Running BT reconnect trigger test suite...\n");

    /* Scenario 1: Shield TV with type='BT', delay=45, trigger='100:PowerOn', executed on MQTT device 100 */
    const char *json_txt = "["
        "{"
            "\"id\": \"7234912\","
            "\"name\": \"NVIDIA Shield\","
            "\"type\": \"BT\","
            "\"bdaddr\": \"00:04:4B:66:77:88\","
            "\"btAutoReconnect\": 1,"
            "\"btReconnectDelaySec\": 45,"
            "\"btReconnectDurationSec\": 30,"
            "\"btTriggerDeviceId\": \"100\","
            "\"btTriggerCommandName\": \"100:PowerOn\""
        "},"
        "{"
            "\"id\": \"100\","
            "\"name\": \"MQTT Switch\","
            "\"type\": \"MQTT\","
            "\"mqttEnabled\": 1"
        "}"
    "]";

    cJSON *devs = cJSON_Parse(json_txt);
    assert(devs != NULL);

    char bdaddr[32] = {0};
    int delay = 0, dur = 0;
    int rc = test_evaluate_reconnect(devs, "100", "MQTT Switch", "PowerOn", bdaddr, sizeof(bdaddr), &delay, &dur);
    assert(rc == 1);
    assert(delay == 45);
    assert(dur == 30);
    assert(strcmp(bdaddr, "00:04:4B:66:77:88") == 0);
    printf("  [PASS] Test 1: Shield TV triggers on MQTT Switch PowerOn with delay=45s\n");

    /* Scenario 2: btTriggerCommandName is just 'PowerOn' and trig_did is 'MQTT Switch' (by name) */
    cJSON_Delete(devs);
    json_txt = "["
        "{"
            "\"id\": \"7234912\","
            "\"name\": \"NVIDIA Shield\","
            "\"type\": \"Bluetooth\","
            "\"bdaddr\": \"00:04:4B:66:77:88\","
            "\"btAutoReconnect\": 1,"
            "\"btReconnectDelaySec\": \"45\","
            "\"btTriggerDeviceId\": \"MQTT Switch\","
            "\"btTriggerCommandName\": \"PowerOn\""
        "}"
    "]";
    devs = cJSON_Parse(json_txt);
    assert(devs != NULL);
    rc = test_evaluate_reconnect(devs, "100", "MQTT Switch", "poweron", bdaddr, sizeof(bdaddr), &delay, &dur);
    assert(rc == 1);
    assert(delay == 45);
    printf("  [PASS] Test 2: Matching by device name and string delay=45\n");

    /* Scenario 3: Button press fallback on Shield TV itself */
    rc = test_evaluate_reconnect(devs, "7234912", "NVIDIA Shield", "Home", bdaddr, sizeof(bdaddr), &delay, &dur);
    assert(rc == 1);
    assert(delay == 0);
    printf("  [PASS] Test 3: Button press on Shield TV itself triggers immediate reconnect (delay=0)\n");

    /* Scenario 4: Command mismatch */
    rc = test_evaluate_reconnect(devs, "100", "MQTT Switch", "VolumeUp", bdaddr, sizeof(bdaddr), &delay, &dur);
    assert(rc == 0);
    printf("  [PASS] Test 4: Unrelated command does not trigger reconnect\n");

    /* Scenario 5: PowerOff and Sleep on Shield TV do NOT trigger reconnect */
    rc = test_evaluate_reconnect(devs, "7234912", "NVIDIA Shield", "PowerOff", bdaddr, sizeof(bdaddr), &delay, &dur);
    assert(rc == 0);
    rc = test_evaluate_reconnect(devs, "7234912", "NVIDIA Shield", "Sleep", bdaddr, sizeof(bdaddr), &delay, &dur);
    assert(rc == 0);
    printf("  [PASS] Test 5: PowerOff and Sleep on Shield TV do not trigger reconnect\n");

    cJSON_Delete(devs);
    printf("[+] All tests passed successfully!\n");
    return 0;
}
