/*
 * One-time migration of the Bluetooth host state hub_btstack used to spread over several
 * files. config/devices.json "bdaddr" is now the only device-to-host mapping and
 * bthid_host_target the only default host:
 *  - bt-devices.json: an entry's bdaddr goes to the device with the same id in devices.json
 *    when that device has no host yet; the file is then renamed to bt-devices.json.migrated.
 *  - bthid_target: its address becomes bthid_host_target when that file does not exist yet;
 *    the file is then renamed to bthid_target.migrated.
 * A legacy file is renamed only after it was read and applied, so any failure leaves it in
 * place for the next start. hub_daemon runs it in config_store_init(), before it loads
 * devices.json into its cache (at start, and after a backup restore).
 */
#include "bt_migrate.h"
#include "cJSON.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void mlog(bt_migrate_log_fn log_fn, const char *fmt, ...) {
    if (!log_fn) return;
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_fn(buf);
}

/* Whole file as a string, or NULL; *missing tells "does not exist" apart from a read error */
static char *read_whole_file(const char *path, long max_size, int *missing) {
    *missing = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) *missing = 1;
        return NULL;
    }
    char *buf = NULL;
    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
    if (sz >= 0 && sz <= max_size && fseek(f, 0, SEEK_SET) == 0) {
        buf = (char *)malloc((size_t)sz + 1);
        if (buf) {
            size_t r = fread(buf, 1, (size_t)sz, f);
            if (r != (size_t)sz || ferror(f)) {
                free(buf);
                buf = NULL;
            } else {
                buf[r] = '\0';
            }
        }
    }
    fclose(f);
    return buf;
}

/* tmp file + rename, so a reader never sees a half-written file; the pid in the tmp name
 * keeps it apart from hub_daemon's own "<file>.tmp" */
static int replace_file(const char *path, const char *content) {
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.migrate.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    size_t len = strlen(content);
    int ok = fwrite(content, 1, len, f) == len;
    if (ok && (len == 0 || content[len - 1] != '\n')) ok = fputc('\n', f) != EOF;
    if (ok) ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* "XX:XX:XX:XX:XX:XX" at s (more text may follow); the all-zero address is not a host */
static int bdaddr_at(const char *s) {
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (s[i] != ':') return 0;
        } else if (!isxdigit((unsigned char)s[i])) {
            return 0;
        }
    }
    return strncmp(s, "00:00:00:00:00:00", 17) != 0;
}

static int valid_bdaddr(const char *s) {
    return s && strlen(s) == 17 && bdaddr_at(s);
}

/* Device ids are strings, but older files carry numbers */
static int id_string(const cJSON *jid, char *out, size_t out_len) {
    out[0] = '\0';
    if (jid && cJSON_IsNumber(jid)) {
        snprintf(out, out_len, "%ld", (long)jid->valuedouble);
    } else if (jid && cJSON_IsString(jid) && jid->valuestring) {
        snprintf(out, out_len, "%s", jid->valuestring);
    }
    return out[0] != '\0';
}

/* A host already set in devices.json wins over the legacy file; btHost is the older key */
static const char *device_host(const cJSON *dev) {
    const char *keys[] = {"bdaddr", "btHost", NULL};
    for (int k = 0; keys[k]; k++) {
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(dev, keys[k]);
        if (j && cJSON_IsString(j) && j->valuestring && j->valuestring[0]) return j->valuestring;
    }
    return NULL;
}

static void rename_migrated(const char *path, bt_migrate_log_fn log_fn) {
    char done[300];
    snprintf(done, sizeof(done), "%s.migrated", path);
    if (rename(path, done) == 0) {
        mlog(log_fn, "BT migration: %s renamed to %s", path, done);
    } else if (errno != ENOENT) {
        mlog(log_fn, "BT migration: could not rename %s (errno %d)", path, errno);
    }
}

