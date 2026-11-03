#include "bt_mantra.h"
#include "bt_pairing.h"
#include "mantra_codec.h"

#define LEARN_DEFAULT_SEC 30
#define LEARN_MAX_SEC     120
#define LEARN_MAX_IDS     4

typedef struct {
    uint16_t id;
    int version;
    int8_t rssi;
} learned_t;

// Listening for the mAntra lighting app's broadcasts, so a fan already paired with the app
// can be controlled by the Hub with the same id
static bool g_learn_active = false;
static time_t g_learn_end = 0;
static learned_t g_learned[LEARN_MAX_IDS];
static int g_learned_count = 0;

static void write_learn_results(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddBoolToObject(root, "listening", g_learn_active);
    cJSON *found = cJSON_AddArrayToObject(root, "found");
    for (int i = 0; i < g_learned_count; i++) {
        cJSON *f = cJSON_CreateObject();
        char id[8];
        snprintf(id, sizeof(id), "%04X", g_learned[i].id);
        cJSON_AddStringToObject(f, "id", id);
        cJSON_AddNumberToObject(f, "version", g_learned[i].version);
        cJSON_AddNumberToObject(f, "rssi", g_learned[i].rssi);
        cJSON_AddItemToArray(found, f);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return;
    FILE *f = fopen(MANTRA_LEARN_FILE ".tmp", "w");
    if (f) {
        fputs(out, f);
        fputc('\n', f);
        fclose(f);
        rename(MANTRA_LEARN_FILE ".tmp", MANTRA_LEARN_FILE);
    }
    free(out);
}

static void learn_end(void) {
    g_learn_active = false;
    write_learn_results();
    log_msg("Mantra learn window ended");
    // Hand scanning back to the BLE remote state machine
    if (app_state == APP_SCANNING) {
        hog_start_scan();
    } else {
        gap_stop_scan();
    }
}

void bt_mantra_learn_cmd(const char *args, bool stop) {
    if (stop) {
        if (g_learn_active) learn_end();
        return;
    }
    int secs = (args && *args) ? atoi(args) : LEARN_DEFAULT_SEC;
    if (secs <= 0) secs = LEARN_DEFAULT_SEC;
    if (secs > LEARN_MAX_SEC) secs = LEARN_MAX_SEC;

    g_learned_count = 0;
    g_learn_active = true;
    g_learn_end = time(NULL) + secs;
    write_learn_results();

    // Passive scan at full duty so a short burst from the app is not missed
    gap_set_scan_parameters(0, 48, 48);
    gap_start_scan();

    char msg[64];
    snprintf(msg, sizeof(msg), "Mantra learn window open for %d s", secs);
    log_msg(msg);
}

void bt_mantra_handle_adv(const uint8_t *ad_data, uint8_t ad_len, int8_t rssi) {
    if (!g_learn_active) return;
    uint16_t id = 0;
    int version = 0;
    if (!mantra_decode_adv(ad_data, ad_len, &id, &version)) return;

    for (int i = 0; i < g_learned_count; i++) {
        if (g_learned[i].id == id && g_learned[i].version == version) {
            g_learned[i].rssi = rssi;
            return;
        }
    }
    if (g_learned_count == LEARN_MAX_IDS) return;
    g_learned[g_learned_count].id = id;
    g_learned[g_learned_count].version = version;
    g_learned[g_learned_count].rssi = rssi;
    g_learned_count++;
    write_learn_results();

    char msg[64];
    snprintf(msg, sizeof(msg), "Mantra fan id %04X (v%d) heard, rssi=%d", id, version, rssi);
    log_msg(msg);
}

void bt_mantra_learn_poll(void) {
    if (g_learn_active && time(NULL) >= g_learn_end) learn_end();
}
