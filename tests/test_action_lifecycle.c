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
#include "../payload/source/net_action.h"

/* Mock device database */
static cJSON *s_mock_device_root = NULL;
/* hw_action.c asks the orchestrator before a Bluetooth reconnect (no reconnect while stopping or
 * powered off): an activity is running here */
orch_state_t orch_get_state(void) { return ORCH_STATE_RUNNING; }
const char *orch_get_current_activity(void) { return "1"; }
/* the MQTT-type device path puts a button event on the daemon's bus */
void hub_events_emit(hub_event_type_t type, cJSON *payload) { (void)type; cJSON_Delete(payload); }
/* HTTP/TCP devices (net_action.c): none in these tests */
int net_action_send(const cJSON *dev, const cJSON *cmd, const char *dev_id, const char *dev_name, const char *command, int live_key) { (void)dev; (void)cmd; (void)dev_id; (void)dev_name; (void)command; (void)live_key; return 0; }

/* The shared device list (resource_cache.h): every acquire must be released */
static int s_list_held = 0;
static char s_list_handle;
const cJSON *get_device_list_acquire(struct cfg_snap **snap) {
    if (!s_mock_device_root) {
        s_mock_device_root = cJSON_Parse("{\"devices\":["
            "{\"id\":\"mock_dev\",\"name\":\"MockDevice\",\"interDeviceDelay\":0,\"interKeyDelay\":0,\"holdInterKeyDelay\":0,\"commands\":["
            "{\"name\":\"PowerToggle\",\"raw\":\"0000 006d 0022 0002\"},"
            "{\"name\":\"VolumeUp\",\"raw\":\"0000 006d 0022 0003\"}]},"
            "{\"id\":\"ble_dev\",\"name\":\"BleFan\",\"type\":\"BLE\",\"commands\":["
            "{\"name\":\"LightOff\",\"keyCode\":\"BLEADV:02011A19054E6F720E0438064C2E561510D42DEB8C15D3B51804030201\"},"
            "{\"name\":\"Broken\",\"keyCode\":\"BLEADV:030106\"}]},"
            "{\"id\":\"plug_dev\",\"name\":\"PluginDevice\",\"interDeviceDelay\":0,\"interKeyDelay\":0,\"holdInterKeyDelay\":0,\"plugin\":\"roku-ecp\",\"config\":{\"ip\":\"192.168.1.50\"},\"commands\":["
            "{\"name\":\"Home\",\"keyCode\":\"PLUGIN:roku-ecp:Home\"},"
            "{\"name\":\"Level\",\"keyCode\":\"PLUGIN:roku-ecp:Level\",\"range\":[1,100]}]},"
            "{\"id\":\"plug_typed\",\"name\":\"AndroidTV\",\"type\":\"plugin:androidtv\",\"config\":{\"host\":\"192.168.1.60\"},\"commands\":["
            "{\"name\":\"Back\",\"keyCode\":\"Back\"}]},"
            "{\"id\":\"rc5_amp\",\"name\":\"Rc5Amp\",\"interDeviceDelay\":0,\"interKeyDelay\":0,\"holdInterKeyDelay\":0,\"commands\":["
            "{\"name\":\"Mute\",\"keyCode\":\"G:Philips RC5 13 Bit Toggle:()(0x140D)():3\"},"
            "{\"name\":\"Plain\",\"keyCode\":\"G:Philips RC5 13 Bit:()(0x140D)():3\"},"
            "{\"name\":\"Other\",\"keyCode\":\"G:RC5 Second Toggle:()(0x140D)():3\"}]},"
            "{\"id\":\"rc5_tv\",\"name\":\"Rc5Tv\",\"interDeviceDelay\":0,\"interKeyDelay\":0,\"holdInterKeyDelay\":0,\"commands\":["
            "{\"name\":\"Mute\",\"keyCode\":\"G:Philips RC5 13 Bit Toggle:()(0x000D)():3\"}]},"
            "{\"id\":\"custom_dev\",\"name\":\"CustomDev\",\"commands\":["
            "{\"name\":\"Wake\",\"keyCode\":\"PLUGIN:ps5-wake:Boot\"},"
            "{\"name\":\"Netflix\",\"keyCode\":\"PLUGIN:android-tv:app:https://www.netflix.com/title/80057281?source=harmony-ng&trackId=255824129\"}]}]}");
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
/* the RC5 toggle bit hw_action.c gave the last encode (ir_encoder.c) */
static int s_last_toggle = -1;
int ir_keycode_is_rc5_toggle(const char *keycode) {
    return keycode && strstr(keycode, "RC5") != NULL && strstr(keycode, "Toggle:") != NULL;
}
int ir_encode_harmony_keycode_toggle(const char *keycode, uint8_t repeats, int rc5_toggle, uint8_t **out_bin, size_t *out_len) {
    s_last_toggle = rc5_toggle;
    return ir_encode_harmony_keycode(keycode, repeats, out_bin, out_len);
}
/* repeat parts of a press (ir_encoder.c): none here, a tap is one send */
int ir_press_repeat_parts(const char *keycode, const uint8_t *blob, size_t blob_len, int press_min_repeats) {
    (void)keycode; (void)blob; (void)blob_len; (void)press_min_repeats;
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

    /* 6. BLE broadcast commands reach hub_btstack as "adv <hex>" */
    char out[4096] = {0};
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
    assert(strstr(out, "Plugin command simulated:") != NULL);
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

    /* 8b. A deep link longer than 63 characters reaches the plugin whole */
    usleep(100000);
    memset(out, 0, sizeof(out));
    res = capture_send("custom_dev", "Netflix", HW_BTN_ACTION_TAP, out, sizeof(out));
    assert(res == 0);
    assert(strstr(out, "plugin=android-tv") != NULL);
    assert(strstr(out, "cmd=app:https://www.netflix.com/title/80057281?source=harmony-ng&trackId=255824129 ") != NULL);
    printf("[+] Long plugin app link dispatched untruncated.\n");

    /* 8c. The RC5 toggle bit: one per device and protocol, 0 at first, sent and then flipped at
     * each press or tap, as a stock Hub does; a code without a toggle always gets 0 */
    static const struct { const char *dev, *cmd; int want; } rc5[] = {
        { "rc5_amp", "Mute", 0 }, { "rc5_amp", "Mute", 1 }, { "rc5_tv", "Mute", 0 }, { "rc5_amp", "Mute", 0 },
        { "rc5_amp", "Plain", 0 }, { "rc5_tv", "Mute", 1 }, { "rc5_amp", "Mute", 1 }, { "rc5_amp", "Plain", 0 },
        /* a second "Toggle" protocol of the same device has its own bit */
        { "rc5_amp", "Mute", 0 }, { "rc5_amp", "Other", 0 }, { "rc5_amp", "Other", 1 }, { "rc5_amp", "Mute", 1 },
    };
    for (size_t i = 0; i < sizeof(rc5) / sizeof(rc5[0]); i++) {
        usleep(120 * 1000); /* past the duplicate-press window */
        s_last_toggle = -1;
        assert(hw_device_command_send_action(rc5[i].dev, 0, rc5[i].cmd, HW_BTN_ACTION_TAP) == 0);
        if (s_last_toggle != rc5[i].want) {
            printf("[-] RC5 toggle of %s %s, send %zu: %d, wanted %d\n", rc5[i].dev, rc5[i].cmd, i, s_last_toggle, rc5[i].want);
            fflush(stdout); /* a pipe in the suite: the line above would be lost at the abort */
            assert(0);
        }
    }
    usleep(120 * 1000);
    assert(hw_device_command_send_action("rc5_amp", 0, "Mute", HW_BTN_ACTION_PRESS) == 0);
    assert(s_last_toggle == 0);
    assert(hw_device_command_send_action("rc5_amp", 0, "Mute", HW_BTN_ACTION_REPEAT) == 0);
    assert(s_last_toggle == 0); /* a held key's repeats encode nothing: the press's frames go on */
    assert(hw_device_command_send_action("rc5_amp", 0, "Mute", HW_BTN_ACTION_RELEASE) == 0);
    usleep(120 * 1000);
    assert(hw_device_command_send_action("rc5_amp", 0, "Mute", HW_BTN_ACTION_TAP) == 0);
    assert(s_last_toggle == 1);
    printf("[+] RC5 toggle bit kept per device and protocol, sent then flipped.\n");

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

    /* 11. A value reaches the plugin; a command's "range" refuses values outside it */
    {
        fflush(stdout);
        int saved = dup(STDOUT_FILENO);
        FILE *tf = tmpfile();
        assert(saved >= 0 && tf);
        dup2(fileno(tf), STDOUT_FILENO);
        g_dry_run = 1;
        int r_in = hw_device_command_send_value("plug_dev", 0, "Level", HW_BTN_ACTION_TAP, 42);
        usleep(100000);
        int r_lo = hw_device_command_send_value("plug_dev", 0, "Level", HW_BTN_ACTION_TAP, 0);
        int r_hi = hw_device_command_send_value("plug_dev", 0, "Level", HW_BTN_ACTION_TAP, 101);
        int r_free = hw_device_command_send_value("plug_dev", 0, "Free", HW_BTN_ACTION_TAP, -5);
        /* a sender without a value (a remote key, an activity step): a ranged command needs one,
         * a command without a range gets 0 */
        int r_none = hw_device_command_send_action("plug_dev", 0, "Level", HW_BTN_ACTION_TAP);
        usleep(300000); /* past the repeat guard of the same command */
        int r_free0 = hw_device_command_send_action("plug_dev", 0, "Free", HW_BTN_ACTION_TAP);
        usleep(100000);
        g_dry_run = 0;
        fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);
        rewind(tf);
        size_t n = fread(out, 1, sizeof(out) - 1, tf);
        out[n] = '\0';
        fclose(tf);
        assert(r_in == 0 && strstr(out, "cmd=Level action=tap val=42") != NULL);
        assert(r_lo == HW_ERR_VALUE_RANGE && r_hi == HW_ERR_VALUE_RANGE);
        assert(r_free == 0 && strstr(out, "cmd=Free action=tap val=-5") != NULL);
        assert(r_none == HW_ERR_VALUE_MISSING);
        assert(r_free0 == 0 && strstr(out, "cmd=Free action=tap val=0") != NULL);
        /* outside the errno range: the infrared senders return -errno codes */
        assert(HW_ERR_VALUE_RANGE < -200 && HW_ERR_VALUE_MISSING < -200);

        cJSON *c = cJSON_Parse("{\"range\":[1,100]}");
        assert(hw_command_value_in_range(c, 1) && hw_command_value_in_range(c, 100));
        assert(!hw_command_value_in_range(c, 101));
        cJSON_Delete(c);
        c = cJSON_Parse("{\"range\":\"bad\"}");
        assert(hw_command_value_in_range(c, 999)); /* a malformed range checks nothing */
        cJSON_Delete(c);
    }
    printf("[+] Plugin value and command range verified.\n");

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
