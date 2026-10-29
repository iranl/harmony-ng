#define _GNU_SOURCE
#include "http_api_devices.h"
#include "http_server.h"
#include "http_util.h"
#include "config_store.h"
#include "resource_cache.h"
#include "hw_action.h"
#include "ir_i2s.h"
#include "config_sanitize.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>

int mqtt_dispatch_button_pulse_topic(const char *dev_id, const char *dev_name_in, const char *cmd_name,
                                     const char *topic_in, int req_pulse_ms,
                                     char *out_topic, size_t out_topic_len, int *out_pulse_ms);

int http_api_devices_handle(int client_fd, const char *method, const char *clean_path,
                            const char *query, const char *body, const char *req) {
    (void)query;
    (void)req;

    /* Route: GET /api/inventory */
    if (strcmp(clean_path, "/api/inventory") == 0 && strcmp(method, "GET") == 0) {
        cJSON *devs = get_device_list_cached();
        if (devs) {
            send_cjson_resp(client_fd, 200, devs);
        } else {
            send_json_error(client_fd, 404, "DeviceList not found");
        }
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/devices */
    if (strcmp(clean_path, "/api/devices") == 0 && strcmp(method, "GET") == 0) {
        cJSON *root = config_store_get_devices_json();
        if (root) {
            send_cjson_resp(client_fd, 200, root);
        } else {
            send_json_error(client_fd, 404, "Devices not found");
        }
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/pronto-blast */
    if (strcmp(clean_path, "/api/pronto-blast") == 0 && strcmp(method, "POST") == 0) {
        char pronto[512] = {0};
        uint8_t ports = IR_PORT_ALL;
        uint8_t repeats = 3;

        cJSON *req_j = body ? cJSON_Parse(body) : NULL;
        if (req_j) {
            cJSON *jp = cJSON_GetObjectItemCaseSensitive(req_j, "pronto");
            if (jp && cJSON_IsString(jp) && jp->valuestring) strncpy(pronto, jp->valuestring, sizeof(pronto) - 1);

            cJSON *jport = cJSON_GetObjectItemCaseSensitive(req_j, "ports");
            if (jport && cJSON_IsNumber(jport)) ports = (uint8_t)jport->valuedouble;
            else if (jport && cJSON_IsString(jport) && jport->valuestring) ports = (uint8_t)atoi(jport->valuestring);

            cJSON *jrep = cJSON_GetObjectItemCaseSensitive(req_j, "repeats");
            if (jrep && cJSON_IsNumber(jrep)) repeats = (uint8_t)jrep->valuedouble;
            else if (jrep && cJSON_IsString(jrep) && jrep->valuestring) repeats = (uint8_t)atoi(jrep->valuestring);

            cJSON_Delete(req_j);
        }

        if (!pronto[0]) {
            send_json_error(client_fd, 400, "Missing pronto parameter");
            close(client_fd);
            return 1;
        }

        int rc = hw_ir_send_pronto(pronto, ports, repeats);
        if (rc == 0) {
            cJSON *suc = cJSON_CreateObject();
            cJSON_AddBoolToObject(suc, "success", 1);
            send_cjson_resp(client_fd, 200, suc);
            cJSON_Delete(suc);
        } else {
            send_json_error(client_fd, 500, "IR send failed");
        }
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/ir-send or POST /ir/send */
    if ((strcmp(clean_path, "/api/ir-send") == 0 || strcmp(clean_path, "/ir/send") == 0) && strcmp(method, "POST") == 0) {
        char dev_id[64] = {0};
        char cmd_name[128] = {0};
        char pronto_buf[2048] = {0};
        int port = 7;
        int repeats = 1;
        cJSON *req_j = body ? parse_request_params(body) : NULL;
        if (req_j) {
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(req_j, "deviceId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(req_j, "id");
            if (jid && cJSON_IsString(jid) && jid->valuestring) strncpy(dev_id, jid->valuestring, sizeof(dev_id) - 1);
            else if (jid && cJSON_IsNumber(jid)) snprintf(dev_id, sizeof(dev_id), "%ld", (long)jid->valuedouble);

            cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(req_j, "command");
            if (!jcmd) jcmd = cJSON_GetObjectItemCaseSensitive(req_j, "name");
            if (jcmd && cJSON_IsString(jcmd) && jcmd->valuestring) strncpy(cmd_name, jcmd->valuestring, sizeof(cmd_name) - 1);

            cJSON *jpronto = cJSON_GetObjectItemCaseSensitive(req_j, "pronto");
            if (!jpronto) jpronto = cJSON_GetObjectItemCaseSensitive(req_j, "code");
            if (!jpronto) jpronto = cJSON_GetObjectItemCaseSensitive(req_j, "raw");
            if (jpronto && cJSON_IsString(jpronto) && jpronto->valuestring) strncpy(pronto_buf, jpronto->valuestring, sizeof(pronto_buf) - 1);

            cJSON *jport = cJSON_GetObjectItemCaseSensitive(req_j, "port");
            if (jport && cJSON_IsNumber(jport) && jport->valueint > 0) port = jport->valueint;

            cJSON *jrep = cJSON_GetObjectItemCaseSensitive(req_j, "repeats");
            if (jrep && cJSON_IsNumber(jrep) && jrep->valueint > 0) repeats = jrep->valueint;

            cJSON_Delete(req_j);
        }

        int rc = -1;
        if (pronto_buf[0]) {
            rc = hw_ir_send_pronto(pronto_buf, (uint8_t)port, (uint8_t)repeats);
            if (rc != 0 && strncmp(pronto_buf, "G:", 2) == 0) {
                rc = hw_ir_send_harmony_keycode(pronto_buf, (uint8_t)port, (uint8_t)repeats);
            }
        } else if (dev_id[0] && cmd_name[0]) {
            rc = hw_device_command_send(dev_id, cmd_name);
        } else {
            send_json_error(client_fd, 400, "Missing deviceId/command or pronto code");
            close(client_fd);
            return 1;
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", rc == 0);
        if (dev_id[0]) cJSON_AddStringToObject(resp, "deviceId", dev_id);
        if (cmd_name[0]) cJSON_AddStringToObject(resp, "command", cmd_name);
        if (rc != 0) cJSON_AddStringToObject(resp, "error", "IR transmit failed");
        send_cjson_resp(client_fd, rc == 0 ? 200 : 500, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: GET /api/device-commands */
    if (strncmp(clean_path, "/api/device-commands", 20) == 0 && strcmp(method, "GET") == 0) {
        char dev_id[64] = {0};
        if (query) {
            cJSON *qp = parse_request_params(query);
            if (qp) {
                cJSON *jid = cJSON_GetObjectItemCaseSensitive(qp, "deviceId");
                if (!jid) jid = cJSON_GetObjectItemCaseSensitive(qp, "id");
                if (jid && cJSON_IsString(jid)) strncpy(dev_id, jid->valuestring, sizeof(dev_id) - 1);
                cJSON_Delete(qp);
            }
        }
        cJSON *res_cmds = cJSON_CreateArray();
        if (dev_id[0]) {
            cJSON *root = config_store_get_devices_json();
            if (root) {
                cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                if (devs && cJSON_IsArray(devs)) {
                    cJSON *di = NULL;
                    cJSON_ArrayForEach(di, devs) {
                        cJSON *d = cJSON_GetObjectItemCaseSensitive(di, "Device");
                        if (!d) d = di;
                        cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                        if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                        char d_id_str[64] = {0};
                        if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                        else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                        if (strcmp(d_id_str, dev_id) == 0) {
                            cJSON *cmds = cJSON_GetObjectItemCaseSensitive(di, "commands");
                            if (!cmds) cmds = cJSON_GetObjectItemCaseSensitive(di, "Commands");
                            if (cmds && cJSON_IsArray(cmds)) {
                                cJSON *ci = NULL;
                                cJSON_ArrayForEach(ci, cmds) {
                                    cJSON_AddItemToArray(res_cmds, cJSON_Duplicate(ci, 1));
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddItemToObject(resp, "commands", res_cmds);
        send_cjson_resp(client_fd, 200, resp);
        cJSON_Delete(resp);
        close(client_fd);
        return 1;
    }

    /* Route: POST /api/device-mqtt-test */
    if (strcmp(clean_path, "/api/device-mqtt-test") == 0 && strcmp(method, "POST") == 0) {
        char dev_id[64] = {0};
        char dev_name[128] = {0};
        char cmd_name[128] = "PowerToggle";
        char req_topic[256] = {0};
        int req_pulse_ms = 1000;

        cJSON *req_j = body ? parse_request_params(body) : NULL;
        if (req_j) {
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(req_j, "deviceId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(req_j, "id");
            if (jid && cJSON_IsString(jid)) strncpy(dev_id, jid->valuestring, sizeof(dev_id) - 1);
            else if (jid && cJSON_IsNumber(jid)) snprintf(dev_id, sizeof(dev_id), "%ld", (long)jid->valuedouble);

            cJSON *jdev = cJSON_GetObjectItemCaseSensitive(req_j, "device");
            if (jdev && cJSON_IsString(jdev)) strncpy(dev_name, jdev->valuestring, sizeof(dev_name) - 1);

            cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(req_j, "command");
            if (jcmd && cJSON_IsString(jcmd) && jcmd->valuestring[0]) strncpy(cmd_name, jcmd->valuestring, sizeof(cmd_name) - 1);

            cJSON *jtop = cJSON_GetObjectItemCaseSensitive(req_j, "topic");
            if (!jtop) jtop = cJSON_GetObjectItemCaseSensitive(req_j, "mqttTopic");
            if (jtop && cJSON_IsString(jtop) && jtop->valuestring[0]) strncpy(req_topic, jtop->valuestring, sizeof(req_topic) - 1);

            cJSON *jp = cJSON_GetObjectItemCaseSensitive(req_j, "pulseMs");
            if (!jp) jp = cJSON_GetObjectItemCaseSensitive(req_j, "mqttPulse");
            if (jp && cJSON_IsNumber(jp) && jp->valueint > 0) req_pulse_ms = jp->valueint;
            cJSON_Delete(req_j);
        }

        if (!dev_name[0] && dev_id[0]) {
            strncpy(dev_name, dev_id, sizeof(dev_name) - 1);
        }

        char final_topic[256] = {0};
        int final_pulse_ms = 0;
        int mrc = mqtt_dispatch_button_pulse_topic(dev_id, dev_name, cmd_name, req_topic[0] ? req_topic : NULL, req_pulse_ms,
                                                  final_topic, sizeof(final_topic), &final_pulse_ms);
        if (mrc == 0) {
            cJSON *resp_j = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp_j, "ok", 1);
            cJSON_AddNumberToObject(resp_j, "pulseMs", final_pulse_ms);
            cJSON_AddStringToObject(resp_j, "topic", final_topic);
            cJSON_AddStringToObject(resp_j, "message", "MQTT test publish sent");
            send_cjson_resp(client_fd, 200, resp_j);
            cJSON_Delete(resp_j);
        } else {
            send_json_error(client_fd, 503, "MQTT broker not connected");
        }
        close(client_fd);
        return 1;
    }

    /* Route: POST /ir/device, POST /api/device-save, POST /ir/device-mqtt, POST /ir/device-power, or POST /ir/new-device */
    if ((strcmp(clean_path, "/ir/device") == 0 || strcmp(clean_path, "/api/device-save") == 0 ||
         strcmp(clean_path, "/ir/device-mqtt") == 0 || strcmp(clean_path, "/ir/device-power") == 0 ||
         strcmp(clean_path, "/ir/new-device") == 0 || strcmp(clean_path, "/api/new-device") == 0) && strcmp(method, "POST") == 0) {
        cJSON *p = parse_request_params(body);
        int success = 0;
        if (p) {
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(p, "id");
            cJSON *j_name = cJSON_GetObjectItemCaseSensitive(p, "name");
            cJSON *j_type = cJSON_GetObjectItemCaseSensitive(p, "type");
            cJSON *j_mfg = cJSON_GetObjectItemCaseSensitive(p, "manufacturer");
            cJSON *j_model = cJSON_GetObjectItemCaseSensitive(p, "model");
            cJSON *j_cp = cJSON_GetObjectItemCaseSensitive(p, "controlPort");
            cJSON *j_mqtt = cJSON_GetObjectItemCaseSensitive(p, "mqttTopic");
            if (!j_mqtt) j_mqtt = cJSON_GetObjectItemCaseSensitive(p, "MQTTTopic");
            cJSON *j_en = cJSON_GetObjectItemCaseSensitive(p, "mqttEnabled");
            if (!j_en) j_en = cJSON_GetObjectItemCaseSensitive(p, "MQTTEnabled");

            const char *dev_id = j_dev ? (cJSON_IsString(j_dev) ? j_dev->valuestring : "") : "";
            char dev_buf[32] = {0};
            if (!dev_id[0] && j_dev && cJSON_IsNumber(j_dev)) {
                snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)j_dev->valuedouble);
                dev_id = dev_buf;
            }
            int cp_val = 0;
            if (j_cp) {
                if (cJSON_IsNumber(j_cp)) cp_val = (int)j_cp->valuedouble;
                else if (cJSON_IsString(j_cp)) cp_val = atoi(j_cp->valuestring);
            }
            const char *mqtt_top = (j_mqtt && cJSON_IsString(j_mqtt)) ? j_mqtt->valuestring : NULL;

            cJSON *j_pulse = cJSON_GetObjectItemCaseSensitive(p, "mqttPulse");
            if (!j_pulse) j_pulse = cJSON_GetObjectItemCaseSensitive(p, "MQTTPulseMs");
            if (!j_pulse) j_pulse = cJSON_GetObjectItemCaseSensitive(p, "pulseMs");
            if (!j_pulse) j_pulse = cJSON_GetObjectItemCaseSensitive(p, "mqttPulseMs");
            int pulse_val = (j_pulse && cJSON_IsNumber(j_pulse)) ? (int)j_pulse->valuedouble : 0;
            if (!pulse_val && j_pulse && cJSON_IsString(j_pulse)) pulse_val = atoi(j_pulse->valuestring);

            int has_en = (j_en != NULL);
            int en_val = 0;
            if (has_en) {
                if (cJSON_IsTrue(j_en)) en_val = 1;
                else if (cJSON_IsNumber(j_en)) en_val = (j_en->valuedouble > 0) ? 1 : 0;
                else if (cJSON_IsString(j_en)) en_val = (strcmp(j_en->valuestring, "1") == 0 || strcasecmp(j_en->valuestring, "true") == 0) ? 1 : 0;
            }

            if (dev_id[0]) {
                cJSON *root = config_store_get_devices_json();
                if (!root) {
                    root = cJSON_CreateObject();
                    cJSON_AddItemToObject(root, "devices", cJSON_CreateArray());
                }
                cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                if (!devs) {
                    devs = cJSON_CreateArray();
                    cJSON_AddItemToObject(root, "devices", devs);
                }
                cJSON *found_dev = NULL;
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
                        found_dev = d;
                        break;
                    }
                }
                if (!found_dev) {
                    found_dev = cJSON_CreateObject();
                    cJSON_AddStringToObject(found_dev, "id", dev_id);
                    cJSON_AddStringToObject(found_dev, "name", (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : dev_id);
                    cJSON_AddStringToObject(found_dev, "type", (j_type && cJSON_IsString(j_type)) ? j_type->valuestring : "Generic");
                    cJSON_AddStringToObject(found_dev, "manufacturer", (j_mfg && cJSON_IsString(j_mfg)) ? j_mfg->valuestring : "");
                    cJSON_AddStringToObject(found_dev, "model", (j_model && cJSON_IsString(j_model)) ? j_model->valuestring : "");
                    cJSON_AddNumberToObject(found_dev, "controlPort", cp_val > 0 ? cp_val : 7);
                    cJSON_AddItemToObject(found_dev, "commands", cJSON_CreateArray());
                    cJSON_AddItemToArray(devs, found_dev);
                }
                if (j_cp != NULL) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "ControlPort");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "control_port");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "controlPort");
                    cJSON_AddNumberToObject(found_dev, "controlPort", cp_val);
                }
                cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "mqtt");
                cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "MQTTEnabled");
                cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "mqtt_enabled");
                if (has_en) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "mqttEnabled");
                    cJSON_AddNumberToObject(found_dev, "mqttEnabled", en_val);
                }
                if (mqtt_top) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "MQTTTopic");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "mqtt_topic");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "mqttTopic");
                    cJSON_AddStringToObject(found_dev, "mqttTopic", mqtt_top);
                }
                if (pulse_val > 0) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "MQTTPulseMs");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "mqtt_pulse_ms");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "mqttPulse");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "mqttPulseMs");
                    cJSON_AddNumberToObject(found_dev, "mqttPulseMs", pulse_val);
                }
                if (j_name && cJSON_IsString(j_name) && j_name->valuestring[0]) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "Name");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "name");
                    cJSON_AddStringToObject(found_dev, "name", j_name->valuestring);
                }
                if (j_type && cJSON_IsString(j_type)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "DeviceTypeDisplayName");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "deviceType");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "type");
                    cJSON_AddStringToObject(found_dev, "type", j_type->valuestring);
                }
                if (j_mfg && cJSON_IsString(j_mfg)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "Manufacturer");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "manufacturer");
                    cJSON_AddStringToObject(found_dev, "manufacturer", j_mfg->valuestring);
                }
                if (j_model && cJSON_IsString(j_model)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "Model");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "model");
                    cJSON_AddStringToObject(found_dev, "model", j_model->valuestring);
                }
                cJSON *j_icon = cJSON_GetObjectItemCaseSensitive(p, "icon");
                if (!j_icon) j_icon = cJSON_GetObjectItemCaseSensitive(p, "Icon");
                if (j_icon) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "Icon");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "icon");
                    if (cJSON_IsString(j_icon)) cJSON_AddStringToObject(found_dev, "icon", j_icon->valuestring);
                    else if (cJSON_IsNumber(j_icon)) cJSON_AddNumberToObject(found_dev, "icon", j_icon->valueint);
                }

                cJSON *j_idd = cJSON_GetObjectItemCaseSensitive(p, "interDeviceDelay");
                if (!j_idd) j_idd = cJSON_GetObjectItemCaseSensitive(p, "inter_device_delay");
                if (!j_idd) j_idd = cJSON_GetObjectItemCaseSensitive(p, "InterDeviceDelay");
                if (j_idd) {
                    int val = cJSON_IsNumber(j_idd) ? (int)j_idd->valuedouble : atoi(j_idd->valuestring);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "inter_device_delay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "InterDeviceDelay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "interDeviceDelay");
                    cJSON_AddNumberToObject(found_dev, "interDeviceDelay", val);
                }

                cJSON *j_ikd = cJSON_GetObjectItemCaseSensitive(p, "interKeyDelay");
                if (!j_ikd) j_ikd = cJSON_GetObjectItemCaseSensitive(p, "inter_key_delay");
                if (!j_ikd) j_ikd = cJSON_GetObjectItemCaseSensitive(p, "InterKeyDelay");
                if (j_ikd) {
                    int val = cJSON_IsNumber(j_ikd) ? (int)j_ikd->valuedouble : atoi(j_ikd->valuestring);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "inter_key_delay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "InterKeyDelay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "interKeyDelay");
                    cJSON_AddNumberToObject(found_dev, "interKeyDelay", val);
                }

                cJSON *j_hikd = cJSON_GetObjectItemCaseSensitive(p, "holdInterKeyDelay");
                if (!j_hikd) j_hikd = cJSON_GetObjectItemCaseSensitive(p, "hold_inter_key_delay");
                if (!j_hikd) j_hikd = cJSON_GetObjectItemCaseSensitive(p, "HoldInterKeyDelay");
                if (j_hikd) {
                    int val = cJSON_IsNumber(j_hikd) ? (int)j_hikd->valuedouble : atoi(j_hikd->valuestring);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "hold_inter_key_delay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "HoldInterKeyDelay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "holdInterKeyDelay");
                    cJSON_AddNumberToObject(found_dev, "holdInterKeyDelay", val);
                }

                cJSON *j_pod = cJSON_GetObjectItemCaseSensitive(p, "powerOnDelay");
                if (!j_pod) j_pod = cJSON_GetObjectItemCaseSensitive(p, "power_on_delay");
                if (!j_pod) j_pod = cJSON_GetObjectItemCaseSensitive(p, "PowerOnDelay");
                if (j_pod) {
                    int val = cJSON_IsNumber(j_pod) ? (int)j_pod->valuedouble : atoi(j_pod->valuestring);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "power_on_delay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "PowerOnDelay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "powerOnDelay");
                    cJSON_AddNumberToObject(found_dev, "powerOnDelay", val);
                }

                cJSON *j_pao = cJSON_GetObjectItemCaseSensitive(p, "isPowerAlwaysOn");
                if (!j_pao) j_pao = cJSON_GetObjectItemCaseSensitive(p, "is_power_always_on");
                if (!j_pao) j_pao = cJSON_GetObjectItemCaseSensitive(p, "IsPowerAlwaysOn");
                if (j_pao) {
                    int val = cJSON_IsTrue(j_pao) || (cJSON_IsNumber(j_pao) && j_pao->valuedouble > 0) || (cJSON_IsString(j_pao) && (strcmp(j_pao->valuestring, "1") == 0 || strcasecmp(j_pao->valuestring, "true") == 0));
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "is_power_always_on");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "IsPowerAlwaysOn");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "isPowerAlwaysOn");
                    cJSON_AddBoolToObject(found_dev, "isPowerAlwaysOn", val);
                }

                cJSON *j_bthost = cJSON_GetObjectItemCaseSensitive(p, "bdaddr");
                if (!j_bthost) j_bthost = cJSON_GetObjectItemCaseSensitive(p, "btHost");
                if (!j_bthost) j_bthost = cJSON_GetObjectItemCaseSensitive(p, "bt_host");
                if (!j_bthost) j_bthost = cJSON_GetObjectItemCaseSensitive(p, "BTAddress");
                if (j_bthost && cJSON_IsString(j_bthost)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bt_host");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btHost");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "BTAddress");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bdaddr");
                    cJSON_AddStringToObject(found_dev, "bdaddr", j_bthost->valuestring);
                }

                cJSON *j_btrec = cJSON_GetObjectItemCaseSensitive(p, "btAutoReconnect");
                if (!j_btrec) j_btrec = cJSON_GetObjectItemCaseSensitive(p, "bt_auto_reconnect");
                if (j_btrec) {
                    int val = 1;
                    if (cJSON_IsNumber(j_btrec)) val = (int)j_btrec->valuedouble ? 1 : 0;
                    else if (cJSON_IsBool(j_btrec)) val = cJSON_IsTrue(j_btrec) ? 1 : 0;
                    else if (cJSON_IsString(j_btrec)) val = atoi(j_btrec->valuestring) ? 1 : 0;
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bt_auto_reconnect");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btAutoReconnect");
                    cJSON_AddNumberToObject(found_dev, "btAutoReconnect", val);
                }

                cJSON *j_btdur = cJSON_GetObjectItemCaseSensitive(p, "btReconnectDurationSec");
                if (!j_btdur) j_btdur = cJSON_GetObjectItemCaseSensitive(p, "btReconnectDuration");
                if (!j_btdur) j_btdur = cJSON_GetObjectItemCaseSensitive(p, "bt_reconnect_duration_sec");
                if (j_btdur) {
                    int val = cJSON_IsNumber(j_btdur) ? (int)j_btdur->valuedouble : atoi(j_btdur->valuestring);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bt_reconnect_duration_sec");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btReconnectDuration");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btReconnectDurationSec");
                    cJSON_AddNumberToObject(found_dev, "btReconnectDurationSec", val);
                }

                cJSON *j_btdly = cJSON_GetObjectItemCaseSensitive(p, "btReconnectDelaySec");
                if (!j_btdly) j_btdly = cJSON_GetObjectItemCaseSensitive(p, "btReconnectDelay");
                if (!j_btdly) j_btdly = cJSON_GetObjectItemCaseSensitive(p, "bt_reconnect_delay_sec");
                if (j_btdly) {
                    int val = cJSON_IsNumber(j_btdly) ? (int)j_btdly->valuedouble : atoi(j_btdly->valuestring);
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bt_reconnect_delay_sec");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btReconnectDelay");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btReconnectDelaySec");
                    cJSON_AddNumberToObject(found_dev, "btReconnectDelaySec", val);
                }

                cJSON *j_trig_dev = cJSON_GetObjectItemCaseSensitive(p, "btTriggerDeviceId");
                if (!j_trig_dev) j_trig_dev = cJSON_GetObjectItemCaseSensitive(p, "bt_trigger_device_id");
                if (j_trig_dev && cJSON_IsString(j_trig_dev)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bt_trigger_device_id");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btTriggerDeviceId");
                    if (j_trig_dev->valuestring[0]) {
                        cJSON_AddStringToObject(found_dev, "btTriggerDeviceId", j_trig_dev->valuestring);
                    }
                }

                cJSON *j_trig_cmd = cJSON_GetObjectItemCaseSensitive(p, "btTriggerCommandName");
                if (!j_trig_cmd) j_trig_cmd = cJSON_GetObjectItemCaseSensitive(p, "bt_trigger_command_name");
                if (!j_trig_cmd) j_trig_cmd = cJSON_GetObjectItemCaseSensitive(p, "bt_trigger_command");
                if (!j_trig_cmd) j_trig_cmd = cJSON_GetObjectItemCaseSensitive(p, "btTriggerCommand");
                if (j_trig_cmd && cJSON_IsString(j_trig_cmd)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bt_trigger_command_name");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "bt_trigger_command");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btTriggerCommand");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "btTriggerCommandName");
                    if (j_trig_cmd->valuestring[0]) {
                        cJSON_AddStringToObject(found_dev, "btTriggerCommandName", j_trig_cmd->valuestring);
                    }
                }

                cJSON *j_po = cJSON_GetObjectItemCaseSensitive(p, "powerOnSequence");
                if (!j_po) j_po = cJSON_GetObjectItemCaseSensitive(p, "power_on_sequence");
                if (!j_po) j_po = cJSON_GetObjectItemCaseSensitive(p, "PowerOnActions");
                if (j_po && cJSON_IsArray(j_po)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "power_on_sequence");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "PowerOnActions");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "powerOnSequence");
                    cJSON *po_copy = cJSON_Duplicate(j_po, 1);
                    sanitize_activity_steps(po_copy);
                    cJSON_AddItemToObject(found_dev, "powerOnSequence", po_copy);
                }

                cJSON *j_pf = cJSON_GetObjectItemCaseSensitive(p, "powerOffSequence");
                if (!j_pf) j_pf = cJSON_GetObjectItemCaseSensitive(p, "power_off_sequence");
                if (!j_pf) j_pf = cJSON_GetObjectItemCaseSensitive(p, "PowerOffActions");
                if (j_pf && cJSON_IsArray(j_pf)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "power_off_sequence");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "PowerOffActions");
                    cJSON_DeleteItemFromObjectCaseSensitive(found_dev, "powerOffSequence");
                    cJSON *pf_copy = cJSON_Duplicate(j_pf, 1);
                    sanitize_activity_steps(pf_copy);
                    cJSON_AddItemToObject(found_dev, "powerOffSequence", pf_copy);
                }

                config_store_save_devices(root);
                success = 1;
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

    /* Route: POST/DELETE /ir/delete-device or /api/device-delete */
    if ((strcmp(clean_path, "/ir/delete-device") == 0 || strcmp(clean_path, "/api/device-delete") == 0) &&
        (strcmp(method, "POST") == 0 || strcmp(method, "DELETE") == 0)) {
        cJSON *p = parse_request_params(body);
        if (!p && query) {
            p = parse_request_params(query);
        }
        int success = 0;
        if (p) {
            cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(p, "deviceId");
            if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(p, "id");
            const char *dev_id = j_dev ? (cJSON_IsString(j_dev) ? j_dev->valuestring : "") : "";
            char dev_buf[64] = {0};
            if (!dev_id[0] && j_dev && cJSON_IsNumber(j_dev)) {
                snprintf(dev_buf, sizeof(dev_buf), "%ld", (long)j_dev->valuedouble);
                dev_id = dev_buf;
            }
            if (!dev_id[0] && query) {
                const char *q = strstr(query, "id=");
                if (q) {
                    strncpy(dev_buf, q + 3, sizeof(dev_buf) - 1);
                    char *amp = strchr(dev_buf, '&');
                    if (amp) *amp = '\0';
                    dev_id = dev_buf;
                }
            }
            if (dev_id[0]) {
                cJSON *root = config_store_get_devices_json();
                if (root) {
                    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
                    if (!devs) devs = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
                    if (devs && cJSON_IsArray(devs)) {
                        int d_count = cJSON_GetArraySize(devs);
                        for (int di = 0; di < d_count; di++) {
                            cJSON *dev_item = cJSON_GetArrayItem(devs, di);
                            cJSON *d = cJSON_GetObjectItemCaseSensitive(dev_item, "Device");
                            if (!d) d = dev_item;
                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id-");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d, "Id");
                            char d_id_str[64] = {0};
                            if (jid && cJSON_IsNumber(jid)) snprintf(d_id_str, sizeof(d_id_str), "%ld", (long)jid->valuedouble);
                            else if (jid && cJSON_IsString(jid)) strncpy(d_id_str, jid->valuestring, sizeof(d_id_str) - 1);
                            if (strcmp(d_id_str, dev_id) == 0) {
                                cJSON_DeleteItemFromArray(devs, di);
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

    return 0;
}
