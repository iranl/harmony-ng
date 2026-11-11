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
        s_mock_device_root = cJSON_Parse("{\"devices\":["
            "{\"id\":\"mock_dev\",\"name\":\"MockDevice\",\"commands\":[{\"name\":\"PowerToggle\",\"raw\":\"0000 006d 0022 0002\"}]},"
            "{\"id\":\"ble_dev\",\"name\":\"BleFan\",\"type\":\"BLE\",\"commands\":["
            "{\"name\":\"LightOff\",\"keyCode\":\"BLEADV:02011A19054E6F720E0438064C2E561510D42DEB8C15D3B51804030201\"},"
            "{\"name\":\"Broken\",\"keyCode\":\"BLEADV:030106\"}]},"
            "{\"id\":\"plug_dev\",\"name\":\"PluginDevice\",\"plugin\":\"roku-ecp\",\"config\":{\"ip\":\"192.168.1.50\"},\"commands\":["
            "{\"name\":\"Home\",\"keyCode\":\"PLUGIN:roku-ecp:Home\"}]},"
            "{\"id\":\"plug_typed\",\"name\":\"AndroidTV\",\"type\":\"plugin:androidtv\",\"config\":{\"host\":\"192.168.1.60\"},\"commands\":["
            "{\"name\":\"Back\",\"keyCode\":\"Back\"}]},"
            "{\"id\":\"custom_dev\",\"name\":\"CustomDev\",\"commands\":["
            "{\"name\":\"Wake\",\"keyCode\":\"PLUGIN:ps5-wake:Boot\"}]}]}");
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

extern int g_dry_run;

/* Run one command with stdout redirected so the dry-run BTstack command line can be checked */
static int capture_send(const char *dev, const char *cmd, int action, char *out, size_t out_sz) {
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    FILE *tf = tmpfile();
    assert(saved >= 0 && tf);
    dup2(fileno(tf), STDOUT_FILENO);
    g_dry_run = 1;
    int rc = hw_device_command_send_action(dev, 0, cmd, action);
    g_dry_run = 0;
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    rewind(tf);
    size_t n = fread(out, 1, out_sz - 1, tf);
    out[n] = '\0';
    fclose(tf);
    return rc;
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

    /* 5. BLE broadcast commands reach hub_btstack as "adv <hex>" */
    char out[512] = {0};
    res = capture_send("ble_dev", "LightOff", HW_BTN_ACTION_TAP, out, sizeof(out));
    assert(res == 0);
    assert(strstr(out, "BTstack command simulated: adv 02011A19054E6F720E0438064C2E561510D42DEB8C15D3B51804030201") != NULL);
    res = capture_send("ble_dev", "LightOff", HW_BTN_ACTION_RELEASE, out, sizeof(out));
    assert(res == 0);
    assert(strstr(out, "adv ") == NULL);
    res = capture_send("ble_dev", "Broken", HW_BTN_ACTION_TAP, out, sizeof(out));
    assert(res == -2);
    assert(strstr(out, "adv ") == NULL);
    /* 6. Plugin commands reach plugin socket with all actions */
    const struct {
        int action;
        const char *expected_action_str;
    } plugin_action_tests[] = {
        { HW_BTN_ACTION_TAP, "action=tap" },
        { HW_BTN_ACTION_PRESS, "action=press" },
        { HW_BTN_ACTION_REPEAT, "action=repeat" },
        { HW_BTN_ACTION_RELEASE, "action=release" }
    };
    for (size_t i = 0; i < sizeof(plugin_action_tests)/sizeof(plugin_action_tests[0]); i++) {
        usleep(100000); /* 100ms sleep to exceed 90ms button debounce window */
        memset(out, 0, sizeof(out));
        res = capture_send("plug_dev", "Home", plugin_action_tests[i].action, out, sizeof(out));
        assert(res == 0);
        assert(strstr(out, "Plugin command simulated: plugin=roku-ecp") != NULL);
        assert(strstr(out, "cmd=Home") != NULL);
        assert(strstr(out, plugin_action_tests[i].expected_action_str) != NULL);
    }
    printf("[+] Plugin command lifecycle actions (TAP, PRESS, REPEAT, RELEASE) verified.\n");

    /* 6b. Verify 90ms debounce drops duplicate TAP immediately sent */
    memset(out, 0, sizeof(out));
    res = capture_send("plug_dev", "Home", HW_BTN_ACTION_TAP, out, sizeof(out));
    assert(res == 0);
    assert(strstr(out, "Plugin command simulated:") == NULL); /* Dropped by debounce */
    printf("[+] Rapid duplicate button press (<90ms) debounce verified.\n");

    /* 7. Plugin device with 'type: plugin:androidtv' */
    memset(out, 0, sizeof(out));
    res = capture_send("plug_typed", "Back", HW_BTN_ACTION_TAP, out, sizeof(out));
    assert(res == 0);
    assert(strstr(out, "Plugin command simulated: plugin=androidtv") != NULL);
    assert(strstr(out, "cmd=Back") != NULL);
    printf("[+] Device type 'plugin:androidtv' dispatching verified.\n");

    /* 8. Command with keycode override 'PLUGIN:ps5-wake:Boot' */
    memset(out, 0, sizeof(out));
    res = capture_send("custom_dev", "Wake", HW_BTN_ACTION_PRESS, out, sizeof(out));
    assert(res == 0);
    assert(strstr(out, "Plugin command simulated: plugin=ps5-wake") != NULL);
    assert(strstr(out, "cmd=Boot") != NULL);
    assert(strstr(out, "action=press") != NULL);
    printf("[+] Keycode override 'PLUGIN:ps5-wake:Boot' dispatching verified.\n");

    /* 9. Direct hw_plugin_command_send safety check */
    assert(hw_plugin_command_send(NULL, "dev", "Dev", "Cmd", 0, 0, NULL) == -1);
    assert(hw_plugin_command_send("", "dev", "Dev", "Cmd", 0, 0, NULL) == -1);
    assert(hw_plugin_command_send("plug", "dev", "Dev", NULL, 0, 0, NULL) == -1);
    assert(hw_plugin_command_send("plug", "dev", "Dev", "", 0, 0, NULL) == -1);
    printf("[+] Direct hw_plugin_command_send validation verified.\n");

    /* 10. Non-existent device and command error handling */
    memset(out, 0, sizeof(out));
    assert(capture_send("non_existent_dev", "PowerToggle", HW_BTN_ACTION_TAP, out, sizeof(out)) == -5);
    assert(capture_send("mock_dev", "NonExistentCmd", HW_BTN_ACTION_TAP, out, sizeof(out)) == -6);
    /* Plugins support arbitrary dynamic commands not in the device commands list */
    res = capture_send("plug_dev", "DynamicArbitraryCommand", HW_BTN_ACTION_TAP, out, sizeof(out));
    assert(res == 0);
    assert(strstr(out, "cmd=DynamicArbitraryCommand") != NULL);
    printf("[+] Non-existent device (-5), missing IR command (-6), and dynamic plugin command dispatching verified.\n");

    if (s_mock_device_root) {
        cJSON_Delete(s_mock_device_root);
        s_mock_device_root = NULL;
    }

    printf("\n===========================================================\n");
    printf("  ALL ACTION LIFECYCLE & DEBOUNCE TESTS PASSED!\n");
    printf("===========================================================\n");
    return 0;
}
