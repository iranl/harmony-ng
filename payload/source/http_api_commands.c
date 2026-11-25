#define _GNU_SOURCE
#include "net_device.h"
#include "http_api_commands.h"
#include "http_util.h"
#include "config_store.h"
#include "ble_adv_payload.h"
#include "plugin_mgr.h"
#include "cmd_range.h"
#include "cmd_ir_alt.h"
#include "cJSON.h"
#include "json_number.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>

/* port or repeats of a saved command: a JSON number, or the numeric text of a form post (both
 * web UIs post this dialog as a form, where every value is text). -1 when absent or not a whole
 * number 0..1000 */
static int cmd_int_field(const cJSON *j) {
    if (cJSON_IsNumber(j)) {
        int v = json_number_int(j, -1, 1001, -1);
        return v > 1000 ? -1 : v;
    }
    if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) {
        char *end = NULL;
        long v = strtol(j->valuestring, &end, 10);
        if (end && *end == '\0' && v >= 0 && v <= 1000) return (int)v;
    }
    return -1;
}

/* Sets a command's repeats and IR port from a save. repeats: 1..20 (the sender's range), absent
 * keeps the stored value, a new command gets 3. port: 1..7 is the command's own port mask, used
 * instead of the device's controlPort; 0 is "Default (Device Setting)" and removes it; absent
 * keeps the stored one. */
static void cmd_set_ir_fields(cJSON *cmd, const cJSON *j_rep, const cJSON *j_port, int is_new) {
    int rep = cmd_int_field(j_rep);
    if (rep > 20) rep = 20;
    if (rep < 1 && (is_new || !cJSON_GetObjectItemCaseSensitive(cmd, "repeats"))) rep = 3;
    if (rep >= 1) {
        cJSON_DeleteItemFromObjectCaseSensitive(cmd, "repeats");
        cJSON_AddNumberToObject(cmd, "repeats", rep);
    }
    int port = cmd_int_field(j_port);
    if (port == 0) {
        cJSON_DeleteItemFromObjectCaseSensitive(cmd, "port");
    } else if (port >= 1 && port <= 7) {
        cJSON_DeleteItemFromObjectCaseSensitive(cmd, "port");
        cJSON_AddNumberToObject(cmd, "port", port);
    }
}

/* The request fields of an HTTP or TCP command (net_device.h); config_sanitize.c checks them
 * and sets the keyCode */
static void copy_net_cmd_fields(const cJSON *p, cJSON *cmd) {
    static const char *const keys[] = { "httpMethod", "httpPath", "httpBody", "httpContentType",
                                        "tcpPayload", "tcpFormat" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(p, keys[i]);
        if (!cJSON_IsString(j)) continue;
        cJSON_DeleteItemFromObjectCaseSensitive(cmd, keys[i]);
        cJSON_AddStringToObject(cmd, keys[i], j->valuestring);
    }
}

