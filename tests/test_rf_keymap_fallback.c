/*
 * test_rf_keymap_fallback.c - Which activity's mapping a remote key uses when the current
 * activity does not map it (or no activity runs): the map's "-1" object, then "default", then
 * the only activity of a map that has exactly one. With several activities and neither "-1" nor
 * "default", the key is not sent (it used to take the first activity in the file).
 *
 * The map and the current activity are private files: fopen() is wrapped
 * (-Wl,--wrap=fopen) and redirected for the three paths rf_keymap.c reads. No key channel
 * listens on the test's socket path, so a mapped command goes out over http_post_local(),
 * which the test records.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#include "rf_keymap.h"

volatile bool g_running = true;
uint64_t get_monotonic_ms(void) { return 0; }
void mark_remote_slot_active(uint8_t slot) { (void)slot; }
bool is_companion_slot(uint8_t slot) { (void)slot; return false; }
void rf_note_slot_family(uint8_t slot, remote_model_t family) { (void)slot; (void)family; }

static char s_dir[64];
static char s_map_path[128];
static char s_act_path[128];
static char s_sock_path[128];

FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode) {
    if (path && strcmp(path, "/data/harmony/remote_slot1_map.json") == 0) return __real_fopen(s_map_path, mode);
    if (path && (strcmp(path, "/tmp/current_activity") == 0 ||
                 strcmp(path, "/data/harmony/current_activity") == 0)) return __real_fopen(s_act_path, mode);
    if (path && strncmp(path, "/data/", 6) == 0) return NULL; /* nothing else from the device */
    return __real_fopen(path, mode);
}

static char s_last_send[256];
static int s_sends;

int http_post_local(int port, const char *path, const char *body) {
    (void)port;
    if (path && (strcmp(path, "/api/ir-send") == 0 || strcmp(path, "/api/activity") == 0 ||
                 strcmp(path, "/api/activity/help") == 0) && body) {
        snprintf(s_last_send, sizeof(s_last_send), "%s", body);
        s_sends++;
    }
    return 200;
}