static void migrate_bt_devices(const char *dir, bt_migrate_log_fn log_fn) {
    char legacy_path[256], devices_path[256];
    snprintf(legacy_path, sizeof(legacy_path), "%s/bt-devices.json", dir);
    snprintf(devices_path, sizeof(devices_path), "%s/config/devices.json", dir);

    int missing = 0;
    char *raw = read_whole_file(legacy_path, 1024 * 1024, &missing);
    if (!raw) {
        if (!missing) mlog(log_fn, "BT migration: %s could not be read, left in place", legacy_path);
        return;
    }
    cJSON *legacy = cJSON_Parse(raw);
    free(raw);
    cJSON *ldevs = NULL;
    if (legacy) ldevs = cJSON_IsArray(legacy) ? legacy : cJSON_GetObjectItemCaseSensitive(legacy, "devices");
    if (!ldevs || !cJSON_IsArray(ldevs)) {
        mlog(log_fn, "BT migration: %s is not valid JSON with a devices list, left in place", legacy_path);
        cJSON_Delete(legacy);
        return;
    }

    /* Entries that can map a device to a host; without any, there is nothing to apply */
    int candidates = 0;
    const cJSON *le = NULL;
    cJSON_ArrayForEach(le, ldevs) {
        char lid[64];
        const cJSON *ja = cJSON_GetObjectItemCaseSensitive(le, "bdaddr");
        if (!ja) ja = cJSON_GetObjectItemCaseSensitive(le, "btHost");
        if (id_string(cJSON_GetObjectItemCaseSensitive(le, "id"), lid, sizeof(lid)) &&
            ja && cJSON_IsString(ja) && valid_bdaddr(ja->valuestring)) {
            candidates++;
        }
    }

    cJSON *droot = NULL;
    if (candidates > 0) {
        raw = read_whole_file(devices_path, 4 * 1024 * 1024, &missing);
        if (!raw) {
            mlog(log_fn, "BT migration: %s %s, %s left in place", devices_path,
                 missing ? "does not exist" : "could not be read", legacy_path);
            cJSON_Delete(legacy);
            return;
        }
        droot = cJSON_Parse(raw);
        free(raw);
        cJSON *ddevs = droot ? cJSON_GetObjectItemCaseSensitive(droot, "devices") : NULL;
        if (!ddevs || !cJSON_IsArray(ddevs)) {
            mlog(log_fn, "BT migration: %s is not valid JSON with a devices list, %s left in place",
                 devices_path, legacy_path);
            cJSON_Delete(droot);
            cJSON_Delete(legacy);
            return;
        }

        int changed = 0;
        cJSON_ArrayForEach(le, ldevs) {
            char lid[64];
            const cJSON *ja = cJSON_GetObjectItemCaseSensitive(le, "bdaddr");
            if (!ja) ja = cJSON_GetObjectItemCaseSensitive(le, "btHost");
            if (!id_string(cJSON_GetObjectItemCaseSensitive(le, "id"), lid, sizeof(lid)) ||
                !ja || !cJSON_IsString(ja) || !valid_bdaddr(ja->valuestring)) {
                continue;
            }
            cJSON *dev = NULL;
            cJSON *ditem = NULL;
            cJSON_ArrayForEach(ditem, ddevs) {
                char did[64];
                if (id_string(cJSON_GetObjectItemCaseSensitive(ditem, "id"), did, sizeof(did)) &&
                    strcmp(did, lid) == 0) {
                    dev = ditem;
                    break;
                }
            }
            if (!dev) {
                mlog(log_fn, "BT migration: device %s (host %s) is not in devices.json, skipped", lid, ja->valuestring);
                continue;
            }
            const char *cur = device_host(dev);
            if (cur) {
                /* A host kept only in the older btHost key moves to bdaddr */
                const cJSON *jb = cJSON_GetObjectItemCaseSensitive(dev, "bdaddr");
                if (!jb || !cJSON_IsString(jb) || !jb->valuestring || !jb->valuestring[0]) {
                    char host[18];
                    snprintf(host, sizeof(host), "%s", cur);
                    cJSON_DeleteItemFromObjectCaseSensitive(dev, "btHost");
                    cJSON_DeleteItemFromObjectCaseSensitive(dev, "bdaddr");
                    cJSON_AddStringToObject(dev, "bdaddr", host);
                    mlog(log_fn, "BT migration: device %s keeps its host %s, moved from btHost to bdaddr", lid, host);
                    changed++;
                } else {
                    mlog(log_fn, "BT migration: device %s keeps its host %s from devices.json", lid, cur);
                }
                continue;
            }
            /* Same shape as link_bt_device_to_addr(): bdaddr only, no empty btHost left over to
             * confuse readers that fall back to it; an empty bdaddr is filled in place */
            cJSON_DeleteItemFromObjectCaseSensitive(dev, "btHost");
            if (cJSON_GetObjectItemCaseSensitive(dev, "bdaddr")) {
                cJSON_ReplaceItemInObjectCaseSensitive(dev, "bdaddr", cJSON_CreateString(ja->valuestring));
            } else {
                cJSON_AddStringToObject(dev, "bdaddr", ja->valuestring);
            }
            mlog(log_fn, "BT migration: device %s linked to host %s in devices.json", lid, ja->valuestring);
            changed++;
        }

        if (changed > 0) {
            char *out = cJSON_Print(droot);
            int rc = out ? replace_file(devices_path, out) : -1;
            free(out);
            if (rc != 0) {
                mlog(log_fn, "BT migration: could not write %s, %s left in place", devices_path, legacy_path);
                cJSON_Delete(droot);
                cJSON_Delete(legacy);
                return;
            }
        }
    }

    cJSON_Delete(droot);
    cJSON_Delete(legacy);
    rename_migrated(legacy_path, log_fn);
}

