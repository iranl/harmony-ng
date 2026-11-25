/*
 * test_rf_keymap_layer.c - The key layer of a remote slot (rf_keymap.h).
 *
 * A key bound as layer_toggle turns the slot's layer on and sends its "on" command, or off and
 * sends "off"; its release sends the release of that same command. While the layer is on, the
 * keys of map "layers" -> <activity> come before the activity's own. The layer belongs to the
 * activity it was turned on in: another activity, or none, turns it off. The API functions set
 * and read it without sending anything.
 *
 * As in test_rf_keymap_fallback.c, fopen() is wrapped (-Wl,--wrap=fopen) for the map, the
 * current activity and the state of a switch, and the sends are recorded from http_post_local().
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
void rf_note_slot_family(uint8_t slot, remote_model_t family) { (void)slot; (void)family; }
bool is_companion_slot(uint8_t slot) { (void)slot; return false; }

static char s_dir[64];
static char s_map_path[128];
static char s_act_path[128];
static char s_state_path[128];
static char s_sock_path[128];

FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode) {
    if (path && strcmp(path, "/data/harmony/remote_slot2_map.json") == 0) return __real_fopen(s_map_path, mode);
    if (path && (strcmp(path, "/tmp/current_activity") == 0 ||
                 strcmp(path, "/data/harmony/current_activity") == 0)) return __real_fopen(s_act_path, mode);
    if (path && strcmp(path, "/tmp/activity_state") == 0) return __real_fopen(s_state_path, mode);
    if (path && strncmp(path, "/data/", 6) == 0) return NULL;
    return __real_fopen(path, mode);
}

static char s_last_send[256];
static int s_sends;
static int s_poweroffs;

int http_post_local(int port, const char *path, const char *body) {
    (void)port;
    if (path && (strcmp(path, "/api/ir-send") == 0 || strcmp(path, "/api/activity") == 0) && body) {
        snprintf(s_last_send, sizeof(s_last_send), "%s", body);
        s_sends++;
    }
    if (path && strcmp(path, "/api/poweroff") == 0) s_poweroffs++;
    return 200;
}

static void write_file(const char *path, const char *text) {
    FILE *f = __real_fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static int failures;

/* Sends key on slot 2 (press or release); want_cmd NULL = nothing sent */
static void check(const char *what, const char *key, const char *action, const char *want_dev, const char *want_cmd) {
    s_sends = 0;
    s_last_send[0] = '\0';
    dispatch_remote_button(2, key, action, NULL);
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

/* Presses key on slot 2: it must start the activity want_act, once */
static void check_start(const char *what, const char *key, const char *want_act) {
    char want[64];
    snprintf(want, sizeof(want), "{\"activity\":\"%s\"}", want_act);
    s_sends = 0;
    s_last_send[0] = '\0';
    dispatch_remote_button(2, key, "press", NULL);
    int ok = (s_sends == 1 && strcmp(s_last_send, want) == 0);
    printf("[%s] %s (sends=%d %s)\n", ok ? "+" : "-", what, s_sends, s_last_send);
    if (!ok) failures++;
}

static void expect_layer_named(const char *what, int want_on, const char *want_act, const char *want_name) {
    char act[32], name[24];
    int on = rf_keymap_layer_active(2, act, sizeof(act), name, sizeof(name));
    int ok = (on == want_on) && strcmp(act, want_act) == 0 && strcmp(name, want_name) == 0;
    printf("[%s] %s (on=%d activity='%s' name='%s')\n", ok ? "+" : "-", what, on, act, name);
    if (!ok) failures++;
}

static void expect_layer(const char *what, int want_on, const char *want_act) {
    expect_layer_named(what, want_on, want_act, "");
}

#define BTN(dev, cmd) "{\"type\":\"command\",\"action\":\"device_cmd\",\"deviceId\":\"" dev "\",\"command\":\"" cmd "\"}"

int main(void) {
    printf("[*] test_rf_keymap_layer\n");
    snprintf(s_dir, sizeof(s_dir), "/tmp/hng_keymap_layer.XXXXXX");
    if (!mkdtemp(s_dir)) { perror("mkdtemp"); return 1; }
    snprintf(s_map_path, sizeof(s_map_path), "%s/map.json", s_dir);
    snprintf(s_act_path, sizeof(s_act_path), "%s/current_activity", s_dir);
    snprintf(s_state_path, sizeof(s_state_path), "%s/activity_state", s_dir);
    snprintf(s_sock_path, sizeof(s_sock_path), "%s/no_key_channel.sock", s_dir);
    g_rf_key_sock_path = s_sock_path;

    /* Activity 1 sends the arrows to device 2 (a set-top box); its Guide opens a menu on device 3
       (the TV) with Green and closes it with Blue, and while the menu is up the arrows, OK and
       Back go to the TV. Activity 2 has no layer. The named layers: "amp" (a device's menus and
       its power on Off), "quiet" (Off does nothing) and "start" (Off starts activity 2). */
    write_file(s_map_path,
        "{\"activities\":{"
        "\"1\":{\"DirectionUp\":" BTN("2", "Up") ",\"Select\":" BTN("2", "Select") ",\"Dvr\":" BTN("2", "Dvr") ","
               "\"Red\":{\"action\":\"activity_start\",\"activityId\":\"2\"},"
               "\"Guide\":{\"action\":\"layer_toggle\",\"on\":{\"deviceId\":\"3\",\"command\":\"Green\"},"
                                                  "\"off\":{\"deviceId\":\"3\",\"command\":\"Blue\"}}},"
        "\"2\":{\"DirectionUp\":" BTN("4", "Up") "}"
        "},\"layers\":{"
        "\"1\":{\"DirectionUp\":" BTN("3", "DirectionUp") ",\"Select\":" BTN("3", "Select") ","
               "\"Back\":" BTN("3", "Return") "},"
        "\"amp\":{\"DirectionUp\":" BTN("5", "DirectionUp") ",\"Dvr\":{},"
                 "\"PowerOffActivity\":" BTN("5", "PowerToggle") "},"
        "\"quiet\":{\"PowerOffActivity\":{}},"
        "\"start\":{\"PowerOffActivity\":{\"action\":\"activity_start\",\"activityId\":\"2\"}}"
        "},\"devices\":{}}");
    write_file(s_act_path, "1\n");
    rf_keymap_invalidate_map(2);

    check("layer off: the arrow is the activity's", "DirectionUp", "press", "2", "Up");
    check("Guide with the layer off sends its on command", "Guide", "press", "3", "Green");
    check("its release releases the same command", "Guide", "release", "3", "Green");
    expect_layer("the layer is on in activity 1", 1, "1");
    check("layer on: the arrow is the layer's", "DirectionUp", "press", "3", "DirectionUp");
    check("layer on: OK is the layer's", "Select", "press", "3", "Select");
    check("layer on: a key only the layer has", "Back", "press", "3", "Return");
    check("Guide with the layer on sends its off command", "Guide", "press", "3", "Blue");
    check("its release releases the off command", "Guide", "release", "3", "Blue");
    expect_layer("the layer is off", 0, "");
    check("layer off again: the arrow is the activity's", "DirectionUp", "press", "2", "Up");
    check("layer off: a key only the layer has sends nothing", "Back", "press", NULL, NULL);

    /* Another activity turns the layer off; coming back does not turn it on */
    check("Guide turns the layer on", "Guide", "press", "3", "Green");
    write_file(s_act_path, "2\n");
    check("another activity: its own arrow", "DirectionUp", "press", "4", "Up");
    expect_layer("and the layer is off", 0, "");
    write_file(s_act_path, "1\n");
    check("back in activity 1 the layer stays off", "DirectionUp", "press", "2", "Up");

    /* A named layer (a device's menus): its keys, a key it binds to nothing sends nothing, and
       Guide turns it off with the off command */
    rf_keymap_layer_set(2, 1, "amp");
    expect_layer_named("API on: the amp's layer", 1, "1", "amp");
    check("the amp's layer: its arrow", "DirectionUp", "press", "5", "DirectionUp");
    check("the amp's layer: a key bound to nothing sends nothing", "Dvr", "press", NULL, NULL);
    check("the amp's layer: a key it lacks is the activity's", "Select", "press", "2", "Select");
    /* Off is the amp's power while its layer binds it, and stops the activity otherwise */
    s_poweroffs = 0;
    check("the amp's layer: Off is the amp's power", "PowerOffActivity", "press", "5", "PowerToggle");
    check("and its release is that key's", "PowerOffActivity", "release", "5", "PowerToggle");
    printf("[%s] the activity was not stopped (poweroffs=%d)\n", s_poweroffs == 0 ? "+" : "-", s_poweroffs);
    if (s_poweroffs != 0) failures++;
    expect_layer_named("and the amp's layer is still on", 1, "1", "amp");
    check("Guide with the amp's layer on sends the off command", "Guide", "press", "3", "Blue");
    expect_layer("and every layer is off", 0, "");
    check("layer off: Off sends no key", "PowerOffActivity", "press", NULL, NULL);
    printf("[%s] and stops the activity (poweroffs=%d)\n", s_poweroffs == 1 ? "+" : "-", s_poweroffs);
    if (s_poweroffs != 1) failures++;
    check("Guide's own layer does not bind Off", "Guide", "press", "3", "Green");
    check("so Off sends no key there", "PowerOffActivity", "press", NULL, NULL);
    printf("[%s] and stops the activity (poweroffs=%d)\n", s_poweroffs == 2 ? "+" : "-", s_poweroffs);
    if (s_poweroffs != 2) failures++;
    expect_layer("and with it the layer is off: the activity stops", 0, "");

    /* Off while the remote browses a device stops the activity, whatever the layer binds */
    rf_keymap_layer_set(2, 1, "amp");
    s_sends = 0;
    dispatch_remote_button(2, "PowerOffActivity", "press", "5");
    printf("[%s] Off while browsing a device stops the activity (poweroffs=%d, sends=%d)\n",
           (s_poweroffs == 3 && s_sends == 0) ? "+" : "-", s_poweroffs, s_sends);
    if (s_poweroffs != 3 || s_sends != 0) failures++;
    expect_layer("and the layer is off", 0, "");

    /* A held Guide turns the layer over once: its holds repeat the command of the press */
    check("Guide pressed: on", "Guide", "press", "3", "Green");
    check("Guide held: the same command", "Guide", "hold", "3", "Green");
    check("Guide held on: still the same", "Guide", "hold", "3", "Green");
    expect_layer("the layer is on, not turned over by the holds", 1, "1");
    check("Guide released", "Guide", "release", "3", "Green");
    expect_layer("and still on after the release", 1, "1");

    /* A key's release goes where its press went, also once the layer is off */
    check("arrow pressed with the layer on", "DirectionUp", "press", "3", "DirectionUp");
    rf_keymap_layer_set(2, 0, NULL);
    check("its hold after the layer went off", "DirectionUp", "hold", "3", "DirectionUp");
    check("its release after the layer went off", "DirectionUp", "release", "3", "DirectionUp");
    check("the next press is the activity's", "DirectionUp", "press", "2", "Up");
    rf_keymap_layer_set(2, 1, NULL);
    check("its release after the layer came on", "DirectionUp", "release", "2", "Up");
    rf_keymap_layer_set(2, 1, "amp");
    s_poweroffs = 0;
    check("Off pressed with the amp's layer on", "PowerOffActivity", "press", "5", "PowerToggle");
    rf_keymap_layer_set(2, 0, NULL);
    check("Off held after the layer went off is still the amp's", "PowerOffActivity", "hold", "5", "PowerToggle");
    check("and so is its release", "PowerOffActivity", "release", "5", "PowerToggle");
    printf("[%s] the activity was not stopped by that press (poweroffs=%d)\n", s_poweroffs == 0 ? "+" : "-", s_poweroffs);
    if (s_poweroffs != 0) failures++;

    /* Off held while the layer binds it to nothing: no press, hold or release stops the activity,
       also once the layer is off */
    rf_keymap_layer_set(2, 1, "quiet");
    s_poweroffs = 0;
    check("Off bound to nothing: the press sends nothing", "PowerOffActivity", "press", NULL, NULL);
    check("nor its hold", "PowerOffActivity", "hold", NULL, NULL);
    rf_keymap_layer_set(2, 0, NULL);
    check("nor a hold after the layer went off", "PowerOffActivity", "hold", NULL, NULL);
    check("nor its release", "PowerOffActivity", "release", NULL, NULL);
    printf("[%s] and the activity was not stopped (poweroffs=%d)\n", s_poweroffs == 0 ? "+" : "-", s_poweroffs);
    if (s_poweroffs != 0) failures++;
    check("the next Off press, the layer off, sends no key", "PowerOffActivity", "press", NULL, NULL);
    printf("[%s] and stops the activity (poweroffs=%d)\n", s_poweroffs == 1 ? "+" : "-", s_poweroffs);
    if (s_poweroffs != 1) failures++;
    /* Off bound to an action that is no device command: its hold is that press's too */
    rf_keymap_layer_set(2, 1, "start");
    s_poweroffs = 0;
    check_start("Off bound to an activity start: started once", "PowerOffActivity", "2");
    check("its hold starts nothing again", "PowerOffActivity", "hold", NULL, NULL);
    check("nor its release", "PowerOffActivity", "release", NULL, NULL);
    printf("[%s] and the activity was not stopped (poweroffs=%d)\n", s_poweroffs == 0 ? "+" : "-", s_poweroffs);
    if (s_poweroffs != 0) failures++;
    rf_keymap_layer_set(2, 0, NULL);
    /* An Off press the layer took and whose release never came is over with the next press */
    rf_keymap_layer_set(2, 1, "amp");
    check("Off pressed with the amp's layer on, never released", "PowerOffActivity", "press", "5", "PowerToggle");
    rf_keymap_layer_set(2, 0, NULL);
    s_poweroffs = 0;
    check("Off pressed again with the layer off sends no key", "PowerOffActivity", "press", NULL, NULL);
    check("nor its hold: the earlier press is over", "PowerOffActivity", "hold", NULL, NULL);
    printf("[%s] both stop the activity (poweroffs=%d)\n", s_poweroffs == 2 ? "+" : "-", s_poweroffs);
    if (s_poweroffs != 2) failures++;

    /* A key whose press sent nothing: its holds and its release send nothing, whatever the layer
       or the activity binds by then */
    rf_keymap_layer_set(2, 1, "amp");
    check("Dvr pressed with the amp's layer on: bound to nothing", "Dvr", "press", NULL, NULL);
    rf_keymap_layer_set(2, 0, NULL);
    check("its hold after the layer went off is not the activity's", "Dvr", "hold", NULL, NULL);
    check("nor its release", "Dvr", "release", NULL, NULL);
    check("the next press is the activity's", "Dvr", "press", "2", "Dvr");
    check("released", "Dvr", "release", "2", "Dvr");
    check("a key nothing binds", "Back", "press", NULL, NULL);
    rf_keymap_layer_set(2, 1, NULL);
    check("its hold after a layer that binds it came on sends nothing", "Back", "hold", NULL, NULL);
    check("nor its release", "Back", "release", NULL, NULL);
    rf_keymap_layer_set(2, 0, NULL);
    /* A key that starts an activity starts it once, however long it is held */
    check_start("Red starts activity 2", "Red", "2");
    check("held, it starts nothing again", "Red", "hold", NULL, NULL);
    check("released", "Red", "release", NULL, NULL);

    /* While an activity is being started or stopped no layer comes on: the switch turned the
       layers off when it began (rf_sync.c), also a switch to the activity that already runs */
    write_file(s_state_path, "starting\n");
    check("during a switch Guide sends nothing", "Guide", "press", NULL, NULL);
    check("nor its release", "Guide", "release", NULL, NULL);
    expect_layer("and turns no layer on", 0, "");
    int during = rf_keymap_layer_set(2, 1, "amp");
    printf("[%s] the API takes on=1 during a switch (rc=%d)\n", during == 0 ? "+" : "-", during);
    if (during != 0) failures++;
    expect_layer("and leaves the layer off", 0, "");
    write_file(s_state_path, "stopping\n");
    rf_keymap_layer_set(2, 1, NULL);
    expect_layer("the same while the activity stops", 0, "");
    write_file(s_state_path, "running\n");
    rf_keymap_layer_set(2, 1, "amp");
    expect_layer_named("after the switch the layer comes on", 1, "1", "amp");
    write_file(s_state_path, "starting\n");
    check("a layer still on during a switch: Guide turns it off", "Guide", "press", "3", "Blue");
    check("released", "Guide", "release", "3", "Blue");
    expect_layer("and it is off", 0, "");
    write_file(s_state_path, "running\n");

    /* The activity changing turns every layer off (rf_sync.c calls this), so a layer does not
       come back with the same activity started again */
    rf_keymap_layer_set(2, 1, "amp");
    rf_keymap_layers_off();
    expect_layer("layers off when the activity changes", 0, "");
    check("and the arrow is the activity's", "DirectionUp", "press", "2", "Up");
    check("released", "DirectionUp", "release", "2", "Up");

    /* Names: a layer the map lacks, and digits only (an activity's own layer), are refused */
    int missing = rf_keymap_layer_set(2, 1, "typo");
    int digits = rf_keymap_layer_set(2, 1, "2");
    printf("[%s] a layer the map lacks and a name of digits are refused (rc=%d, %d)\n",
           (missing == -1 && digits == -1) ? "+" : "-", missing, digits);
    if (missing != -1 || digits != -1) failures++;
    expect_layer("and no layer is on", 0, "");
    check("Guide again turns the activity's own layer on", "Guide", "press", "3", "Green");
    expect_layer("the activity's own layer", 1, "1");
    check("its arrow is the menu layer's", "DirectionUp", "press", "3", "DirectionUp");
    rf_keymap_layer_set(2, 1, "amp");
    check("the API switches it to the amp's layer", "DirectionUp", "press", "5", "DirectionUp");
    int bad = rf_keymap_layer_set(2, 1, "no spaces");
    printf("[%s] a bad name is refused (rc=%d)\n", bad == -1 ? "+" : "-", bad);
    if (bad != -1) failures++;
    expect_layer_named("and the layer is as it was", 1, "1", "amp");
    rf_keymap_layer_set(2, 0, NULL);

    /* The API sets it without sending, and only while an activity runs */
    rf_keymap_layer_set(2, 1, NULL);
    expect_layer("API on in activity 1", 1, "1");
    check("API on: the arrow is the layer's", "DirectionUp", "press", "3", "DirectionUp");
    rf_keymap_layer_set(2, 0, NULL);
    expect_layer("API off", 0, "");
    write_file(s_act_path, "-1\n");
    rf_keymap_layer_set(2, 1, NULL);
    expect_layer("API on with no activity running stays off", 0, "");
    /* Nothing runs: a layer_toggle key reached through the map's fallback has no layer to turn
       on, and sends nothing */
    write_file(s_map_path,
        "{\"activities\":{\"-1\":{\"Guide\":{\"action\":\"layer_toggle\",\"on\":{\"deviceId\":\"3\",\"command\":\"Green\"},"
        "\"off\":{\"deviceId\":\"3\",\"command\":\"Blue\"}}}},\"layers\":{},\"devices\":{}}");
    rf_keymap_invalidate_map(2);
    check("no activity: a layer_toggle key sends nothing", "Guide", "press", NULL, NULL);
    check("nor its release", "Guide", "release", NULL, NULL);
    expect_layer("and no layer is on", 0, "");
    int unknown = rf_keymap_layer_set(0, 1, NULL) + rf_keymap_layer_set(200, 1, NULL);
    printf("[%s] an unknown slot is refused (rc sum=%d)\n", unknown == -2 ? "+" : "-", unknown);
    if (unknown != -2) failures++;
    expect_layer("and changes nothing", 0, "");

    unlink(s_map_path);
    unlink(s_act_path);
    unlink(s_state_path);
    rmdir(s_dir);
    if (failures) {
        printf("[-] test_rf_keymap_layer: %d failure(s)\n", failures);
        return 1;
    }
    printf("[+] test_rf_keymap_layer passed\n");
    return 0;
}
