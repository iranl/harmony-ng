/*
 * The key path reads the device list without copying it: every remote key press, hold repeat and
 * release goes through hw_device_command_send*(), which used to make a deep copy of the whole
 * devices.json tree each time (on the Hub, a 36 KB list = 71 KB in about 2,800 allocations and
 * 8-9 ms). This test loads a list of that size into the real config store (config_store.c,
 * resource_cache.c) and counts the cJSON allocations of the send path, the lookups the queue uses
 * (hw_device_exists, hw_device_command_exists, hw_device_bt_host), and checks that the shared list
 * survives a save made while a send holds it.
 *
 * Runs with the mock hardware library (HARMONY_MOCK_DIR) for the /data paths; the IR encoder and
 * blaster are stubbed. See tests/run_mock_suite.sh for the build line.
 */
#define _GNU_SOURCE
#include "config_store.h"
#include "hw_action.h"
#include "orchestrator.h"
#include "hub_events.h"
#include "cJSON.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } else printf("  PASS %s\n", msg); } while (0)

/* ---- stubs: no IR hardware, no daemon around the store ---- */
orch_state_t orch_get_state(void) { return ORCH_STATE_RUNNING; }
const char *orch_get_current_activity(void) { return "1"; }
void hub_events_emit(hub_event_type_t type, cJSON *payload) { (void)type; cJSON_Delete(payload); }
void state_watch_notify_config_changed(const char *what) { (void)what; }
int ir_encode_pronto(const char *pronto, uint8_t repeats, uint32_t gap, uint8_t **out_bin, size_t *out_len) {
    (void)pronto; (void)repeats; (void)gap;
    *out_bin = (uint8_t *)malloc(16);
    *out_len = 16;
    return 0;
}
int ir_encode_raw(uint32_t freq_hz, const uint32_t *timings_us, size_t count, uint8_t repeats, uint8_t **out_bin, size_t *out_len) {
    (void)freq_hz; (void)timings_us; (void)count; (void)repeats;
    *out_bin = (uint8_t *)malloc(16);
    *out_len = 16;
    return 0;
}
int ir_encode_harmony_keycode(const char *keycode, uint8_t repeats, uint8_t **out_bin, size_t *out_len) {
    (void)keycode; (void)repeats;
    *out_bin = (uint8_t *)malloc(16);
    *out_len = 16;
    return 0;
}
static int s_blasts;
int ir_i2s_blast_blob(const uint8_t *blob, size_t len, uint8_t port_mask) {
    (void)blob; (void)len; (void)port_mask;
    s_blasts++;
    return 0;
}
/* The held-key IR player change adds these two to the transmitter; unused here */
int ir_i2s_blast_blob_part(const uint8_t *blob, size_t blob_len, int part, uint8_t port_mask);
int ir_i2s_blast_blob_part(const uint8_t *blob, size_t blob_len, int part, uint8_t port_mask) { (void)blob; (void)blob_len; (void)part; (void)port_mask; return -1; }
void ir_i2s_tx_hold(int on);
void ir_i2s_tx_hold(int on) { (void)on; }
void ir_free_encoded(uint8_t *bin) { free(bin); }

/* ---- cJSON allocation counter ---- */
static volatile long s_allocs;
static volatile long s_bytes;
static void *count_malloc(size_t n) {
    __sync_fetch_and_add(&s_allocs, 1);
    __sync_fetch_and_add(&s_bytes, (long)n);
    return malloc(n);
}

/* A list like the Hub's: 4 devices, 36 KB as printed by cJSON_Print */
static cJSON *make_devices(int with_marker) {
    cJSON *root = cJSON_CreateObject();
    cJSON *devs = cJSON_AddArrayToObject(root, "devices");
    for (int d = 1; d <= 4; d++) {
        cJSON *dev = cJSON_CreateObject();
        char id[8];
        snprintf(id, sizeof(id), "%d", d);
        cJSON_AddStringToObject(dev, "id", id);
        cJSON_AddStringToObject(dev, "name", d == 1 ? (with_marker ? "Saved TV" : "TV") : "Device");
        cJSON_AddStringToObject(dev, "type", "IR");
        cJSON_AddNumberToObject(dev, "interKeyDelay", 0);
        cJSON_AddNumberToObject(dev, "interDeviceDelay", 0);
        cJSON *cmds = cJSON_AddArrayToObject(dev, "commands");
        for (int c = 0; c < 70; c++) {
            cJSON *cmd = cJSON_CreateObject();
            char name[16];
            snprintf(name, sizeof(name), "Command%02d", c);
            cJSON_AddStringToObject(cmd, "name", name);
            cJSON_AddStringToObject(cmd, "keyCode", "G:Samsung 16 and 20 Bit:(0x0707)(0x02FD)():1");
            cJSON_AddNumberToObject(cmd, "repeats", 3);
            cJSON_AddItemToArray(cmds, cmd);
        }
        cJSON_AddItemToArray(devs, dev);
    }
    return root;
}

