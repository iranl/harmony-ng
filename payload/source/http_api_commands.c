#define _GNU_SOURCE
#include "http_api_commands.h"
#include "http_util.h"
#include "config_store.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>

int http_api_commands_handle(int client_fd, const char *method, const char *clean_path,
                             const char *query, const char *body, const char *req) {
    (void)query;
    (void)req;

    /* Route: POST /ir/command, POST /api/command-save, or POST /bt/command */
    if ((strcmp(clean_path, "/ir/command") == 0 || strcmp(clean_path, "/api/command-save") == 0 ||
         strcmp(clean_path, "/bt/command") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        int success = 0;
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
            int repeats = (j_rep && cJSON_IsNumber(j_rep)) ? (int)j_rep->valuedouble : 3;
            int port = (j_port && cJSON_IsNumber(j_port)) ? (int)j_port->valuedouble : 0;

            if (dev_id[0] && name[0]) {
                cJSON *root = config_store_get_devices_json();
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *dev_item = NULL;
                        cJSON_ArrayForEach(dev_item, devs) {
                            cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
                            if (!d) d = dev_item;
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                            char d_id_str[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON *cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "commands");
                                if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "Commands");
                                if (!cmds) {
                                    cmds = cJSON_CreateArray();
                                    cJSON_AddItemToObject(dev_item, "commands", cmds);
                                }
                                cJSON *cmd_match = NULL;
                                cJSON *ci = NULL;
                                cJSON_ArrayForEach(ci, cmds) {
                                    cJSON *cn = cJSON_GetObjectItemCaseSensitive(ci, "name");
                                    if (!cn) cn = cJSON_GetObjectItemCaseSensitive(ci, "Name");
                                    if (cn && cJSON_IsString(cn) && cn->valuestring) {
                                        if ((old_name[0] && strcmp(cn->valuestring, old_name) == 0) ||
                                            strcmp(cn->valuestring, name) == 0) {
                                            cmd_match = ci;
                                            break;
                                        }
                                    }
                                }
                                cJSON *dtype = cJSON_GetObjectItemCaseSensitive(d, "type");
                                if (!dtype) dtype = cJSON_GetObjectItemCaseSensitive(d, "DeviceTypeDisplayName");
                                const char *tstr = (dtype && dtype->valuestring) ? dtype->valuestring : "";
                                cJSON *dmfg = cJSON_GetObjectItemCaseSensitive(d, "manufacturer");
                                if (!dmfg) dmfg = cJSON_GetObjectItemCaseSensitive(d, "Manufacturer");
                                const char *mstr = (dmfg && dmfg->valuestring) ? dmfg->valuestring : "";
                                int dev_is_bt = (strcasestr(tstr, "bluetooth") != NULL || strcasestr(tstr, "bt") != NULL || strcasestr(mstr, "bluetooth") != NULL);
                                int dev_is_mqtt = (strcasestr(tstr, "mqtt") != NULL || strcasestr(mstr, "mqtt") != NULL);
                                int is_bt_cmd = dev_is_bt || (strncmp(raw_data, "G:HID", 5) == 0) || is_macro;
                                int is_mqtt_cmd = dev_is_mqtt || (strncmp(raw_data, "G:MQTT", 6) == 0);

                                if (cmd_match) {
                                    cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "Name");
                                    cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "name");
                                    cJSON_AddStringToObject(cmd_match, "name", name);

                                    if (raw_data[0]) {
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "KeyCode");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "keyCode");
                                        cJSON_AddStringToObject(cmd_match, "keyCode", raw_data);

                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "Raw");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "raw");
                                        cJSON_AddStringToObject(cmd_match, "raw", raw_data);
                                    }
                                    if (is_macro) {
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "isMacro");
                                        cJSON_AddBoolToObject(cmd_match, "isMacro", 1);
                                    }
                                    if (is_bt_cmd || is_mqtt_cmd) {
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "repeats");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "Repeats");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "controlPort");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "ControlPort");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "port");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "Port");
                                    } else {
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "Repeats");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "repeats");
                                        cJSON_AddNumberToObject(cmd_match, "repeats", repeats);

                                        if (port > 0) {
                                            cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "ControlPort");
                                            cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "controlPort");
                                            cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "port");
                                            cJSON_AddNumberToObject(cmd_match, "port", port);
                                        }
                                    }
                                } else {
                                    cJSON *new_cmd = cJSON_CreateObject();
                                    cJSON_AddStringToObject(new_cmd, "name", name);
                                    cJSON_AddStringToObject(new_cmd, "keyCode", raw_data);
                                    cJSON_AddStringToObject(new_cmd, "raw", raw_data);
                                    if (is_macro) {
                                        cJSON_AddBoolToObject(new_cmd, "isMacro", 1);
                                    }
                                    if (!is_bt_cmd && !is_mqtt_cmd) {
                                        cJSON_AddNumberToObject(new_cmd, "repeats", repeats);
                                        if (port > 0) cJSON_AddNumberToObject(new_cmd, "port", port);
                                    }
                                    cJSON_AddItemToArray(cmds, new_cmd);
                                }
                                config_store_save_devices(root);
                                success = 1;
                                break;
                            }
                        }
                    }
                }
            }
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/commands-batch or POST /ir/commands-batch */
    if ((strcmp(clean_path, "/api/commands-batch") == 0 || strcmp(clean_path, "/ir/commands-batch") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        int saved_count = 0;
        int success = 0;
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
                cJSON *root = config_store_get_devices_json();
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *dev_item = NULL;
                        cJSON_ArrayForEach(dev_item, devs) {
                            cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
                            if (!d) d = dev_item;
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                            char d_id_str[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON *cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "commands");
                                if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "Commands");
                                if (!cmds) {
                                    cmds = cJSON_CreateArray();
                                    cJSON_AddItemToObject(dev_item, "commands", cmds);
                                }
                                cJSON *cmd_in = NULL;
                                cJSON_ArrayForEach(cmd_in, j_cmds) {
                                    cJSON *jn = cJSON_GetObjectItemCaseSensitive(cmd_in, "name");
                                    cJSON *jr = cJSON_GetObjectItemCaseSensitive(cmd_in, "raw");
                                    if (!jr) jr = cJSON_GetObjectItemCaseSensitive(cmd_in, "code");
                                    cJSON *jrep = cJSON_GetObjectItemCaseSensitive(cmd_in, "repeats");
                                    cJSON *jport = cJSON_GetObjectItemCaseSensitive(cmd_in, "port");
                                    const char *name = (jn && cJSON_IsString(jn)) ? jn->valuestring : "";
                                    const char *raw_data = (jr && cJSON_IsString(jr)) ? jr->valuestring : "";
                                    int repeats = (jrep && cJSON_IsNumber(jrep)) ? (int)jrep->valuedouble : 3;
                                    int port = (jport && cJSON_IsNumber(jport)) ? (int)jport->valuedouble : 0;
                                    if (!name[0] || !raw_data[0]) continue;

                                    cJSON *cmd_match = NULL;
                                    cJSON *ci = NULL;
                                    cJSON_ArrayForEach(ci, cmds) {
                                        cJSON *cn = cJSON_GetObjectItemCaseSensitive(ci, "name");
                                        if (!cn) cn = cJSON_GetObjectItemCaseSensitive(ci, "Name");
                                        if (cn && cJSON_IsString(cn) && cn->valuestring && strcmp(cn->valuestring, name) == 0) {
                                            cmd_match = ci;
                                            break;
                                        }
                                    }
                                    if (cmd_match) {
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "KeyCode");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "keyCode");
                                        cJSON_AddStringToObject(cmd_match, "keyCode", raw_data);

                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "Raw");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "raw");
                                        cJSON_AddStringToObject(cmd_match, "raw", raw_data);

                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "Repeats");
                                        cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "repeats");
                                        cJSON_AddNumberToObject(cmd_match, "repeats", repeats);

                                        if (port > 0) {
                                            cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "ControlPort");
                                            cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "controlPort");
                                            cJSON_DeleteItemFromObjectCaseSensitive(cmd_match, "port");
                                            cJSON_AddNumberToObject(cmd_match, "port", port);
                                        }
                                    } else {
                                        cJSON *new_cmd = cJSON_CreateObject();
                                        cJSON_AddStringToObject(new_cmd, "name", name);
                                        cJSON_AddStringToObject(new_cmd, "keyCode", raw_data);
                                        cJSON_AddStringToObject(new_cmd, "raw", raw_data);
                                        cJSON_AddNumberToObject(new_cmd, "repeats", repeats);
                                        if (port > 0) cJSON_AddNumberToObject(new_cmd, "port", port);
                                        cJSON_AddItemToArray(cmds, new_cmd);
                                    }
                                    saved_count++;
                                }
                                config_store_save_devices(root);
                                success = 1;
                                break;
                            }
                        }
                    }
                }
            }
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        cJSON_AddNumberToObject(resp, "count", saved_count);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /ir/delete-command, POST /api/command-delete, or POST /bt/delete-command */
    if ((strcmp(clean_path, "/ir/delete-command") == 0 || strcmp(clean_path, "/api/command-delete") == 0 ||
         strcmp(clean_path, "/bt/delete-command") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        int success = 0;
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
                cJSON *root = config_store_get_devices_json();
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                    if (devs && cJSON_IsArray(devs)) {
                        cJSON *dev_item = NULL;
                        cJSON_ArrayForEach(dev_item, devs) {
                            cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
                            if (!d) d = dev_item;
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                            char d_id_str[32] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON *cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "commands");
                                if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(dev_item, "Commands");
                                if (cmds && cJSON_IsArray(cmds)) {
                                    int c_count = cJSON_GetArraySize(cmds);
                                    for (int ci = 0; ci < c_count; ci++) {
                                        cJSON *c_item = cJSON_GetArrayItem(cmds, ci);
                                        cJSON *cn = cJSON_GetObjectItemCaseSensitive(c_item, "name");
                                        if (!cn) cn = cJSON_GetObjectItemCaseSensitive(c_item, "Name");
                                        if (cn && cJSON_IsString(cn) && strcmp(cn->valuestring, cmd_name) == 0) {
                                            cJSON_DeleteItemFromArray(cmds, ci);
                                            config_store_save_devices(root);
                                            success = 1;
                                            break;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
            cJSON_Delete(p);
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", success);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    return 0;
}
