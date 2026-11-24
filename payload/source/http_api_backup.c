#include "http_api_backup.h"
#include "http_util.h"
#include "http_auth.h"
#include "config_store.h"
#include "resource_cache.h"
#include "bt_common.h"
#include "hub_cmd.h"
#include "cJSON.h"
#include "rf_slot_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>

/* --- Import: every part is checked before the first file is written --- */

/* One file of an import, ready to be written */
struct import_file {
    const char *path;
    char *text;
    size_t len;
};

/* Sections that hold a JSON object; the others (network, wifi, auth, hub_id) are text files */
static int import_is_json_section(const char *key) {
    static const char *const json_keys[] = { "devices", "activities", "settings", "mqtt", "bluetooth",
                                             "bt_remote_map", "remote_slot1_map", "remote_slot2_map",
                                             "remote_slot3_map", "remote_slot4_map", "remote_slot5_map",
                                             "remote_slot6_map",
                                             "ui_remote_layout", "users", NULL };
    for (int i = 0; json_keys[i]; i++) {
        if (strcmp(key, json_keys[i]) == 0) return 1;
    }
    return 0;
}

/* The text for one section's file. A JSON section must be an object, sent as an object or as a
 * string that parses completely to one; devices needs a "devices" array and activities an
 * "activities" array. Returns 1 when out is ready, 0 when there is nothing to write (the empty
 * list an export puts in for a file it could not read), -1 when the section is not valid. */
static int import_prepare(const char *key, const char *path, const cJSON *sec, struct import_file *out) {
    out->path = path;
    out->text = NULL;
    out->len = 0;
    if (strcmp(key, "rf_slot_map") == 0) {
        /* six different pipes 1..6, the check hub_rf makes at its start: anything else became
         * the file, and hub_rf then dropped the table (slots and pipes the same), moving the
         * remotes of swapped slots */
        uint8_t pipes[RF_SLOT_TABLE_SLOTS];
        char table[32];
        if (!cJSON_IsString(sec) || !rf_slot_table_parse(sec->valuestring, pipes)) return -1;
        rf_slot_table_format(pipes, table, sizeof(table));
        out->text = strdup(table);
        if (!out->text) return -1;
        out->len = strlen(out->text);
        return 1;
    }
    if (!import_is_json_section(key)) {
        if (cJSON_IsString(sec)) out->text = strdup(sec->valuestring ? sec->valuestring : "");
        else out->text = cJSON_PrintUnformatted(sec);
        if (!out->text) return -1;
        out->len = strlen(out->text);
        return 1;
    }
    cJSON *parsed = NULL;
    const cJSON *obj = sec;
    if (cJSON_IsString(sec)) {
        parsed = sec->valuestring ? cJSON_ParseWithOpts(sec->valuestring, NULL, 1) : NULL;
        obj = parsed;
    }
    int rc = -1;
    const char *list = strcmp(key, "devices") == 0 ? "devices" : strcmp(key, "activities") == 0 ? "activities" : NULL;
    if (list && obj && cJSON_IsArray(obj) && cJSON_GetArraySize(obj) == 0) {
        rc = 0;
    } else if (obj && cJSON_IsObject(obj) && (!list || cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(obj, list)))) {
        if (cJSON_IsString(sec)) out->text = strdup(sec->valuestring);
        else out->text = cJSON_PrintUnformatted(sec);
        if (out->text) {
            out->len = strlen(out->text);
            rc = 1;
        }
    }
    cJSON_Delete(parsed);
    return rc;
}

/* An imported slot table (prepared text) that is not the one hub_rf uses now: a missing file, or
 * one that is no table, means slots and pipes the same, as hub_rf reads it. Only then does
 * hub_rf restart after the import (the restart takes the remotes off the air for seconds). */
static int slot_table_changes(const struct import_file *f) {
    uint8_t live[RF_SLOT_TABLE_SLOTS], imported[RF_SLOT_TABLE_SLOTS];
    rf_slot_table_read(f->path, live);
    rf_slot_table_parse(f->text, imported);
    return memcmp(live, imported, sizeof(live)) != 0;
}

/* Writes every prepared file under a temporary name first (flushed to flash), then renames them
 * into place: when a write fails, every live file is left as it was. 0 when all are in place. */