static long allocs_of_sends(int n, const char *what) {
    long before = s_allocs;
    for (int i = 0; i < n; i++) {
        char cmd[16];
        snprintf(cmd, sizeof(cmd), "Command%02d", i % 70); /* another key each time: no same-key filter */
        if (strcmp(what, "send") == 0) hw_device_command_send_action("1", 1, cmd, HW_BTN_ACTION_TAP);
        else if (strcmp(what, "release") == 0) hw_device_command_send_action("1", 1, cmd, HW_BTN_ACTION_RELEASE);
        else if (strcmp(what, "exists") == 0) hw_device_exists("1", 1);
        else if (strcmp(what, "command_exists") == 0) hw_device_command_exists("1", 1, cmd);
        else {
            char addr[32];
            hw_device_bt_host("1", addr, sizeof(addr));
        }
    }
    return (s_allocs - before) / n;
}

int main(void) {
    printf("[-] Key path: the device list is shared, not copied per key\n");
    char mock[] = "/tmp/hng_keypath_XXXXXX";
    if (!mkdtemp(mock)) return 1;
    setenv("HARMONY_MOCK_DIR", mock, 1);
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/data", mock);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/data/harmony", mock);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/data/harmony/config", mock);
    mkdir(dir, 0755);

    config_store_init();
    cJSON *devs = make_devices(0);
    char *printed = cJSON_Print(devs);
    size_t list_bytes = printed ? strlen(printed) : 0;
    free(printed);
    CHECK(config_store_save_devices(devs) == 0, "device list saved to the store");

    /* One copy of the list, for scale */
    cJSON_Hooks hooks = { count_malloc, free };
    cJSON_InitHooks(&hooks);
    long a0 = s_allocs, b0 = s_bytes;
    cJSON *copy = config_store_devices_copy();
    long copy_allocs = s_allocs - a0, copy_bytes = s_bytes - b0;
    cJSON_Delete(copy);
    printf("    devices.json: %zu bytes; one copy: %ld allocations, %ld bytes\n", list_bytes, copy_allocs, copy_bytes);

    enum { N = 200 };
    long per_send = allocs_of_sends(N, "send");
    long per_release = allocs_of_sends(N, "release");
    long per_exists = allocs_of_sends(N, "exists");
    long per_cmd_exists = allocs_of_sends(N, "command_exists");
    long per_bt_host = allocs_of_sends(N, "bt_host");
    printf("    cJSON allocations per call: send %ld, release %ld, device_exists %ld, command_exists %ld, bt_host %ld\n",
           per_send, per_release, per_exists, per_cmd_exists, per_bt_host);
    CHECK(s_blasts >= N, "the key presses reached the (stubbed) blaster");
    CHECK(per_send < 10, "a key press does not copy the device list");
    CHECK(per_release < 10, "a key release does not copy the device list");
    CHECK(per_exists < 10 && per_cmd_exists < 10 && per_bt_host < 10, "the queue's device lookups do not copy the list");

    /* A save while a reader holds the list: the reader keeps the old one, the next gets the new */
    CHECK(config_store_snapshots_held() == 0, "every send released the shared list");
    cfg_snap_t *snap = NULL;
    const cJSON *held = config_store_devices_acquire(&snap);
    const cJSON *dev1 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(held, "devices"), 0);
    const char *old_name = cJSON_GetObjectItemCaseSensitive(dev1, "name")->valuestring;
    CHECK(config_store_save_devices(make_devices(1)) == 0, "save while a reader holds the list");
    CHECK(strcmp(old_name, "TV") == 0, "the held list is unchanged after the save");
    cfg_snap_t *snap2 = NULL;
    const cJSON *now = config_store_devices_acquire(&snap2);
    const cJSON *ndev1 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(now, "devices"), 0);
    CHECK(now != held && strcmp(cJSON_GetObjectItemCaseSensitive(ndev1, "name")->valuestring, "Saved TV") == 0,
          "the next reader gets the saved list");
    config_store_release(&snap);
    config_store_release(&snap2);
    CHECK(snap == NULL && snap2 == NULL && config_store_snapshots_held() == 0, "handles cleared, counts back to 0");

    config_store_invalidate_cache(); /* nothing left for LeakSanitizer */
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", mock);
    if (system(cmd) != 0) printf("    (could not remove %s)\n", mock);
    if (g_failures) {
        printf("[-] %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("[+] key path: all checks passed\n");
    return 0;
}
