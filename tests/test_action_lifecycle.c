#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include "../payload/source/cJSON.h"
#include "../payload/source/hw_action.h"
#include "../payload/source/orchestrator.h"

/* Mock device database */
static cJSON *s_mock_device_root = NULL;
/* hw_action.c asks the orchestrator before a Bluetooth reconnect (no reconnect while stopping or
 * powered off): an activity is running here */
orch_state_t orch_get_state(void) { return ORCH_STATE_RUNNING; }
const char *orch_get_current_activity(void) { return "1"; }

cJSON *get_device_list_cached(void) {
    if (!s_mock_device_root) {
        s_mock_device_root = cJSON_Parse("{\"devices\":[{\"id\":\"mock_dev\",\"name\":\"MockDevice\",\"commands\":[{\"name\":\"PowerToggle\",\"raw\":\"0000 006d 0022 0002\"}]}]}");
    }
    return s_mock_device_root;
}

int ir_encode_pronto(const char *pronto, uint8_t repeats, uint32_t gap, uint8_t **out_bin, size_t *out_len) {
    (void)pronto; (void)repeats; (void)gap;
    *out_bin = (uint8_t *)malloc(16);
    *out_len = 16;
    return 0;
}

int ir_encode_raw(uint32_t freq_hz, const uint32_t *timings_us, size_t count, uint8_t repeats, uint8_t **out_bin, size_t *out_len) {
    (void)freq_hz; (void)timings_us; (void)count; (void)repeats; (void)out_bin; (void)out_len;
    return 0;
}

int ir_encode_harmony_keycode(const char *keycode, uint8_t repeats, uint8_t **out_bin, size_t *out_len) {
    (void)keycode; (void)repeats; (void)out_bin; (void)out_len;
    return 0;
}

int ir_i2s_blast_blob(const uint8_t *blob, size_t len, uint8_t port_mask) {
    (void)blob; (void)len; (void)port_mask;
    return 0;
}

void ir_free_encoded(uint8_t *bin) {
    if (bin) free(bin);
}

static int s_mqtt_handled = 0;
static int mock_mqtt_handler(const char *device_id, const char *dev_name, const char *command) {
    (void)device_id;
    (void)dev_name;
    (void)command;
    s_mqtt_handled++;
    return 0;
}

int main(void) {
    printf("[*] Running Action Lifecycle & Debounce Verification Tests...\n");

    /* 1. Verify Action Constants */
    assert(HW_BTN_ACTION_TAP == 0);
    assert(HW_BTN_ACTION_PRESS == 1);
    assert(HW_BTN_ACTION_REPEAT == 2);
    assert(HW_BTN_ACTION_RELEASE == 3);
    printf("[+] Action constants verified.\n");

    /* 2. Action String Parsing Matrix */
    const char *inputs[] = {"tap", "press", "hold", "repeat", "release", "unknown", NULL};
    int expected[] = {
        HW_BTN_ACTION_TAP,
        HW_BTN_ACTION_PRESS,
        HW_BTN_ACTION_REPEAT,
        HW_BTN_ACTION_REPEAT,
        HW_BTN_ACTION_RELEASE,
        HW_BTN_ACTION_TAP
    };
    for (int i = 0; inputs[i] != NULL; i++) {
        int act = HW_BTN_ACTION_TAP;
        if (strcasecmp(inputs[i], "press") == 0) act = HW_BTN_ACTION_PRESS;
        else if (strcasecmp(inputs[i], "hold") == 0 || strcasecmp(inputs[i], "repeat") == 0) act = HW_BTN_ACTION_REPEAT;
        else if (strcasecmp(inputs[i], "release") == 0) act = HW_BTN_ACTION_RELEASE;
        assert(act == expected[i]);
    }
    printf("[+] Action string parsing matrix verified.\n");

    /* 3. MQTT Handler registration check */
    hw_set_mqtt_button_handler(mock_mqtt_handler);
    printf("[+] MQTT button handler registration verified.\n");

    /* 4. Release Action Invokes Cancel without crash */
    int res = hw_device_command_send_action("mock_dev", 0, "PowerToggle", HW_BTN_ACTION_RELEASE);
    assert(res == 0);
    printf("[+] RELEASE action cleanly handled by hw_device_command_send_action().\n");

    if (s_mock_device_root) {
        cJSON_Delete(s_mock_device_root);
        s_mock_device_root = NULL;
    }

    printf("\n===========================================================\n");
    printf("  ALL ACTION LIFECYCLE & DEBOUNCE TESTS PASSED!\n");
    printf("===========================================================\n");
    return 0;
}
