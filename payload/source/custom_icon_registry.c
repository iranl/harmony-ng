#define _GNU_SOURCE
#include "custom_icon_registry.h"
#include "rf_sync.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

#define REGISTRY_FILE       "/data/harmony/custom_icons_registry.json"
#define PUSHED_TRACKER_FILE "/data/harmony/custom_icons_pushed.json"

static pthread_mutex_t s_reg_lock = PTHREAD_MUTEX_INITIALIZER;

static void normalize_name(const char *raw, char *out, size_t out_len) {
    if (!raw || !out || out_len == 0) return;
    const char *slash = strrchr(raw, '/');
    const char *base = slash ? slash + 1 : raw;
    size_t i = 0;
    for (; base[i] && i < out_len - 1; i++) {
        out[i] = (char)tolower((unsigned char)base[i]);
    }
    out[i] = '\0';
    char *dot = strrchr(out, '.');
    if (dot && strcmp(dot, ".png") == 0) {
        *dot = '\0';
    }
}

static cJSON *load_json_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 262144) { fclose(f); return NULL; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, sz, f);
    buf[n] = '\0';
    fclose(f);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

static void save_json_file_atomic(const char *path, cJSON *root) {
    if (!path || !root) return;
    char *txt = cJSON_PrintUnformatted(root);
    if (!txt) return;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs(txt, f);
        fclose(f);
        rename(tmp, path);
    }
    free(txt);
}

uint16_t custom_icon_registry_get_id(const char *icon_name) {
    if (!icon_name || !icon_name[0]) return 0;
    char norm[80] = {0};
    normalize_name(icon_name, norm, sizeof(norm));
    if (!norm[0]) return 0;

    pthread_mutex_lock(&s_reg_lock);
    cJSON *root = load_json_file(REGISTRY_FILE);
    if (!root) {
        root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "next_id", CUSTOM_ICON_BASE_ID);
        cJSON_AddItemToObject(root, "icons", cJSON_CreateObject());
    }

    cJSON *icons = cJSON_GetObjectItem(root, "icons");
    if (!icons) {
        icons = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "icons", icons);
    }

    cJSON *item = cJSON_GetObjectItem(icons, norm);
    uint16_t id = 0;
    if (item && cJSON_IsNumber(item)) {
        id = (uint16_t)item->valueint;
    } else {
        cJSON *j_next = cJSON_GetObjectItem(root, "next_id");
        int next_id = (j_next && cJSON_IsNumber(j_next)) ? j_next->valueint : CUSTOM_ICON_BASE_ID;
        if (next_id < CUSTOM_ICON_BASE_ID || next_id > 0xFFFF) next_id = CUSTOM_ICON_BASE_ID;
        id = (uint16_t)next_id;

        cJSON_AddNumberToObject(icons, norm, id);
        cJSON_DeleteItemFromObject(root, "next_id");
        cJSON_AddNumberToObject(root, "next_id", next_id + 1);
        save_json_file_atomic(REGISTRY_FILE, root);
        printf("[+] Registered custom icon '%s' -> ID 0x%04X (%u)\n", norm, id, id);
    }

    cJSON_Delete(root);
    pthread_mutex_unlock(&s_reg_lock);
    return id;
}

static bool is_icon_already_pushed(uint8_t slot, uint16_t custom_id) {
    cJSON *root = load_json_file(PUSHED_TRACKER_FILE);
    if (!root) return false;
    char slot_key[16];
    snprintf(slot_key, sizeof(slot_key), "slot%u", (unsigned int)slot);
    cJSON *arr = cJSON_GetObjectItem(root, slot_key);
    bool found = false;
    if (arr && cJSON_IsArray(arr)) {
        cJSON *elem = NULL;
        cJSON_ArrayForEach(elem, arr) {
            if (elem && cJSON_IsNumber(elem) && (uint16_t)elem->valueint == custom_id) {
                found = true;
                break;
            }
        }
    }
    cJSON_Delete(root);
    return found;
}

static void mark_icon_pushed(uint8_t slot, uint16_t custom_id) {
    cJSON *root = load_json_file(PUSHED_TRACKER_FILE);
    if (!root) root = cJSON_CreateObject();
    char slot_key[16];
    snprintf(slot_key, sizeof(slot_key), "slot%u", (unsigned int)slot);
    cJSON *arr = cJSON_GetObjectItem(root, slot_key);
    if (!arr) {
        arr = cJSON_CreateArray();
        cJSON_AddItemToObject(root, slot_key, arr);
    }
    cJSON_AddItemToArray(arr, cJSON_CreateNumber((int)custom_id));
    save_json_file_atomic(PUSHED_TRACKER_FILE, root);
    cJSON_Delete(root);
}

static bool locate_custom_icon_file(const char *norm, char *out_path, size_t out_len) {
    if (!norm || !norm[0]) return false;
    const char *paths[] = {
        "/data/harmony/custom_icons/%s.png",
        "/data/harmony/custom_icons/%s",
        "/data/harmony/custom_icons/custom_%s.png",
        "/data/harmony/custom_icons/custom_%s",
        "/tmp/www/assets/icons/%s.png",
        "/tmp/www/assets/icons/%s",
        "/cache/www/assets/icons/%s.png",
        "/cache/www/assets/icons/%s",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        char candidate[256];
        snprintf(candidate, sizeof(candidate), paths[i], norm);
        if (access(candidate, R_OK) == 0) {
            if (out_path && out_len > 0) {
                strncpy(out_path, candidate, out_len - 1);
                out_path[out_len - 1] = '\0';
            }
            return true;
        }
    }
    return false;
}

bool custom_icon_registry_has_file(const char *icon_name) {
    if (!icon_name || !icon_name[0]) return false;
    char norm[80] = {0};
    normalize_name(icon_name, norm, sizeof(norm));
    if (!norm[0]) return false;
    return locate_custom_icon_file(norm, NULL, 0);
}

int custom_icon_registry_sync_remote(uint8_t target_slot, uint16_t custom_id, const char *icon_name) {
    if (custom_id < CUSTOM_ICON_BASE_ID || !icon_name || !icon_name[0]) return -1;
    if (target_slot < 1 || target_slot > 2) target_slot = 1;

    char norm[80] = {0};
    normalize_name(icon_name, norm, sizeof(norm));
    if (!norm[0]) return -1;

    pthread_mutex_lock(&s_reg_lock);
    if (is_icon_already_pushed(target_slot, custom_id)) {
        pthread_mutex_unlock(&s_reg_lock);
        return 0;
    }

    /* Locate custom PNG on Hub */
    char candidate[256] = {0};
    bool found = locate_custom_icon_file(norm, candidate, sizeof(candidate));

    if (!found) {
        pthread_mutex_unlock(&s_reg_lock);
        printf("[-] Custom icon file not found on Hub for '%s'\n", norm);
        return -1;
    }

    char id_str[16];
    snprintf(id_str, sizeof(id_str), "%u", (unsigned int)custom_id);

    printf("[*] Pushing custom icon '%s' (%s) to remote slot %u as %s.png...\n",
           norm, candidate, (unsigned int)target_slot, id_str);
    int res = rf_push_custom_icon_file((int)target_slot, candidate, id_str);
    if (res == 0) {
        mark_icon_pushed(target_slot, custom_id);
        printf("[+] Custom icon %s.png successfully transferred to slot %u!\n", id_str, (unsigned int)target_slot);
    } else {
        printf("[-] Failed to transfer custom icon %s.png to slot %u\n", id_str, (unsigned int)target_slot);
    }

    pthread_mutex_unlock(&s_reg_lock);
    return res;
}
