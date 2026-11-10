/*
 * test_rf_keymap_lock.c - Concurrency stress test verifying thread-safety of
 * get_remote_map_unlocked(), rf_keymap_invalidate_map(), and dispatch_remote_button().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>

#include "rf_keymap.h"
#include "cJSON.h"

volatile bool g_running = true;
uint64_t get_monotonic_ms(void) { return 0; }
int http_post_local(int port, const char *path, const char *body) {
    (void)port; (void)path; (void)body;
    return 200;
}
void mark_remote_slot_active(uint8_t slot) { (void)slot; }
void rf_note_slot_family(uint8_t slot, remote_model_t family) { (void)slot; (void)family; }

static volatile int s_stop = 0;

static void *invalidator_thread(void *arg) {
    (void)arg;
    while (!s_stop) {
        rf_keymap_invalidate_map(1);
        usleep(100);
    }
    return NULL;
}

static void *reader_thread(void *arg) {
    (void)arg;
    char out_dev[64];
    char out_cmd[64];
    for (int i = 0; i < 2000 && !s_stop; i++) {
        /* Read activity button */
        dispatch_remote_button(1, "DirectionUp", "press", NULL);

        /* Read device button */
        dispatch_remote_button(1, "VolumeUp", "press", "dev1");

        /* Read screen button with g_map_lock */
        get_remote_screen_button(1, "act1", 0, out_dev, sizeof(out_dev), out_cmd, sizeof(out_cmd));

        /* Direct unlocked lookup while holding lock */
        pthread_mutex_lock(&g_map_lock);
        cJSON *map = get_remote_map_unlocked(1);
        if (map) {
            cJSON *acts = cJSON_GetObjectItemCaseSensitive(map, "activities");
            if (acts && cJSON_IsObject(acts)) {
                cJSON_GetObjectItemCaseSensitive(acts, "act1");
            }
        }
        pthread_mutex_unlock(&g_map_lock);
    }
    return NULL;
}

int main(void) {
    printf("[*] Starting test_rf_keymap_lock concurrency stress test...\n");

    /* Create dummy remote_slot1_map.json */
    FILE *f = fopen("/data/harmony/remote_slot1_map.json", "w");
    assert(f != NULL);
    const char *sample =
            "{\n"
            "  \"activities\": {\n"
            "    \"act1\": {\n"
            "      \"DirectionUp\": {\"type\":\"command\",\"action\":\"device_cmd\",\"deviceId\":\"dev1\",\"command\":\"Up\"},\n"
            "      \"screen_buttons\": [{\"deviceId\":\"dev1\",\"command\":\"Select\"}]\n"
            "    }\n"
            "  },\n"
            "  \"devices\": {\n"
            "    \"dev1\": {\n"
            "      \"VolumeUp\": {\"type\":\"command\",\"action\":\"device_cmd\",\"command\":\"VolUp\"}\n"
            "    }\n"
            "  }\n"
            "}\n";
        fputs(sample, f);
        fclose(f);

    pthread_t inv_th;
    pthread_t rdr_th1, rdr_th2;

    pthread_create(&inv_th, NULL, invalidator_thread, NULL);
    pthread_create(&rdr_th1, NULL, reader_thread, NULL);
    pthread_create(&rdr_th2, NULL, reader_thread, NULL);

    pthread_join(rdr_th1, NULL);
    pthread_join(rdr_th2, NULL);
    s_stop = 1;
    pthread_join(inv_th, NULL);

    printf("[+] test_rf_keymap_lock passed: no use-after-free or race condition observed\n");
    return 0;
}