static int import_commit(const struct import_file *files, int n) {
    char tmp[300];
    int written = 0;
    for (; written < n; written++) {
        snprintf(tmp, sizeof(tmp), "%s.import", files[written].path);
        if (write_file_atomic_sized(tmp, files[written].text, files[written].len) != 0) break;
    }
    if (written < n) {
        for (int i = 0; i <= written && i < n; i++) {
            snprintf(tmp, sizeof(tmp), "%s.import", files[i].path);
            unlink(tmp);
        }
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < n; i++) {
        snprintf(tmp, sizeof(tmp), "%s.import", files[i].path);
        if (rename(tmp, files[i].path) != 0) {
            unlink(tmp);
            rc = -1;
        }
    }
    return rc;
}

static void import_free(struct import_file *files, int n) {
    for (int i = 0; i < n; i++) free(files[i].text);
}

/* The Bluetooth devices that have a host, in the shape of the retired bt-devices.json:
 * config/devices.json is the only device-to-host mapping now, so the "bluetooth" export is
 * built from it. Importing such a file writes bt-devices.json, which config_store_init(),
 * called right after the restore, folds into devices.json (bt_migrate.c). */
static char *bt_links_export_json(size_t *out_len) {
    cJSON *out = cJSON_CreateObject();
    if (!out) return NULL;
    cJSON_AddNumberToObject(out, "version", 1);
    cJSON *list = cJSON_AddArrayToObject(out, "devices");

    size_t dlen = 0;
    char *raw = read_file_text_alloc(config_store_path("config/devices.json"), 4000000, &dlen);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    cJSON *devs = root ? cJSON_GetObjectItemCaseSensitive(root, "devices") : NULL;
    if (list && devs && cJSON_IsArray(devs)) {
        const cJSON *d = NULL;
        cJSON_ArrayForEach(d, devs) {
            const cJSON *ja = cJSON_GetObjectItemCaseSensitive(d, "bdaddr");
            if (!ja) ja = cJSON_GetObjectItemCaseSensitive(d, "btHost");
            if (!ja || !cJSON_IsString(ja) || !ja->valuestring || !ja->valuestring[0]) continue;
            cJSON *e = cJSON_CreateObject();
            if (!e) break;
            const char *keys[] = {"id", "name", "type", NULL};
            for (int k = 0; keys[k]; k++) {
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(d, keys[k]);
                if (v) cJSON_AddItemToObject(e, keys[k], cJSON_Duplicate(v, 1));
            }
            cJSON_AddStringToObject(e, "bdaddr", ja->valuestring);
            cJSON_AddItemToArray(list, e);
        }
    }
    cJSON_Delete(root);

    char *txt = cJSON_Print(out);
    cJSON_Delete(out);
    if (txt && out_len) *out_len = strlen(txt);
    return txt;
}