static void write_file(const char *path, const char *text) {
    FILE *f = __real_fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static int failures;

/* Presses key on slot 1 with the given map and current activity. want_cmd NULL = nothing sent. */
static void check(const char *what, const char *map, const char *current, const char *key,
                  const char *want_dev, const char *want_cmd) {
    write_file(s_map_path, map);
    write_file(s_act_path, current);
    rf_keymap_invalidate_map(1);
    s_sends = 0;
    s_last_send[0] = '\0';
    dispatch_remote_button(1, key, "press", NULL);

    int ok;
    if (!want_cmd) {
        ok = (s_sends == 0);
    } else {
        char want[160];
        snprintf(want, sizeof(want), "\"deviceId\":\"%s\",\"command\":\"%s\"", want_dev, want_cmd);
        ok = (s_sends == 1 && strstr(s_last_send, want) != NULL);
    }
    printf("[%s] %s (sends=%d %s)\n", ok ? "+" : "-", what, s_sends, s_last_send);
    if (!ok) failures++;
}

#define BTN(dev, cmd) "{\"type\":\"command\",\"action\":\"device_cmd\",\"deviceId\":\"" dev "\",\"command\":\"" cmd "\"}"

int main(void) {
    printf("[*] test_rf_keymap_fallback\n");
    snprintf(s_dir, sizeof(s_dir), "/tmp/hng_keymap_fb.XXXXXX");
    if (!mkdtemp(s_dir)) { perror("mkdtemp"); return 1; }
    snprintf(s_map_path, sizeof(s_map_path), "%s/map.json", s_dir);
    snprintf(s_act_path, sizeof(s_act_path), "%s/current_activity", s_dir);
    snprintf(s_sock_path, sizeof(s_sock_path), "%s/no_key_channel.sock", s_dir);
    g_rf_key_sock_path = s_sock_path;

    /* Two activities, no "-1" or "default": a key only the first one maps */
    const char *two_acts =
        "{\"activities\":{"
        "\"10\":{\"VolumeUp\":" BTN("1", "VolUp") "},"
        "\"20\":{\"DirectionUp\":" BTN("2", "Up") "}"
        "},\"devices\":{}}";
    check("own mapping of the current activity", two_acts, "20\n", "DirectionUp", "2", "Up");
    check("several activities, key unmapped in the current one: not sent", two_acts, "20\n", "VolumeUp", NULL, NULL);
    check("several activities, nothing running: not sent", two_acts, "-1\n", "VolumeUp", NULL, NULL);

    /* "-1" and "default" still come first */
    const char *with_off =
        "{\"activities\":{"
        "\"10\":{\"VolumeUp\":" BTN("1", "VolUp") "},"
        "\"20\":{\"DirectionUp\":" BTN("2", "Up") "},"
        "\"-1\":{\"VolumeUp\":" BTN("3", "OffVolUp") "}"
        "}}";
    check("\"-1\" object used for an unmapped key", with_off, "20\n", "VolumeUp", "3", "OffVolUp");
    check("\"-1\" object used with nothing running", with_off, "-1\n", "VolumeUp", "3", "OffVolUp");
    const char *with_default =
        "{\"activities\":{"
        "\"10\":{\"VolumeUp\":" BTN("1", "VolUp") "},"
        "\"20\":{\"DirectionUp\":" BTN("2", "Up") "},"
        "\"default\":{\"VolumeUp\":" BTN("4", "DefVolUp") "}"
        "}}";
    check("\"default\" object used for an unmapped key", with_default, "20\n", "VolumeUp", "4", "DefVolUp");

    /* An activity key bound only in one activity's entry (a Companion's WatchTV key): with
       several activities it no longer starts anything from Off */
    const char *start_key =
        "{\"activities\":{"
        "\"10\":{\"WatchTVActivity\":{\"action\":\"activity_start\",\"activityId\":\"10\"}},"
        "\"20\":{\"DirectionUp\":" BTN("2", "Up") "}"
        "}}";
    write_file(s_map_path, start_key);
    write_file(s_act_path, "-1\n");
    rf_keymap_invalidate_map(1);
    s_sends = 0;
    dispatch_remote_button(1, "WatchTVActivity", "press", NULL);
    printf("[%s] activity key bound in one of several activities, from Off: nothing started (sends=%d)\n",
           s_sends == 0 ? "+" : "-", s_sends);
    if (s_sends != 0) failures++;
    const char *start_key_off =
        "{\"activities\":{"
        "\"10\":{},\"20\":{},"
        "\"-1\":{\"WatchTVActivity\":{\"action\":\"activity_start\",\"activityId\":\"10\"}}"
        "}}";
    write_file(s_map_path, start_key_off);
    rf_keymap_invalidate_map(1);
    s_sends = 0;
    s_last_send[0] = '\0';
    dispatch_remote_button(1, "WatchTVActivity", "press", NULL);
    bool started = (s_sends == 1 && strstr(s_last_send, "\"activity\":\"10\"") != NULL);
    printf("[%s] the same key in the \"-1\" entry starts the activity from Off (%s)\n", started ? "+" : "-", s_last_send);
    if (!started) failures++;

    /* An empty entry (an activity whose last key was removed) is not an activity entry */
    const char *one_and_empty =
        "{\"activities\":{\"10\":{\"VolumeUp\":" BTN("1", "VolUp") "},\"20\":{}}}";
    check("one activity plus an empty entry: the activity's mapping", one_and_empty, "20\n", "VolumeUp", "1", "VolUp");
    check("one activity plus an empty entry, nothing running: its mapping", one_and_empty, "-1\n", "VolumeUp", "1", "VolUp");

    /* A map with one activity keeps the old fallback */
    const char *one_act =
        "{\"activities\":{\"10\":{\"VolumeUp\":" BTN("1", "VolUp") "}},\"devices\":{\"1\":{}}}";
    check("single activity, nothing running: its mapping", one_act, "-1\n", "VolumeUp", "1", "VolUp");
    check("single activity, other current activity: its mapping", one_act, "77\n", "VolumeUp", "1", "VolUp");

    /* A map without an "activities" object: "devices" is not an activity */
    const char *flat_one =
        "{\"10\":{\"VolumeUp\":" BTN("1", "VolUp") "},\"devices\":{\"1\":{}}}";
    check("flat map, one activity next to \"devices\": its mapping", flat_one, "-1\n", "VolumeUp", "1", "VolUp");
    const char *flat_two =
        "{\"10\":{\"VolumeUp\":" BTN("1", "VolUp") "},\"20\":{\"DirectionUp\":" BTN("2", "Up") "},\"devices\":{\"1\":{}}}";
    check("flat map, two activities: not sent", flat_two, "-1\n", "VolumeUp", NULL, NULL);

    /* A key mapped to Help (a remote without the Help screen sends the raw key) */
    write_file(s_map_path, "{\"activities\":{\"10\":{\"Info\":{\"type\":\"help\"}},\"20\":{}}}");
    write_file(s_act_path, "10\n");
    rf_keymap_invalidate_map(1);
    s_sends = 0;
    dispatch_remote_button(1, "Info", "press", NULL);
    printf("[%s] key mapped to Help: Help runs for the activity (sends=%d)\n", s_sends == 1 ? "+" : "-", s_sends);
    if (s_sends != 1) failures++;

    unlink(s_map_path);
    unlink(s_act_path);
    rmdir(s_dir);
    if (failures) {
        printf("[-] test_rf_keymap_fallback: %d check(s) failed\n", failures);
        return 1;
    }
    printf("[+] test_rf_keymap_fallback passed\n");
    return 0;
}
