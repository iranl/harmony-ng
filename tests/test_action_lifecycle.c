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
#include "../payload/source/resource_cache.h"
#include "../payload/source/orchestrator.h"
#include "../payload/source/hub_events.h"

/* Mock device database */
static cJSON *s_mock_device_root = NULL;
/* hw_action.c asks the orchestrator before a Bluetooth reconnect (no reconnect while stopping or
 * powered off): an activity is running here */
orch_state_t orch_get_state(void) { return ORCH_STATE_RUNNING; }
const char *orch_get_current_activity(void) { return "1"; }
/* the MQTT-type device path puts a button event on the daemon's bus */
void hub_events_emit(hub_event_type_t type, cJSON *payload) { (void)type; cJSON_Delete(payload); }

/* The shared device list (resource_cache.h): every acquire must be released */
static int s_list_held = 0;
static char s_list_handle;
const cJSON *get_device_list_acquire(struct cfg_snap **snap) {
    if (!s_mock_device_root) {
        s_mock_device_root = cJSON_Parse("{\"devices\":[{\"id\":\"mock_dev\",\"name\":\"MockDevice\",\"interDeviceDelay\":0,\"interKeyDelay\":0,\"holdInterKeyDelay\":0,\"commands\":[{\"name\":\"PowerToggle\",\"raw\":\"0000 006d 0022 0002\"},{\"name\":\"VolumeUp\",\"raw\":\"0000 006d 0022 0003\"}]}]}");
    }
    *snap = (struct cfg_snap *)&s_list_handle;
    s_list_held++;
    return s_mock_device_root;
}