static int backup_handle(int client_fd, const char *method, const char *clean_path,
                         const char *query, const char *body, const char *req) {
    (void)query;
    (void)req;

    /* Route: GET /export/<target> */
    if (strncmp(clean_path, "/export/", 8) == 0 && strcmp(method, "GET") == 0) {
        const char *tgt = clean_path + 8;
        if (strcmp(tgt, "bundle") == 0) {
            cJSON *bundle = cJSON_CreateObject();
            cJSON_AddNumberToObject(bundle, "version", 1);
            cJSON_AddNumberToObject(bundle, "timestamp", (double)time(NULL));

            static const struct { const char *key; const char *path; } b_files[] = {
                { "devices", "/data/harmony/config/devices.json" },
                { "activities", "/data/harmony/config/activities.json" },
                { "settings", "/data/harmony/config/settings.json" },
                { "mqtt", "/data/harmony/mqtt.json" },
                { "network", "/data/harmony/ethernet.conf" },
                { "wifi", "/etc/wpa_supplicant.conf" },
                /* Legacy file, only there until hub_daemon has migrated it into devices.json
                 * (which the bundle carries anyway); skipped when absent like any other file */
                { "bluetooth", "/data/harmony/bt-devices.json" },
                { "bt_remote_map", "/data/harmony/bt_remote_map.json" },
                { "remote_slot1_map", "/data/harmony/remote_slot1_map.json" },
                { "remote_slot2_map", "/data/harmony/remote_slot2_map.json" },
                { "remote_slot3_map", "/data/harmony/remote_slot3_map.json" },
                { "remote_slot4_map", "/data/harmony/remote_slot4_map.json" },
                { "remote_slot5_map", "/data/harmony/remote_slot5_map.json" },
                { "remote_slot6_map", "/data/harmony/remote_slot6_map.json" },
                /* which CC2544 pipe each slot is (pairing into a chosen slot): without it a
                 * restored Hub has the maps but its remotes in other slots */
                { "rf_slot_map", "/data/harmony/rf_slot_map.txt" },
                { "ui_remote_layout", "/data/harmony/web_remote_layouts.json" },
                { "auth", "/data/harmony/webui_auth.conf" },
                { "users", HARMONY_USERS_JSON },
                { "hub_id", "/data/harmony/hub_id" },
                { NULL, NULL }
            };

            for (int i = 0; b_files[i].key; i++) {
                const char *use_path = b_files[i].path;
                size_t flen = 0;
                char *txt = read_file_text_alloc(use_path, 2000000, &flen);
                uint8_t pipes[RF_SLOT_TABLE_SLOTS];
                if (txt && strcmp(b_files[i].key, "rf_slot_map") == 0 && !rf_slot_table_parse(txt, pipes)) {
                    /* hub_rf ignores such a file (slots and pipes the same, as with none), and the
                     * import refuses it: the bundle leaves it out like a missing file */
                    free(txt);
                    txt = NULL;
                }
                if (txt) {
                    /* text files stay text: cJSON_Parse read "1 2 3 4 5 6" (rf_slot_map) or a
                     * hub_id like "20130507121425-002126" as just their first number */
                    cJSON *parsed = import_is_json_section(b_files[i].key) ? cJSON_Parse(txt) : NULL;
                    if (parsed) {
                        cJSON_AddItemToObject(bundle, b_files[i].key, parsed);
                    } else {
                        cJSON_AddStringToObject(bundle, b_files[i].key, txt);
                    }
                    free(txt);
                } else {
                    if (strcmp(b_files[i].key, "devices") == 0 || strcmp(b_files[i].key, "activities") == 0) {
                        cJSON_AddItemToObject(bundle, b_files[i].key, cJSON_CreateArray());
                    } else if (strcmp(b_files[i].key, "settings") == 0 || strcmp(b_files[i].key, "mqtt") == 0) {
                        cJSON_AddItemToObject(bundle, b_files[i].key, cJSON_CreateObject());
                    }
                }
            }

            FILE *kf = fopen("/data/harmony/btstack_keys.bin", "rb");
            if (!kf) kf = fopen("/data/harmony/btstack_keys.tlv", "rb");
            if (kf) {
                fseek(kf, 0, SEEK_END);
                long ksz = ftell(kf);
                fseek(kf, 0, SEEK_SET);
                if (ksz > 0 && ksz < 200000) {
                    unsigned char *kbuf = (unsigned char *)malloc((size_t)ksz);
                    if (kbuf) {
                        size_t kr = fread(kbuf, 1, (size_t)ksz, kf);
                        char *b64 = b64_encode(kbuf, kr);
                        if (b64) {
                            cJSON_AddStringToObject(bundle, "btstack_keys", b64);
                            free(b64);
                        }
                        free(kbuf);
                    }
                }
                fclose(kf);
            }

            char *json_out = cJSON_Print(bundle);
            cJSON_Delete(bundle);
            if (json_out) {
                size_t jlen = strlen(json_out);
                uint32_t crc = crc32_simple((const unsigned char *)json_out, jlen);
                char header[512];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json; charset=utf-8\r\n"
                    "Content-Disposition: attachment; filename=\"harmony_backup_bundle.json\"\r\n"
                    "X-Checksum-CRC32: %08x\r\n"
                    "Content-Length: %zu\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n",
                    crc, jlen);
                if (http_send_all(client_fd, header, (size_t)hlen) == 0) http_send_all(client_fd, json_out, jlen);
                free(json_out);
                close(client_fd);
                return 1;
            }
        }

        const char *file_to_send = NULL;
        const char *download_name = "backup.json";
        const char *ctype = "application/json; charset=utf-8";
        const char *fallback_content = NULL;
        int bt_export = 0;

        if (strcmp(tgt, "devices") == 0) {
            file_to_send = config_store_path("config/devices.json");
            download_name = "devices.json";
            fallback_content = "[]\n";
        } else if (strcmp(tgt, "activities") == 0) {
            file_to_send = config_store_path("config/activities.json");
            download_name = "activities.json";
            fallback_content = "[]\n";
        } else if (strcmp(tgt, "settings") == 0) {
            file_to_send = config_store_path("config/settings.json");
            download_name = "settings.json";
            fallback_content = "{}\n";
        } else if (strcmp(tgt, "mqtt") == 0) {
            file_to_send = config_store_path("mqtt.json");
            download_name = "mqtt.json";
            fallback_content = "{}\n";
        } else if (strcmp(tgt, "network") == 0 || strcmp(tgt, "ethernet") == 0) {
            file_to_send = config_store_path("ethernet.conf");
            download_name = "ethernet.conf";
            ctype = "text/plain; charset=utf-8";
            fallback_content = "# No static ethernet configured (DHCP active)\n";
        } else if (strcmp(tgt, "wifi") == 0) {
            file_to_send = "/etc/wpa_supplicant.conf";
            download_name = "wpa_supplicant.conf";
            ctype = "text/plain; charset=utf-8";
            fallback_content = "# No custom Wi-Fi configured\n";
        } else if (strcmp(tgt, "bluetooth") == 0) {
            bt_export = 1;
            download_name = "bt-devices.json";
            fallback_content = "{\"devices\":[]}\n";
        } else if (strcmp(tgt, "remote-mapping") == 0 || strcmp(tgt, "bt_remote_map") == 0) {
            file_to_send = config_store_path("bt_remote_map.json");
            download_name = "bt_remote_map.json";
            fallback_content = "{\"remote\":{\"name\":\"Homatics B25\"},\"activities\":{}}\n";
        } else if (strcmp(tgt, "remote_slot1_map") == 0) {
            file_to_send = config_store_path("remote_slot1_map.json");
            download_name = "remote_slot1_map.json";
            fallback_content = "{\"remote\":{\"name\":\"Harmony Slot 1\"},\"activities\":{},\"devices\":{}}\n";
        } else if (strcmp(tgt, "remote_slot2_map") == 0) {
            file_to_send = config_store_path("remote_slot2_map.json");
            download_name = "remote_slot2_map.json";
            fallback_content = "{\"remote\":{\"name\":\"Harmony Slot 2\"},\"activities\":{},\"devices\":{}}\n";
        } else if (strcmp(tgt, "remote_slot3_map") == 0) {
            file_to_send = config_store_path("remote_slot3_map.json");
            download_name = "remote_slot3_map.json";
            fallback_content = "{\"remote\":{\"name\":\"Harmony Slot 3\"},\"activities\":{},\"devices\":{}}\n";
        } else if (strcmp(tgt, "remote_slot4_map") == 0) {
            file_to_send = config_store_path("remote_slot4_map.json");
            download_name = "remote_slot4_map.json";
            fallback_content = "{\"remote\":{\"name\":\"Harmony Slot 4\"},\"activities\":{},\"devices\":{}}\n";
        } else if (strcmp(tgt, "remote_slot5_map") == 0) {
            file_to_send = config_store_path("remote_slot5_map.json");
            download_name = "remote_slot5_map.json";
            fallback_content = "{\"remote\":{\"name\":\"Harmony Slot 5\"},\"activities\":{},\"devices\":{}}\n";
        } else if (strcmp(tgt, "remote_slot6_map") == 0) {
            file_to_send = config_store_path("remote_slot6_map.json");
            download_name = "remote_slot6_map.json";
            fallback_content = "{\"remote\":{\"name\":\"Harmony Slot 6\"},\"activities\":{},\"devices\":{}}\n";
        } else if (strcmp(tgt, "rf_slot_map") == 0) {
            file_to_send = config_store_path("rf_slot_map.txt");
            download_name = "rf_slot_map.txt";
            ctype = "text/plain; charset=utf-8";
            fallback_content = "1 2 3 4 5 6\n"; /* no file: slots and pipes the same */
        } else if (strcmp(tgt, "ui-remote-layout") == 0 || strcmp(tgt, "ui_remote_layout") == 0) {
            file_to_send = config_store_path("web_remote_layouts.json");
            download_name = "web_remote_layouts.json";
            fallback_content = "{}\n";
        } else if (strcmp(tgt, "auth") == 0) {
            file_to_send = config_store_path("webui_auth.conf");
            download_name = "webui_auth.conf";
            ctype = "text/plain; charset=utf-8";
            fallback_content = "# WebUI authentication disabled (default)\n";
        } else if (strcmp(tgt, "users") == 0) {
            file_to_send = HARMONY_USERS_JSON;
            download_name = "users.json";
            fallback_content = "{\"users\":[]}\n";
        }

        if (file_to_send || bt_export) {
            size_t flen = 0;
            char *buf = bt_export ? bt_links_export_json(&flen) : read_file_text_alloc(file_to_send, 2000000, &flen);
            const char *content_ptr = buf;
            if (!content_ptr && fallback_content) {
                content_ptr = fallback_content;
                flen = strlen(fallback_content);
            }
            if (content_ptr) {
                char header[512];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Disposition: attachment; filename=\"%s\"\r\n"
                    "Content-Length: %zu\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n",
                    ctype, download_name, flen);
                if (http_send_all(client_fd, header, (size_t)hlen) == 0) http_send_all(client_fd, content_ptr, flen);
                if (buf) free(buf);
                close(client_fd);
                return 1;
            }
        }
        send_json_error(client_fd, 404, "Export target not found");
        close(client_fd);
        return 1;
    }

    /* Route: POST /import */
    if (strcmp(clean_path, "/import") == 0 && strcmp(method, "POST") == 0) {
        cJSON *params = parse_request_params(body);
        int success = 0;
        /* Every section is checked and turned into its file's text before anything is written:
         * a cut or wrong file used to replace the live configuration and the Hub still answered
         * "Restored successfully" */
        /* every file section of the bundle plus the two Bluetooth key files, with room; a section
         * is refused rather than written past the end (it was 16 for 21 possible files) */
        struct import_file files[28];
        const int files_max = (int)(sizeof(files) / sizeof(files[0]));
        int slot_map_restored = 0; /* the import changes the slot table: hub_rf restarts after it */
        int nfiles = 0;
        const char *refused = NULL; /* the section that is not valid */
        int write_failed = 0;
        int auth_before = 0; /* an import that turns authentication on answers what to check */
        /* the accounts before: an imported name that is new must not get the tokens a deleted
         * account of that name left in the token file */
        struct user_account users_before[MAX_HTTP_USERS];
        int count_before = load_user_accounts(users_before, MAX_HTTP_USERS, &auth_before);
        int tokens_left = 0;
        cJSON *import_check = NULL;
        if (params) {
            cJSON *p_tgt = cJSON_GetObjectItemCaseSensitive(params, "target");
            cJSON *p_pay = cJSON_GetObjectItemCaseSensitive(params, "payload");
            const char *tgt = p_tgt ? (cJSON_IsString(p_tgt) ? p_tgt->valuestring : "") : "";

            cJSON *p_net = cJSON_GetObjectItemCaseSensitive(params, "restore_network");
            int allow_network = (p_net && (cJSON_IsTrue(p_net) || (cJSON_IsNumber(p_net) && p_net->valueint > 0)));

            /* "all": the new web UI's name for the whole backup */
            if (strcmp(tgt, "bundle") == 0 || strcmp(tgt, "all") == 0) {
                cJSON *bundle = NULL;
                if (p_pay && cJSON_IsString(p_pay)) bundle = cJSON_ParseWithOpts(p_pay->valuestring, NULL, 1);
                else if (p_pay && cJSON_IsObject(p_pay)) bundle = p_pay;
                else bundle = params;
                if (!bundle || !cJSON_IsObject(bundle)) refused = "bundle";

                if (!refused) {
                    static const struct { const char *key; const char *path; int is_net; } b_files[] = {
                        { "devices", "/data/harmony/config/devices.json", 0 },
                        { "activities", "/data/harmony/config/activities.json", 0 },
                        { "settings", "/data/harmony/config/settings.json", 0 },
                        { "mqtt", "/data/harmony/mqtt.json", 0 },
                        { "network", "/data/harmony/ethernet.conf", 1 },
                        { "wifi", "/etc/wpa_supplicant.conf", 1 },
                        /* An old backup's bt-devices.json: config_store_init() below folds it
                         * into devices.json (only devices without a host) */
                        { "bluetooth", "/data/harmony/bt-devices.json", 0 },
                        { "bt_remote_map", "/data/harmony/bt_remote_map.json", 0 },
                        { "remote_slot1_map", "/data/harmony/remote_slot1_map.json", 0 },
                        { "remote_slot2_map", "/data/harmony/remote_slot2_map.json", 0 },
                        { "remote_slot3_map", "/data/harmony/remote_slot3_map.json", 0 },
                        { "remote_slot4_map", "/data/harmony/remote_slot4_map.json", 0 },
                        { "remote_slot5_map", "/data/harmony/remote_slot5_map.json", 0 },
                        { "remote_slot6_map", "/data/harmony/remote_slot6_map.json", 0 },
                        { "rf_slot_map", "/data/harmony/rf_slot_map.txt", 0 },
                        { "ui_remote_layout", "/data/harmony/web_remote_layouts.json", 0 },
                        { "auth", "/data/harmony/webui_auth.conf", 0 },
                        { "users", HARMONY_USERS_JSON, 0 },
                        { "hub_id", "/data/harmony/hub_id", 0 },
                        { NULL, NULL, 0 }
                    };

                    for (int i = 0; b_files[i].key && !refused; i++) {
                        if (b_files[i].is_net && !allow_network) {
                            continue;
                        }
                        cJSON *sec = cJSON_GetObjectItemCaseSensitive(bundle, b_files[i].key);
                        if (sec) {
                            if (strcmp(b_files[i].key, "mqtt") == 0 && cJSON_IsObject(sec)) {
                                if (!cJSON_GetObjectItemCaseSensitive(sec, "enabled")) {
                                    cJSON *h = cJSON_GetObjectItemCaseSensitive(sec, "host");
                                    if (!h) {
                                        cJSON *bk = cJSON_GetObjectItemCaseSensitive(sec, "broker");
                                        if (bk) h = cJSON_GetObjectItemCaseSensitive(bk, "host");
                                    }
                                    if (h && cJSON_IsString(h) && h->valuestring && h->valuestring[0]) {
                                        cJSON_AddBoolToObject(sec, "enabled", 1);
                                    }
                                }
                            }
                            if (nfiles >= files_max - 2) { refused = b_files[i].key; break; }
                            /* a bundle exported before text sections stayed text has hub_id as a
                             * number, cut to its first digits: skipped, not restored cut */
                            if (strcmp(b_files[i].key, "hub_id") == 0 && !cJSON_IsString(sec)) continue;
                            int pr = import_prepare(b_files[i].key, b_files[i].path, sec, &files[nfiles]);
                            if (pr < 0) refused = b_files[i].key;
                            else if (pr > 0) {
                                nfiles++;
                                if (strcmp(b_files[i].key, "rf_slot_map") == 0)
                                    slot_map_restored = slot_table_changes(&files[nfiles - 1]);
                            }
                        }
                    }

                    cJSON *j_keys = cJSON_GetObjectItemCaseSensitive(bundle, "btstack_keys");
                    if (!refused && j_keys && cJSON_IsString(j_keys) && j_keys->valuestring[0]) {
                        size_t klen = 0;
                        unsigned char *raw_keys = b64_decode(j_keys->valuestring, &klen);
                        if (!raw_keys || klen == 0 || klen >= 200000) {
                            free(raw_keys);
                            refused = "btstack_keys";
                        } else {
                            /* the same keys under both names; the second copy owns its buffer */
                            files[nfiles++] = (struct import_file){ "/data/harmony/btstack_keys.tlv", (char *)raw_keys, klen };
                            char *copy = (char *)malloc(klen);
                            if (copy) {
                                memcpy(copy, raw_keys, klen);
                                files[nfiles++] = (struct import_file){ "/data/harmony/btstack_keys.bin", copy, klen };
                            } else {
                                refused = "btstack_keys";
                            }
                        }
                    }

                    if (!refused) {
                        if (import_commit(files, nfiles) != 0) {
                            write_failed = 1;
                        } else {
                            sync();
                            invalidate_device_list_cache();
                            config_store_init();
                            notify_rf_push_config();
                            hub_send_btstack_cmd("reload_map");
                            /* hub_rf reads the slot table at its start: it restarts once idle */
                            if (slot_map_restored && hub_rf_request("POST", "/api/rf/restart", "{}") != 202)
                                printf("[-] Import: hub_rf not reached, the slot table applies at its next start\n");
                            struct user_account tmp_u[MAX_HTTP_USERS];
                            int tmp_en = 0;
                            int tmp_c = load_user_accounts(tmp_u, MAX_HTTP_USERS, &tmp_en);
                            if (tmp_c > 0) save_user_accounts(tmp_u, tmp_c, tmp_en);
                            if (http_tokens_remove_new_names(users_before, count_before, tmp_u, tmp_c) != 0) tokens_left = 1;
                            if (!auth_before && tmp_en && tmp_c > 0) {
                                import_check = cJSON_CreateObject();
                                http_auth_add_switch_check(import_check, tmp_u, tmp_c, NULL);
                            }
                            http_auth_drop_all_sessions(); /* accounts replaced: end every session */
                            success = 1;
                        }
                    }
                }
                if (bundle && bundle != p_pay && bundle != params) cJSON_Delete(bundle);
            } else if (tgt[0] && p_pay) {
                const char *file_to_write = NULL;
                const char *key = tgt; /* section name of the target, for the check */
                if (strcmp(tgt, "devices") == 0) { file_to_write = "/data/harmony/config/devices.json"; }
                else if (strcmp(tgt, "activities") == 0) { file_to_write = "/data/harmony/config/activities.json"; }
                else if (strcmp(tgt, "settings") == 0) { file_to_write = "/data/harmony/config/settings.json"; }
                else if (strcmp(tgt, "mqtt") == 0) { file_to_write = "/data/harmony/mqtt.json"; }
                else if (strcmp(tgt, "network") == 0 || strcmp(tgt, "ethernet") == 0) { file_to_write = "/data/harmony/ethernet.conf"; key = "network"; }
                else if (strcmp(tgt, "wifi") == 0) { file_to_write = "/etc/wpa_supplicant.conf"; }
                /* Folded into devices.json by config_store_init() below (bt_migrate.c) */
                else if (strcmp(tgt, "bluetooth") == 0) { file_to_write = "/data/harmony/bt-devices.json"; }
                else if (strcmp(tgt, "remote-mapping") == 0 || strcmp(tgt, "bt_remote_map") == 0) { file_to_write = "/data/harmony/bt_remote_map.json"; key = "bt_remote_map"; }
                else if (strcmp(tgt, "remote_slot1_map") == 0) { file_to_write = "/data/harmony/remote_slot1_map.json"; }
                else if (strcmp(tgt, "remote_slot2_map") == 0) { file_to_write = "/data/harmony/remote_slot2_map.json"; }
                else if (strcmp(tgt, "remote_slot3_map") == 0) { file_to_write = "/data/harmony/remote_slot3_map.json"; }
                else if (strcmp(tgt, "remote_slot4_map") == 0) { file_to_write = "/data/harmony/remote_slot4_map.json"; }
                else if (strcmp(tgt, "remote_slot5_map") == 0) { file_to_write = "/data/harmony/remote_slot5_map.json"; }
                else if (strcmp(tgt, "remote_slot6_map") == 0) { file_to_write = "/data/harmony/remote_slot6_map.json"; }
                else if (strcmp(tgt, "ui-remote-layout") == 0 || strcmp(tgt, "ui_remote_layout") == 0) { file_to_write = "/data/harmony/web_remote_layouts.json"; key = "ui_remote_layout"; }
                else if (strcmp(tgt, "rf_slot_map") == 0) { file_to_write = "/data/harmony/rf_slot_map.txt"; }
                else if (strcmp(tgt, "auth") == 0) { file_to_write = "/data/harmony/webui_auth.conf"; }
                else if (strcmp(tgt, "users") == 0) { file_to_write = HARMONY_USERS_JSON; }

                if (file_to_write) {
                    /* the empty list counts as not valid here: there is nothing else to restore */
                    if (import_prepare(key, file_to_write, p_pay, &files[0]) <= 0) {
                        refused = tgt;
                    } else {
                        nfiles = 1;
                        if (strcmp(tgt, "rf_slot_map") == 0) slot_map_restored = slot_table_changes(&files[0]);
                        if (import_commit(files, nfiles) != 0) write_failed = 1;
                        else success = 1;
                    }
                    if (success) {
                        invalidate_device_list_cache();
                        config_store_init();
                        if (strcmp(tgt, "devices") == 0 || strcmp(tgt, "activities") == 0 ||
                                (strncmp(tgt, "remote_slot", 11) == 0 && strcmp(tgt + 12, "_map") == 0)) {
                            notify_rf_push_config();
                        }
                        if (strcmp(tgt, "remote-mapping") == 0 || strcmp(tgt, "bt_remote_map") == 0) {
                            hub_send_btstack_cmd("reload_map");
                        }
                        /* as for a bundle: hub_rf reads the table at its start */
                        if (slot_map_restored && hub_rf_request("POST", "/api/rf/restart", "{}") != 202)
                            printf("[-] Import: hub_rf not reached, the slot table applies at its next start\n");
                        if (strcmp(tgt, "auth") == 0 || strcmp(tgt, "users") == 0) {
                            struct user_account tmp_u[MAX_HTTP_USERS];
                            int tmp_en = 0;
                            int tmp_c = load_user_accounts(tmp_u, MAX_HTTP_USERS, &tmp_en);
                            if (tmp_c > 0) save_user_accounts(tmp_u, tmp_c, tmp_en);
                            if (http_tokens_remove_new_names(users_before, count_before, tmp_u, tmp_c) != 0) tokens_left = 1;
                            if (!auth_before && tmp_en && tmp_c > 0) {
                                import_check = cJSON_CreateObject();
                                http_auth_add_switch_check(import_check, tmp_u, tmp_c, NULL);
                            }
                            http_auth_drop_all_sessions(); /* accounts replaced: end every session */
                        }
                    }
                }
            }
            import_free(files, nfiles);
            if (refused) {
                char msg[96];
                snprintf(msg, sizeof(msg), "Import refused, nothing was changed: '%s' is not valid", refused);
                cJSON_Delete(params);
                send_json_error(client_fd, 400, msg);
                close(client_fd);
                return 1;
            }
            if (write_failed) {
                cJSON_Delete(params);
                send_json_error(client_fd, 500, "Import failed: could not write the files");
                close(client_fd);
                return 1;
            }
            cJSON_Delete(params);
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        cJSON_AddStringToObject(resp, "message", success ? "Restored successfully" : "Import failed");
        if (tokens_left) {
            /* they stay refused while the token file cannot be changed (see tokens-reset) */
            cJSON_AddStringToObject(resp, "warning", "Old access tokens of a restored account name could not be removed from the token file");
        }
        if (import_check) {
            /* the import turned authentication on: what to check, as the other switches answer */
            cJSON_AddBoolToObject(resp, "auth_enabled", 1);
            cJSON_AddItemToObject(resp, "to_check", cJSON_DetachItemFromObjectCaseSensitive(import_check, "to_check"));
            cJSON_Delete(import_check);
        }
        send_cjson_resp(client_fd, success ? 200 : 400, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    return 0;
}

/* One import or export at a time: each holds the whole configuration in memory several times
 * (request body, parsed tree, printed sections), and several at once could run the Hub out of
 * memory. A second one gets 409. */
static pthread_mutex_t g_backup_busy = PTHREAD_MUTEX_INITIALIZER;

int http_api_backup_handle(int client_fd, const char *method, const char *clean_path,
                           const char *query, const char *body, const char *req) {
    int is_backup = (strncmp(clean_path, "/export/", 8) == 0 && strcmp(method, "GET") == 0) ||
                    (strcmp(clean_path, "/import") == 0 && strcmp(method, "POST") == 0);
    if (!is_backup) return 0;
    if (pthread_mutex_trylock(&g_backup_busy) != 0) {
        send_json_error(client_fd, 409, "Another import or export is running");
        close(client_fd);
        return 1;
    }
    int handled = backup_handle(client_fd, method, clean_path, query, body, req);
    pthread_mutex_unlock(&g_backup_busy);
    return handled;
}
