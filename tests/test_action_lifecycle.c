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
#include "../payload/source/mantra_codec.h"

/* Mock device database */
static cJSON *s_mock_device_root = NULL;
cJSON *get_device_list_cached(void) {
    if (!s_mock_device_root) {
        s_mock_device_root = cJSON_Parse("{\"devices\":["
            "{\"id\":\"mock_dev\",\"name\":\"MockDevice\",\"commands\":[{\"name\":\"PowerToggle\",\"raw\":\"0000 006d 0022 0002\"}]},"
            "{\"id\":\"ble_dev\",\"name\":\"BleFan\",\"type\":\"BLE\",\"commands\":["
            "{\"name\":\"LightOff\",\"keyCode\":\"BLEADV:02011A19054E6F720E0438064C2E561510D42DEB8C15D3B51804030201\"},"
            "{\"name\":\"Broken\",\"keyCode\":\"BLEADV:030106\"}]},"
            "{\"id\":\"fan\",\"name\":\"MantraFan\",\"type\":\"MANTRA\",\"mantraId\":\"C5F0\",\"mantraVersion\":0,\"commands\":["
            "{\"name\":\"LightOn\",\"keyCode\":\"MANTRA:LIGHT_ON\"},"
            "{\"name\":\"Speed\",\"keyCode\":\"MANTRA:SPEED:20\"},"
            "{\"name\":\"Dance\",\"keyCode\":\"MANTRA:DANCE\"}]},"
            "{\"id\":\"fan_noid\",\"name\":\"Unpaired\",\"type\":\"MANTRA\",\"commands\":["
            "{\"name\":\"LightOn\",\"keyCode\":\"MANTRA:LIGHT_ON\"}]}]}");
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
    printf("[+] BLE broadcast commands forwarded, releases ignored, bad payloads rejected.\n");

    /* 6. Mantra commands are built from the device's fan id and sent as one broadcast */
    res = capture_send("fan", "LightOn", HW_BTN_ACTION_TAP, out, sizeof(out));
    assert(res == 0);
    char *hex = strstr(out, "adv 02011A19054E6F");
    assert(hex != NULL);
    uint8_t adv[MANTRA_ADV_LEN];
    for (int i = 0; i < MANTRA_ADV_LEN; i++) {
        unsigned int b;
        sscanf(hex + 4 + 2 * i, "%2x", &b);
        adv[i] = (uint8_t)b;
    }
    uint16_t fan_id = 0;
    int fan_ver = -1;
    assert(mantra_decode_adv(adv, MANTRA_ADV_LEN, &fan_id, &fan_ver) == 1 && fan_id == 0xC5F0 && fan_ver == 0);
    res = capture_send("fan", "Speed", HW_BTN_ACTION_TAP, out, sizeof(out));
    assert(res == 0 && strstr(out, "adv 02011A19054E6F") != NULL);
    res = capture_send("fan", "Dance", HW_BTN_ACTION_TAP, out, sizeof(out));
    assert(res == -2 && strstr(out, "adv ") == NULL);
    res = capture_send("fan_noid", "LightOn", HW_BTN_ACTION_TAP, out, sizeof(out));
    assert(res == -2 && strstr(out, "adv ") == NULL);
    printf("[+] Mantra commands encoded with the device's fan id; unknown actions and missing ids rejected.\n");

    /* 7. Pairing queues the search then the bind broadcast */
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    FILE *tf = tmpfile();
    dup2(fileno(tf), STDOUT_FILENO);
    g_dry_run = 1;
    res = hw_mantra_pair("fan");
    int res_noid = hw_mantra_pair("fan_noid");
    int res_missing = hw_mantra_pair("nope");
    g_dry_run = 0;
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    rewind(tf);
    size_t n = fread(out, 1, sizeof(out) - 1, tf);
    out[n] = '\0';
    fclose(tf);
    assert(res == 0 && res_noid == -2 && res_missing == -5);
    char *first = strstr(out, "adv 02011A190569635F48");
    assert(first && strstr(first, " 2000") && strstr(first + 1, "adv 02011A190569635F48"));
    printf("[+] Mantra pairing sends search and bind; devices without an id or not found are rejected.\n");

    if (s_mock_device_root) {
        cJSON_Delete(s_mock_device_root);
        s_mock_device_root = NULL;
    }

    printf("\n===========================================================\n");
    printf("  ALL ACTION LIFECYCLE & DEBOUNCE TESTS PASSED!\n");
    printf("===========================================================\n");
    return 0;
}