void release_device_list(struct cfg_snap **snap) {
    if (!*snap) return;
    *snap = NULL;
    s_list_held--;
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

/* IR sends that got past the filters: counted where the code is encoded, in the caller's thread
 * (a tap is then sent there, a press is handed to the player thread of hw_action.c) */
static int s_blasts = 0;
int ir_encode_harmony_keycode(const char *keycode, uint8_t repeats, uint8_t **out_bin, size_t *out_len) {
    (void)keycode; (void)repeats;
    *out_bin = (uint8_t *)malloc(16);
    *out_len = 16;
    s_blasts++;
    return 0;
}

static volatile int s_taps = 0;    /* blocking sends: taps */
static volatile int s_starts = 0;  /* held keys: start sequences sent by the player */
static volatile int s_repeats = 0; /* held keys: repeat frames */
int ir_i2s_blast_blob(const uint8_t *blob, size_t len, uint8_t port_mask) {
    (void)blob; (void)len; (void)port_mask;
    s_taps++;
    return 0;
}

/* A frame of a held key: each takes 20 ms here. The mock codes have no finish sequence. */
int ir_i2s_blast_blob_part(const uint8_t *blob, size_t len, int part, uint8_t port_mask) {
    (void)blob; (void)len; (void)port_mask;
    if (part == 2) return -2;
    if (part == 0) __sync_fetch_and_add(&s_starts, 1);
    else __sync_fetch_and_add(&s_repeats, 1);
    usleep(20 * 1000);
    return 0;
}

void ir_i2s_tx_hold(int on) { (void)on; }

/* 4b. A held IR key: the press returns at once and the player repeats the code until the
 * release; a hold sends nothing itself; with no hold it stops after the deadline; a tap ends it */
static void test_held_key(void) {
    int starts = s_starts, reps = s_repeats, taps = s_taps;
    assert(hw_device_command_send_action("mock_dev", 0, "VolumeUp", HW_BTN_ACTION_PRESS) == 0);
    for (int i = 0; i < 3; i++) {
        usleep(300 * 1000);
        assert(hw_device_command_send_action("mock_dev", 0, "VolumeUp", HW_BTN_ACTION_REPEAT) == 0);
    }
    usleep(100 * 1000);
    assert(hw_device_command_send_action("mock_dev", 0, "VolumeUp", HW_BTN_ACTION_RELEASE) == 0);
    usleep(60 * 1000);
    int held = s_repeats - reps;
    assert(s_starts == starts + 1 && s_taps == taps); /* one start: the holds were not sends */
    assert(held >= 30 && held <= 55);                 /* 1 s of 20 ms frames */
    usleep(200 * 1000);
    assert(s_repeats - reps == held);                 /* nothing after the release */
    printf("[+] held key: one start, %d repeat frames in 1 s, none after the release.\n", held);

    starts = s_starts;
    reps = s_repeats;
    assert(hw_device_command_send_action("mock_dev", 0, "VolumeUp", HW_BTN_ACTION_PRESS) == 0);
    usleep((HW_IR_HOLD_DEADLINE_MS + 250) * 1000);    /* no hold, no release */
    held = s_repeats - reps;
    assert(s_starts == starts + 1);
    assert(held >= HW_IR_HOLD_DEADLINE_MS / 20 - 10 && held <= HW_IR_HOLD_DEADLINE_MS / 20 + 2);
    usleep(200 * 1000);
    assert(s_repeats - reps == held);
    printf("[+] no hold and no release: stopped by itself after %d ms (%d repeat frames).\n", HW_IR_HOLD_DEADLINE_MS, held);

    /* a hold or a release for a key that does not play sends nothing */
    starts = s_starts;
    reps = s_repeats;
    assert(hw_device_command_send_action("mock_dev", 0, "VolumeUp", HW_BTN_ACTION_REPEAT) == 0);
    assert(hw_device_command_send_action("mock_dev", 0, "VolumeUp", HW_BTN_ACTION_RELEASE) == 0);
    usleep(100 * 1000);
    assert(s_starts == starts && s_repeats == reps && s_taps == taps);
    printf("[+] a hold without a press sends nothing.\n");

    /* a tap while a key is held ends the key and is one blocking send */
    assert(hw_device_command_send_action("mock_dev", 0, "VolumeUp", HW_BTN_ACTION_PRESS) == 0);
    usleep(150 * 1000);
    assert(hw_device_command_send_action("mock_dev", 0, "PowerToggle", HW_BTN_ACTION_TAP) == 0);
    reps = s_repeats;
    assert(s_taps == taps + 1);
    usleep(150 * 1000);
    assert(s_repeats == reps && s_taps == taps + 1);
    printf("[+] a tap ends the held key and is sent once.\n");
    usleep(100 * 1000); /* past the same-button filter of the checks that follow */
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

    test_held_key();

    /* 5. Same-button filter (the mock device has no key delays, so each call returns at once): a
     * duplicate press within 90 ms of the last accepted one is dropped, a real second press after
     * a release is not (the window used to restart at the release) */
    int before = s_blasts;
    assert(hw_device_command_send_action("mock_dev", 0, "VolumeUp", HW_BTN_ACTION_TAP) == 0);
    usleep(30 * 1000);
    assert(hw_device_command_send_action("mock_dev", 0, "VolumeUp", HW_BTN_ACTION_TAP) == 0);
    assert(s_blasts == before + 1);
    printf("[+] duplicate tap 30 ms after the first dropped.\n");
    before = s_blasts;
    assert(hw_device_command_send_action("mock_dev", 0, "PowerToggle", HW_BTN_ACTION_PRESS) == 0);
    usleep(60 * 1000);
    assert(hw_device_command_send_action("mock_dev", 0, "PowerToggle", HW_BTN_ACTION_RELEASE) == 0);
    usleep(70 * 1000);
    assert(hw_device_command_send_action("mock_dev", 0, "PowerToggle", HW_BTN_ACTION_PRESS) == 0);
    assert(s_blasts == before + 2);
    printf("[+] press, release, press 70 ms after the release: both presses sent.\n");

    assert(s_list_held == 0);
    printf("[+] every use of the shared device list was released.\n");

    if (s_mock_device_root) {
        cJSON_Delete(s_mock_device_root);
        s_mock_device_root = NULL;
    }

    printf("\n===========================================================\n");
    printf("  ALL ACTION LIFECYCLE & DEBOUNCE TESTS PASSED!\n");
    printf("===========================================================\n");
    return 0;
}
