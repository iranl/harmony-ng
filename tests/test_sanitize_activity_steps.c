#include "cJSON.h"
#include "config_sanitize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int main(void) {
    printf("[*] Testing sanitize_activity_steps (canonical single 'delay' key, no duplicates)...\n");

    /* 1. Test delay with canonical "delay": 2500 */
    cJSON *steps1 = cJSON_Parse("[{\"type\":\"Delay\",\"deviceId\":\"0\",\"command\":\"\",\"delay\":2500}]");
    assert(steps1 != NULL);
    sanitize_activity_steps(steps1);
    cJSON *s0 = cJSON_GetArrayItem(steps1, 0);
    assert(s0 != NULL);
    cJSON *t0 = cJSON_GetObjectItemCaseSensitive(s0, "type");
    cJSON *d0 = cJSON_GetObjectItemCaseSensitive(s0, "delay");
    assert(t0 && strcmp(t0->valuestring, "Delay") == 0);
    assert(d0 && d0->valueint == 2500);
    /* Verify NO duplicate legacy keys */
    assert(cJSON_GetObjectItemCaseSensitive(s0, "delay_ms") == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(s0, "delayMs") == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(s0, "Delay") == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(s0, "Duration") == NULL);
    cJSON_Delete(steps1);

    /* 2. Test delay with string representation "delay": "3500" */
    cJSON *steps2 = cJSON_Parse("[{\"type\":\"Delay\",\"delay\":\"3500\"}]");
    assert(steps2 != NULL);
    sanitize_activity_steps(steps2);
    s0 = cJSON_GetArrayItem(steps2, 0);
    assert(s0 != NULL);
    d0 = cJSON_GetObjectItemCaseSensitive(s0, "delay");
    assert(d0 && d0->valueint == 3500);
    assert(cJSON_GetObjectItemCaseSensitive(s0, "delay_ms") == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(s0, "delayMs") == NULL);
    cJSON_Delete(steps2);

    /* 3. Test Delay type step with default delay when delay is 0 */
    cJSON *steps3 = cJSON_Parse("[{\"type\":\"Delay\"}]");
    assert(steps3 != NULL);
    sanitize_activity_steps(steps3);
    s0 = cJSON_GetArrayItem(steps3, 0);
    assert(s0 != NULL);
    d0 = cJSON_GetObjectItemCaseSensitive(s0, "delay");
    assert(d0 && d0->valueint == 1000);
    assert(cJSON_GetObjectItemCaseSensitive(s0, "Duration") == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(s0, "delay_ms") == NULL);
    cJSON_Delete(steps3);

    /* 4. Test Switched pseudo-step is preserved */
    cJSON *steps4 = cJSON_Parse("[{\"type\":\"Switched\",\"deviceId\":\"0\",\"command\":\"Switched\",\"delay\":0}]");
    assert(steps4 != NULL);
    sanitize_activity_steps(steps4);
    s0 = cJSON_GetArrayItem(steps4, 0);
    assert(s0 != NULL);
    t0 = cJSON_GetObjectItemCaseSensitive(s0, "type");
    cJSON *cmd0 = cJSON_GetObjectItemCaseSensitive(s0, "command");
    assert(t0 && strcmp(t0->valuestring, "Switched") == 0);
    assert(cmd0 && strcmp(cmd0->valuestring, "Switched") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(s0, "delay_ms") == NULL);
    cJSON_Delete(steps4);

    /* 5. Test Command step */
    cJSON *steps5 = cJSON_Parse("[{\"type\":\"Command\",\"deviceId\":\"3\",\"command\":\"PowerOn\"}]");
    assert(steps5 != NULL);
    sanitize_activity_steps(steps5);
    s0 = cJSON_GetArrayItem(steps5, 0);
    assert(s0 != NULL);
    t0 = cJSON_GetObjectItemCaseSensitive(s0, "type");
    cmd0 = cJSON_GetObjectItemCaseSensitive(s0, "command");
    cJSON *dev0 = cJSON_GetObjectItemCaseSensitive(s0, "deviceId");
    assert(t0 && strcmp(t0->valuestring, "Command") == 0);
    assert(cmd0 && strcmp(cmd0->valuestring, "PowerOn") == 0);
    assert(dev0 && strcmp(dev0->valuestring, "3") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(s0, "delay_ms") == NULL);
    cJSON_Delete(steps5);

    /* 6. Non-string type/command (null, number, array): no crash, step without a command dropped */
    cJSON *steps6 = cJSON_Parse("[{\"command\":2},{\"type\":null},{\"type\":[],\"command\":{}},"
                                "{\"type\":null,\"command\":\"Mute\",\"deviceId\":\"4\"}]");
    assert(steps6 != NULL);
    sanitize_activity_steps(steps6);
    assert(cJSON_GetArraySize(steps6) == 1);
    s0 = cJSON_GetArrayItem(steps6, 0);
    cmd0 = cJSON_GetObjectItemCaseSensitive(s0, "command");
    assert(cmd0 && strcmp(cmd0->valuestring, "Mute") == 0);
    cJSON_Delete(steps6);

    /* 7. Numeric deviceId is kept as its string form, not replaced by "0" */
    cJSON *steps7 = cJSON_Parse("[{\"type\":\"Command\",\"command\":\"PowerOn\",\"deviceId\":3}]");
    assert(steps7 != NULL);
    sanitize_activity_steps(steps7);
    s0 = cJSON_GetArrayItem(steps7, 0);
    dev0 = cJSON_GetObjectItemCaseSensitive(s0, "deviceId");
    assert(dev0 && cJSON_IsString(dev0) && strcmp(dev0->valuestring, "3") == 0);
    cJSON_Delete(steps7);

    printf("[+] ALL sanitize_activity_steps tests passed (100%% canonical, zero duplicate keys)!\n");
    return 0;
}
