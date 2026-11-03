#include "bt_mantra.h"
#include "bt_adv.h"
#include "bt_pairing.h"
#include "mantra_codec.h"

#define WINDOW_DEFAULT_SEC 30
#define WINDOW_MAX_SEC     120
#define LEARN_MAX_IDS      4
#define SEARCH_MS          1000
#define BIND_MS            3000
// Time after the bind starts for it to go out before the window closes
#define BIND_WINDOW_SEC    4

typedef struct {
    uint16_t id;
    int version;
    int8_t rssi;
} learned_t;

typedef enum { MODE_IDLE, MODE_LEARN, MODE_PAIR_SEARCH, MODE_PAIR_BIND } mantra_mode_t;

// Learning listens for the mAntra app's commands and the fan's acknowledgements, which carry the
// fan's id. Pairing broadcasts the app's search until a freshly powered-on fan answers with its
// id, then binds with that id as the app does.
static mantra_mode_t g_mode = MODE_IDLE;
static time_t g_window_end = 0;
static learned_t g_learned[LEARN_MAX_IDS];
static int g_learned_count = 0;
static bool g_paired = false;
static learned_t g_paired_fan;
static uint16_t g_counter = 0;

static void write_results(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    char id[8];
    cJSON_AddBoolToObject(root, "listening", g_mode == MODE_LEARN);
    cJSON_AddBoolToObject(root, "pairing", g_mode == MODE_PAIR_SEARCH || g_mode == MODE_PAIR_BIND);
    if (g_paired) {
        snprintf(id, sizeof(id), "%04X", g_paired_fan.id);
        cJSON_AddStringToObject(root, "paired", id);
        cJSON_AddNumberToObject(root, "pairedVersion", g_paired_fan.version);
    } else {
        cJSON_AddStringToObject(root, "paired", "");
    }
    cJSON *found = cJSON_AddArrayToObject(root, "found");
    for (int i = 0; i < g_learned_count; i++) {
        cJSON *f = cJSON_CreateObject();
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

static void window_end(void) {
    mantra_mode_t was = g_mode;
    g_mode = MODE_IDLE;
    write_results();
    log_msg(was == MODE_LEARN ? "Mantra learn window ended" :
            (g_paired ? "Mantra pairing finished" : "Mantra pairing timed out: no fan answered"));
    // Hand scanning back to the BLE remote state machine
    if (app_state == APP_SCANNING) {
        hog_start_scan();
    } else {
        gap_stop_scan();
    }
}

static void window_start(mantra_mode_t mode, const char *args) {
    int secs = (args && *args) ? atoi(args) : WINDOW_DEFAULT_SEC;
    if (secs <= 0) secs = WINDOW_DEFAULT_SEC;
    if (secs > WINDOW_MAX_SEC) secs = WINDOW_MAX_SEC;

    g_learned_count = 0;
    g_paired = false;
    g_mode = mode;
    g_window_end = time(NULL) + secs;
    if (g_counter == 0) g_counter = (uint16_t)(time(NULL) ^ getpid());
    write_results();

    // Passive scan at full duty so a short burst from the app or the fan is not missed
    gap_set_scan_parameters(0, 48, 48);
    gap_start_scan();

    char msg[64];
    snprintf(msg, sizeof(msg), "Mantra %s window open for %d s", mode == MODE_LEARN ? "learn" : "pairing", secs);
    log_msg(msg);
}

void bt_mantra_learn_cmd(const char *args, bool stop) {
    if (stop) {
        if (g_mode != MODE_IDLE) window_end();
        return;
    }
    window_start(MODE_LEARN, args);
}

void bt_mantra_pair_cmd(const char *args) {
    window_start(MODE_PAIR_SEARCH, args);
}

static void record_learned(uint16_t id, int version, int8_t rssi) {
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
    write_results();

    char msg[64];
    snprintf(msg, sizeof(msg), "Mantra fan id %04X (v%d) heard, rssi=%d", id, version, rssi);
    log_msg(msg);
}

void bt_mantra_handle_adv(const uint8_t *ad_data, uint8_t ad_len, int8_t rssi) {
    uint16_t id = 0;
    int version = 0;
    if (g_mode == MODE_LEARN) {
        if (mantra_decode_adv(ad_data, ad_len, &id, &version)) record_learned(id, version, rssi);
    } else if (g_mode == MODE_PAIR_SEARCH) {
        if (!mantra_decode_search_reply(ad_data, ad_len, &id, &version)) return;
        g_paired = true;
        g_paired_fan.id = id;
        g_paired_fan.version = version;
        g_paired_fan.rssi = rssi;
        g_mode = MODE_PAIR_BIND;
        g_window_end = time(NULL) + BIND_WINDOW_SEC;

        uint8_t adv[MANTRA_ADV_LEN];
        int len = mantra_encode_pair_adv(1, id, ++g_counter, adv);
        bt_adv_enqueue(adv, len, BIND_MS);
        write_results();

        char msg[64];
        snprintf(msg, sizeof(msg), "Mantra fan %04X answered the search; binding", id);
        log_msg(msg);
    }
}

void bt_mantra_learn_poll(void) {
    if (g_mode == MODE_IDLE) return;
    if (time(NULL) >= g_window_end) {
        window_end();
        return;
    }
    // Keep a search on air until the fan answers, as the app does while it shows "Searching"
    if (g_mode == MODE_PAIR_SEARCH && bt_adv_pending() == 0) {
        uint8_t adv[MANTRA_ADV_LEN];
        int len = mantra_encode_pair_adv(0, 0, ++g_counter, adv);
        bt_adv_enqueue(adv, len, SEARCH_MS);
    }
}