static void migrate_bthid_target(const char *dir, bt_migrate_log_fn log_fn) {
    char legacy_path[256], host_path[256];
    snprintf(legacy_path, sizeof(legacy_path), "%s/bthid_target", dir);
    snprintf(host_path, sizeof(host_path), "%s/bthid_host_target", dir);

    int missing = 0;
    char *raw = read_whole_file(legacy_path, 4096, &missing);
    if (!raw) {
        if (!missing) mlog(log_fn, "BT migration: %s could not be read, left in place", legacy_path);
        return;
    }
    if (access(host_path, F_OK) == 0) {
        /* Both were written together on every pairing; the host target is the one kept */
        free(raw);
        mlog(log_fn, "BT migration: %s already set, %s not needed", host_path, legacy_path);
        rename_migrated(legacy_path, log_fn);
        return;
    }

    /* "type=btkeyboard\nbdaddr=..." from hub_btstack, or "btkeyboard <addr>" from the old
     * save_bthid_target(): take the first address in the file */
    char addr[18] = "";
    size_t left = strlen(raw);
    for (const char *p = raw; left >= 17; p++, left--) {
        if (bdaddr_at(p)) {
            memcpy(addr, p, 17);
            addr[17] = '\0';
            break;
        }
    }
    free(raw);
    if (!addr[0]) {
        mlog(log_fn, "BT migration: %s holds no host address, left in place", legacy_path);
        return;
    }

    char content[40];
    snprintf(content, sizeof(content), "bdaddr=%s\n", addr);
    if (replace_file(host_path, content) != 0) {
        mlog(log_fn, "BT migration: could not write %s, %s left in place", host_path, legacy_path);
        return;
    }
    mlog(log_fn, "BT migration: default host %s moved from %s to %s", addr, legacy_path, host_path);
    rename_migrated(legacy_path, log_fn);
}

void bt_migrate_legacy_state(const char *harmony_dir, bt_migrate_log_fn log_fn) {
    if (!harmony_dir || !harmony_dir[0]) return;
    migrate_bt_devices(harmony_dir, log_fn);
    migrate_bthid_target(harmony_dir, log_fn);
}