int http_api_commands_handle(int client_fd, const char *method, const char *clean_path,
                             const char *query, const char *body, const char *req) {
    (void)query;
    (void)req;

    /* Route: POST /ir/command, POST /api/command-save, or POST /bt/command */
    if ((strcmp(clean_path, "/ir/command") == 0 || strcmp(clean_path, "/api/command-save") == 0 ||
         strcmp(clean_path, "/bt/command") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        if (p && !cmd_range_request_ok(p)) {
            cJSON_Delete(p);
            send_json_error(client_fd, 400, "range must be [min, max] (integers, min <= max), or null / [] to remove it");
            close(client_fd);
            return 1;
        }
        int success = 0;
        int save_failed = 0; /* the device was updated but devices.json could not be written */
        if (p) {
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(p, "id");
            cJSON *j_name = cJSON_GetObjectItemCaseSensitive(p, "name");
            cJSON *j_old = cJSON_GetObjectItemCaseSensitive(p, "oldName");
            cJSON *j_raw = cJSON_GetObjectItemCaseSensitive(p, "raw");
            if (!j_raw) j_raw = cJSON_GetObjectItemCaseSensitive(p, "code");
            cJSON *j_script = cJSON_GetObjectItemCaseSensitive(p, "script");
            cJSON *j_rep = cJSON_GetObjectItemCaseSensitive(p, "repeats");
            cJSON *j_port = cJSON_GetObjectItemCaseSensitive(p, "port");

            const char *dev_id = j_dev ? (cJSON_IsString(j_dev) ? j_dev->valuestring : "") : "";
            char dev_buf[32] = {0};
            if (!dev_id[0] && j_dev && cJSON_IsNumber(j_dev)) {
                snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)j_dev->valuedouble);
                dev_id = dev_buf;
            }
            const char *name = (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : "";
            const char *old_name = (j_old && cJSON_IsString(j_old)) ? j_old->valuestring : "";
            const char *raw_data = (j_raw && cJSON_IsString(j_raw)) ? j_raw->valuestring : "";
            char macro_buf[1024] = {0};
            int is_macro = 0;
            if (j_script && cJSON_IsString(j_script) && j_script->valuestring[0]) {
                snprintf(macro_buf, sizeof(macro_buf), "BT:SCRIPT:%s", j_script->valuestring);
                raw_data = macro_buf;
                is_macro = 1;
            }

            if (dev_id[0] && name[0]) {
                config_store_lock();
                cJSON *root = config_store_devices_copy();
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *dev_item = NULL;
                        cJSON_ArrayForEach(dev_item, devs) {
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(dev_item, "id");
                            char d_id_str[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON *cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "commands");
                                if (!cmds) {
                                    cmds = cJSON_CreateArray();
                                    cJSON_AddItemToObject(dev_item, "commands", cmds);
                                }
                                cJSON *cmd_match = NULL;
                                cJSON *ci = NULL;
                                cJSON_ArrayForEach(ci, cmds) {
                                    cJSON *cn = cJSON_GetObjectItemCaseSensitive(ci, "name");
                                    if (cn && cJSON_IsString(cn) && cn->valuestring) {
                                        if ((old_name[0] && strcmp(cn->valuestring, old_name) == 0) ||
                                            strcmp(cn->valuestring, name) == 0) {
                                            cmd_match = ci;
                                            break;
                                        }
                                    }
                                }
                                cJSON *dtype = cJSON_GetObjectItemCaseSensitive(dev_item, "type");
                                const char *tstr = (dtype && dtype->valuestring) ? dtype->valuestring : "";
                                cJSON *dmfg = cJSON_GetObjectItemCaseSensitive(dev_item, "manufacturer");
                                const char *mstr = (dmfg && dmfg->valuestring) ? dmfg->valuestring : "";
                                int dev_net = net_device_kind(dev_item);
                                int dev_is_bt = !dev_net && (strcasestr(tstr, "bluetooth") != NULL || strcasestr(tstr, "bt") != NULL || strcasestr(mstr, "bluetooth") != NULL);
                                int dev_is_mqtt = (strcasestr(tstr, "mqtt") != NULL || strcasestr(mstr, "mqtt") != NULL);
                                int dev_is_plugin = plugin_mgr_device_plugin_id(dev_item, NULL, 0);
                                int is_bt_cmd = dev_is_bt || (strncmp(raw_data, "G:HID", 5) == 0) || (strncmp(raw_data, "HID:", 4) == 0) || (strncmp(raw_data, "hid:", 4) == 0) || is_macro;
                                int is_mqtt_cmd = dev_is_mqtt || (strncmp(raw_data, "G:MQTT", 6) == 0);
                                int is_ble_cmd = (strncmp(raw_data, BLE_ADV_KEYCODE_PREFIX, BLE_ADV_KEYCODE_PREFIX_LEN) == 0);
                                int is_plugin_cmd = dev_is_plugin || (strncmp(raw_data, "PLUGIN:", 7) == 0 || strncmp(raw_data, "plugin:", 7) == 0);

                                if (cmd_match) {
                                    cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "name");
                                    cJSON_AddStringToObject(cmd_match, "name", name);

                                    if (raw_data[0]) {
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "keyCode");
                                        cJSON_AddStringToObject(cmd_match, "keyCode", raw_data);

                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "raw");
                                        cJSON_AddStringToObject(cmd_match, "raw", raw_data);
                                    }
                                    if (is_macro) {
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "isMacro");
                                        cJSON_AddBoolToObject(cmd_match, "isMacro", 1);
                                    }
                                    cmd_range_copy(cmd_match, p);
                                    if (dev_is_plugin) cmd_ir_alt_copy(cmd_match, p);
                                    if (dev_net) copy_net_cmd_fields(p, cmd_match);
                                    if (dev_is_plugin && cmd_has_ir_alt(cmd_match)) {
                                        /* a plugin command with an infrared alternative keeps its IR fields */
                                        cmd_set_ir_fields(cmd_match, j_rep, j_port, 0);
                                    } else if (is_bt_cmd || is_mqtt_cmd || is_ble_cmd || is_plugin_cmd || dev_net) {
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "repeats");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "port");
                                    } else {
                                        cmd_set_ir_fields(cmd_match, j_rep, j_port, 0);
                                    }
                                } else {
                                    cJSON *new_cmd = cJSON_CreateObject();
                                    cJSON_AddStringToObject(new_cmd, "name", name);
                                    cJSON_AddStringToObject(new_cmd, "keyCode", raw_data);
                                    cJSON_AddStringToObject(new_cmd, "raw", raw_data);
                                    if (is_macro) {
                                        cJSON_AddBoolToObject(new_cmd, "isMacro", 1);
                                    }
                                    cmd_range_copy(new_cmd, p);
                                    if (dev_is_plugin) cmd_ir_alt_copy(new_cmd, p);
                                    /* a new plugin command without "range" (or infrared alternative):
                                     * the manifest's (the legacy UI's single preset Add sends none) */
                                    if (dev_is_plugin && (!cJSON_GetObjectItemCaseSensitive(p, "range") ||
                                                          !cmd_ir_alt_in_request(p))) {
                                        char pid[64];
                                        plugin_mgr_device_plugin_id(dev_item, pid, sizeof(pid));
                                        cJSON *dd = plugin_mgr_get_device_definition(pid);
                                        if (!cJSON_GetObjectItemCaseSensitive(p, "range")) cmd_range_fill_from_manifest(new_cmd, dd);
                                        if (!cmd_ir_alt_in_request(p)) cmd_ir_alt_fill_from_manifest(new_cmd, dd);
                                        cJSON_Delete(dd);
                                    }
                                    if (dev_net) copy_net_cmd_fields(p, new_cmd);
                                    if ((dev_is_plugin && cmd_has_ir_alt(new_cmd)) ||
                                        (!is_bt_cmd && !is_mqtt_cmd && !is_ble_cmd && !is_plugin_cmd && !dev_net)) {
                                        cmd_set_ir_fields(new_cmd, j_rep, j_port, 1);
                                    }
                                    cJSON_AddItemToArray(cmds, new_cmd);
                                }
                                if (config_store_save_devices(root) == 0) {
                                    root = NULL;
                                    success = 1;
                                } else {
                                    save_failed = 1;
                                }
                                break;
                            }
                        }
                    }
                    cJSON_Delete(root); /* NULL once the store took it */
                }
                config_store_unlock();
            }
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        if (save_failed) cJSON_AddStringToObject(resp, "error", "Could not save the devices (flash full?)");
        send_cjson_resp(client_fd, save_failed ? 500 : 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/commands-batch or POST /ir/commands-batch */
    if ((strcmp(clean_path, "/api/commands-batch") == 0 || strcmp(clean_path, "/ir/commands-batch") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        const cJSON *rc_in = NULL;
        cJSON_ArrayForEach(rc_in, (p ? cJSON_GetObjectItemCaseSensitive(p, "commands") : NULL)) {
            if (cJSON_IsObject(rc_in) && !cmd_range_request_ok(rc_in)) {
                cJSON_Delete(p);
                send_json_error(client_fd, 400, "range must be [min, max] (integers, min <= max), or null / [] to remove it");
                close(client_fd);
                return 1;
            }
        }
        int saved_count = 0;
        int success = 0;
        int save_failed = 0; /* the device was updated but devices.json could not be written */
        if (p) {
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(p, "id");
            cJSON *j_cmds = cJSON_GetObjectItemCaseSensitive(p, "commands");

            const char *dev_id = j_dev ? (cJSON_IsString(j_dev) ? j_dev->valuestring : "") : "";
            char dev_buf[32] = {0};
            if (!dev_id[0] && j_dev && cJSON_IsNumber(j_dev)) {
                snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)j_dev->valuedouble);
                dev_id = dev_buf;
            }

            if (dev_id[0] && j_cmds && cJSON_IsArray(j_cmds)) {
                config_store_lock();
                cJSON *root = config_store_devices_copy();
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *dev_item = NULL;
                        cJSON_ArrayForEach(dev_item, devs) {
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(dev_item, "id");
                            char d_id_str[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON *cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "commands");
                                if (!cmds) {
                                    cmds = cJSON_CreateArray();
                                    cJSON_AddItemToObject(dev_item, "commands", cmds);
                                }
                                int dev_is_plugin = plugin_mgr_device_plugin_id(dev_item, NULL, 0);
                                cJSON *cmd_in = NULL;
                                cJSON_ArrayForEach(cmd_in, j_cmds) {
                                    cJSON *jn = cJSON_GetObjectItemCaseSensitive(cmd_in, "name");
                                    cJSON *jr = cJSON_GetObjectItemCaseSensitive(cmd_in, "raw");
                                    if (!jr) jr = cJSON_GetObjectItemCaseSensitive(cmd_in, "code");
                                    cJSON *jrep = cJSON_GetObjectItemCaseSensitive(cmd_in, "repeats");
                                    cJSON *jport = cJSON_GetObjectItemCaseSensitive(cmd_in, "port");
                                    const char *name = (jn && cJSON_IsString(jn)) ? jn->valuestring : "";
                                    const char *raw_data = (jr && cJSON_IsString(jr)) ? jr->valuestring : "";
                                    if (!name[0] || !raw_data[0]) continue;
                                    int is_ble_cmd = (strncmp(raw_data, BLE_ADV_KEYCODE_PREFIX, BLE_ADV_KEYCODE_PREFIX_LEN) == 0);
                                    int is_plugin_cmd = dev_is_plugin || (strncmp(raw_data, "PLUGIN:", 7) == 0 || strncmp(raw_data, "plugin:", 7) == 0);

                                    cJSON *cmd_match = NULL;
                                    cJSON *ci = NULL;
                                    cJSON_ArrayForEach(ci, cmds) {
                                        cJSON *cn = cJSON_GetObjectItemCaseSensitive(ci, "name");
                                        if (cn && cJSON_IsString(cn) && cn->valuestring && strcmp(cn->valuestring, name) == 0) {
                                            cmd_match = ci;
                                            break;
                                        }
                                    }
                                    if (cmd_match) {
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "keyCode");
                                        cJSON_AddStringToObject(cmd_match, "keyCode", raw_data);

                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "raw");
                                        cJSON_AddStringToObject(cmd_match, "raw", raw_data);

                                        if (dev_is_plugin) cmd_ir_alt_copy(cmd_match, cmd_in);
                                        if (dev_is_plugin && cmd_has_ir_alt(cmd_match)) {
                                            cmd_set_ir_fields(cmd_match, jrep, jport, 0);
                                        } else if (is_ble_cmd || is_plugin_cmd) {
                                            cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "repeats");
                                            cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "port");
                                        } else {
                                            cmd_set_ir_fields(cmd_match, jrep, jport, 0);
                                        }
                                        cmd_range_copy(cmd_match, cmd_in);
                                    } else {
                                        cJSON *new_cmd = cJSON_CreateObject();
                                        cJSON_AddStringToObject(new_cmd, "name", name);
                                        cJSON_AddStringToObject(new_cmd, "keyCode", raw_data);
                                        cJSON_AddStringToObject(new_cmd, "raw", raw_data);
                                        cmd_range_copy(new_cmd, cmd_in);
                                        if (dev_is_plugin) cmd_ir_alt_copy(new_cmd, cmd_in);
                                        if (dev_is_plugin && (!cJSON_GetObjectItemCaseSensitive(cmd_in, "range") ||
                                                              !cmd_ir_alt_in_request(cmd_in))) {
                                            char pid[64];
                                            plugin_mgr_device_plugin_id(dev_item, pid, sizeof(pid));
                                            cJSON *dd = plugin_mgr_get_device_definition(pid);
                                            if (!cJSON_GetObjectItemCaseSensitive(cmd_in, "range")) cmd_range_fill_from_manifest(new_cmd, dd);
                                            if (!cmd_ir_alt_in_request(cmd_in)) cmd_ir_alt_fill_from_manifest(new_cmd, dd);
                                            cJSON_Delete(dd);
                                        }
                                        if ((dev_is_plugin && cmd_has_ir_alt(new_cmd)) || (!is_ble_cmd && !is_plugin_cmd)) {
                                            cmd_set_ir_fields(new_cmd, jrep, jport, 1);
                                        }
                                        cJSON_AddItemToArray(cmds, new_cmd);
                                    }
                                    saved_count++;
                                }
                                if (config_store_save_devices(root) == 0) {
                                    root = NULL;
                                    success = 1;
                                } else {
                                    save_failed = 1;
                                }
                                break;
                            }
                        }
                    }
                    cJSON_Delete(root); /* NULL once the store took it */
                }
                config_store_unlock();
            }
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        /* nothing was stored when the save failed: count only what reached the flash */
        cJSON_AddNumberToObject(resp, "count", success ? saved_count : 0);
        if (save_failed) cJSON_AddStringToObject(resp, "error", "Could not save the devices (flash full?)");
        send_cjson_resp(client_fd, save_failed ? 500 : 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /ir/delete-command, POST /api/command-delete, or POST /bt/delete-command */
    if ((strcmp(clean_path, "/ir/delete-command") == 0 || strcmp(clean_path, "/api/command-delete") == 0 ||
         strcmp(clean_path, "/bt/delete-command") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        int success = 0;
        int save_failed = 0; /* the command was removed but devices.json could not be written */
        if (p) {
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(p, "id");
            cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(p, "command");
            if (!j_cmd) j_cmd = cJSON_GetObjectItemCaseSensitive(p, "name");

            const char *dev_id = j_dev ? (cJSON_IsString(j_dev) ? j_dev->valuestring : "") : "";
            char dev_buf[32] = {0};
            if (!dev_id[0] && j_dev && cJSON_IsNumber(j_dev)) {
                snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)j_dev->valuedouble);
                dev_id = dev_buf;
            }
            const char *cmd_name = (j_cmd && cJSON_IsString(j_cmd)) ? j_cmd->valuestring : "";

            if (dev_id[0] && cmd_name[0]) {
                config_store_lock();
                cJSON *root = config_store_devices_copy();
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *dev_item = NULL;
                        cJSON_ArrayForEach(dev_item, devs) {
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(dev_item, "id");
                            char d_id_str[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON *cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "commands");
                                if (cmds && cJSON_IsArray(cmds)) {
                                    int c_count = cJSON_GetArraySize(cmds);
                                    for (int ci = 0; ci < c_count; ci++) {
                                        cJSON *c_item = cJSON_GetArrayItem(cmds, ci);
                                        cJSON *cn = cJSON_GetObjectItemCaseSensitive(c_item, "name");
                                        if (cn && cJSON_IsString(cn) && strcmp(cn->valuestring, cmd_name) == 0) {
                                            cJSON_DeleteItemFromArray(cmds, ci);
                                            if (config_store_save_devices(root) == 0) {
                                                root = NULL;
                                                success = 1;
                                            } else {
                                                save_failed = 1;
                                            }
                                            break;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                    cJSON_Delete(root); /* NULL once the store took it */
                }
                config_store_unlock();
            }
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        if (save_failed) cJSON_AddStringToObject(resp, "error", "Could not save the devices (flash full?)");
        send_cjson_resp(client_fd, save_failed ? 500 : 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    return 0;
}
