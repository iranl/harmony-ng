#define _GNU_SOURCE
#include "config_store.h"
#include "config_sanitize.h"
#include "bt_common.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <strings.h>
#include <time.h>

extern int g_dry_run;

static cJSON *g_devices_json_cache = NULL;
static cJSON *g_activities_json_cache = NULL;
static pthread_mutex_t g_cfg_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

char *read_file_text(const char *path, size_t max_bytes) {
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || (size_t)sz > max_bytes) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';
    return buf;
}

int write_file_atomic(const char *path, const char *content) {
    if (!path || !content) return -1;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return rename(tmp, path);
}

const char *config_store_path(const char *rel_path) {
    static __thread char s_buf[256];
    if (!rel_path) return "/data/harmony";
    if (rel_path[0] == '/') return rel_path;
    snprintf(s_buf, sizeof(s_buf), "/data/harmony/%s", rel_path);
    return s_buf;
}

int config_store_init(void) {
    mkdir(HARMONY_DIR, 0755);
    mkdir(HARMONY_CONFIG_DIR, 0755);

    pthread_mutex_lock(&g_cfg_lock);
    if (g_devices_json_cache) { cJSON_Delete(g_devices_json_cache); g_devices_json_cache = NULL; }
    if (g_activities_json_cache) { cJSON_Delete(g_activities_json_cache); g_activities_json_cache = NULL; }

    const char *dev_path = HARMONY_DEVICES_FILE;
    char *dev_raw = read_file_text(dev_path, 4 * 1024 * 1024);
    if (dev_raw) {
        g_devices_json_cache = cJSON_Parse(dev_raw);
        free(dev_raw);
        if (g_devices_json_cache) {
            sanitize_devices_json(g_devices_json_cache);
        }
    }
    if (!g_devices_json_cache) {
        g_devices_json_cache = cJSON_CreateObject();
        cJSON_AddArrayToObject(g_devices_json_cache, "devices");
    }

    const char *act_path = HARMONY_ACTIVITIES_FILE;
    char *act_raw = read_file_text(act_path, 2 * 1024 * 1024);
    if (act_raw) {
        g_activities_json_cache = cJSON_Parse(act_raw);
        free(act_raw);
        if (g_activities_json_cache) {
            sanitize_activities_json(g_activities_json_cache);
        }
    }
    if (!g_activities_json_cache) {
        g_activities_json_cache = cJSON_CreateObject();
        cJSON_AddArrayToObject(g_activities_json_cache, "activities");
    }
    pthread_mutex_unlock(&g_cfg_lock);
    return 0;
}

cJSON *config_store_get_devices_json(void) {
    pthread_mutex_lock(&g_cfg_lock);
    if (!g_devices_json_cache) {
        pthread_mutex_unlock(&g_cfg_lock);
        config_store_init();
        pthread_mutex_lock(&g_cfg_lock);
    }
    cJSON *res = g_devices_json_cache;
    pthread_mutex_unlock(&g_cfg_lock);
    return res;
}

cJSON *config_store_get_activities_json(void) {
    pthread_mutex_lock(&g_cfg_lock);
    if (!g_activities_json_cache) {
        pthread_mutex_unlock(&g_cfg_lock);
        config_store_init();
        pthread_mutex_lock(&g_cfg_lock);
    }
    cJSON *res = g_activities_json_cache;
    pthread_mutex_unlock(&g_cfg_lock);
    return res;
}

int config_store_save_devices(cJSON *devices_json) {
    if (!devices_json) return -1;
    sanitize_devices_json(devices_json);
    char *out = cJSON_Print(devices_json);
    if (!out) return -1;
    pthread_mutex_lock(&g_cfg_lock);
    write_file_atomic(HARMONY_DEVICES_FILE, out);
    free(out);
    if (g_devices_json_cache && g_devices_json_cache != devices_json) {
        cJSON_Delete(g_devices_json_cache);
    }
    g_devices_json_cache = devices_json;
    pthread_mutex_unlock(&g_cfg_lock);
    notify_rf_push_config();
    return 0;
}

int config_store_save_activities(cJSON *activities_json) {
    if (!activities_json) return -1;
    sanitize_activities_json(activities_json);
    char *out = cJSON_Print(activities_json);
    if (!out) return -1;
    pthread_mutex_lock(&g_cfg_lock);
    write_file_atomic(HARMONY_ACTIVITIES_FILE, out);
    free(out);
    if (g_activities_json_cache && g_activities_json_cache != activities_json) {
        cJSON_Delete(g_activities_json_cache);
    }
    g_activities_json_cache = activities_json;
    pthread_mutex_unlock(&g_cfg_lock);
    notify_rf_push_config();
    return 0;
}

void notify_rf_push_config(void) {
    FILE *f = fopen("/data/harmony/rf_sync_pending.flag", "w");
    if (f) {
        fprintf(f, "%ld\n", (long)time(NULL));
        fclose(f);
    }
}

void config_store_invalidate_cache(void) {
    pthread_mutex_lock(&g_cfg_lock);
    if (g_devices_json_cache) { cJSON_Delete(g_devices_json_cache); g_devices_json_cache = NULL; }
    if (g_activities_json_cache) { cJSON_Delete(g_activities_json_cache); g_activities_json_cache = NULL; }
    pthread_mutex_unlock(&g_cfg_lock);
}

static void parse_steps(const cJSON *arr, const char *dev_id, struct activity_step **steps_out, int *count_out) {
    *steps_out = NULL;
    *count_out = 0;
    if (!arr || !cJSON_IsArray(arr)) return;
    int sz = cJSON_GetArraySize(arr);
    if (sz <= 0) return;
    struct activity_step *steps = (struct activity_step *)calloc((size_t)sz, sizeof(*steps));
    if (!steps) return;
    int cnt = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (dev_id && dev_id[0]) {
            strncpy(steps[cnt].device_id, dev_id, sizeof(steps[cnt].device_id) - 1);
        }
        const cJSON *order = cJSON_GetObjectItemCaseSensitive(item, "order");
        if (order && cJSON_IsNumber(order)) steps[cnt].order = order->valueint;
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(item, "command");
        if (cmd && cJSON_IsString(cmd) && cmd->valuestring) {
            strncpy(steps[cnt].command, cmd->valuestring, sizeof(steps[cnt].command) - 1);
        }
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (type && cJSON_IsString(type) && type->valuestring) {
            strncpy(steps[cnt].type, type->valuestring, sizeof(steps[cnt].type) - 1);
        } else {
            strncpy(steps[cnt].type, "IRCommand", sizeof(steps[cnt].type) - 1);
        }
        const cJSON *del = cJSON_GetObjectItemCaseSensitive(item, "delay");
        if (del && cJSON_IsNumber(del)) steps[cnt].delay_ms = del->valueint;
        cnt++;
    }
    *steps_out = steps;
    *count_out = cnt;
}

int load_ir_inventory(struct ir_inventory *inv) {
    if (!inv) return -1;
    memset(inv, 0, sizeof(*inv));

    cJSON *root = config_store_get_devices_json();
    if (!root) return -1;

    /* Check clean format: { "devices": [ ... ] } */
    cJSON *clean_devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (clean_devs && cJSON_IsArray(clean_devs)) {
        const cJSON *d = NULL;
        cJSON_ArrayForEach(d, clean_devs) {
            if (inv->device_count >= MAX_IR_DEVICES) break;
            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
            long id = (jid && cJSON_IsNumber(jid)) ? (long)jid->valuedouble : 0;
            if (id <= 0 && jid && cJSON_IsString(jid) && jid->valuestring) id = atol(jid->valuestring);
            if (id <= 0) continue;

            struct ir_device *dev = &inv->devices[inv->device_count];
            snprintf(dev->id, sizeof(dev->id), "%ld", id);
            if (id > inv->max_device_id) inv->max_device_id = id;

            const cJSON *name = cJSON_GetObjectItemCaseSensitive(d, "name");
            if (name && cJSON_IsString(name) && name->valuestring) {
                strncpy(dev->name, name->valuestring, sizeof(dev->name) - 1);
            }
            const cJSON *mfg = cJSON_GetObjectItemCaseSensitive(d, "manufacturer");
            if (mfg && cJSON_IsString(mfg) && mfg->valuestring) {
                strncpy(dev->manufacturer, mfg->valuestring, sizeof(dev->manufacturer) - 1);
            }
            const cJSON *model = cJSON_GetObjectItemCaseSensitive(d, "model");
            if (model && cJSON_IsString(model) && model->valuestring) {
                strncpy(dev->model, model->valuestring, sizeof(dev->model) - 1);
            }
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(d, "type");
            if (type && cJSON_IsString(type) && type->valuestring) {
                strncpy(dev->type, type->valuestring, sizeof(dev->type) - 1);
            }

            const cJSON *cp = cJSON_GetObjectItemCaseSensitive(d, "controlPort");
            dev->control_port = (cp && cJSON_IsNumber(cp)) ? cp->valueint : 7;
            const cJSON *tr = cJSON_GetObjectItemCaseSensitive(d, "transport");
            dev->transport = (tr && cJSON_IsNumber(tr)) ? tr->valueint : 1;
            const cJSON *idd = cJSON_GetObjectItemCaseSensitive(d, "interDeviceDelay");
            dev->inter_device_delay = (idd && cJSON_IsNumber(idd)) ? idd->valueint : 500;
            const cJSON *ikd = cJSON_GetObjectItemCaseSensitive(d, "interKeyDelay");
            dev->inter_key_delay = (ikd && cJSON_IsNumber(ikd)) ? ikd->valueint : 100;
            const cJSON *hikd = cJSON_GetObjectItemCaseSensitive(d, "holdInterKeyDelay");
            dev->hold_inter_key_delay = (hikd && cJSON_IsNumber(hikd)) ? hikd->valueint : 200;
            const cJSON *didd = cJSON_GetObjectItemCaseSensitive(d, "defaultInterDeviceDelay");
            dev->default_inter_device_delay = (didd && cJSON_IsNumber(didd)) ? didd->valueint : 500;
            const cJSON *pod = cJSON_GetObjectItemCaseSensitive(d, "powerOnDelay");
            dev->power_on_delay = (pod && cJSON_IsNumber(pod)) ? pod->valueint : 1500;
            const cJSON *pao = cJSON_GetObjectItemCaseSensitive(d, "isPowerAlwaysOn");
            dev->is_power_always_on = (pao && (cJSON_IsTrue(pao) || (cJSON_IsNumber(pao) && pao->valueint > 0))) ? 1 : 0;

            const cJSON *bar = cJSON_GetObjectItemCaseSensitive(d, "btAutoReconnect");
            if (bar) {
                if (cJSON_IsNumber(bar)) dev->bt_auto_reconnect = bar->valueint;
                else if (cJSON_IsBool(bar)) dev->bt_auto_reconnect = cJSON_IsTrue(bar) ? 1 : 0;
                else dev->bt_auto_reconnect = 1;
            } else {
                dev->bt_auto_reconnect = 1;
            }

            const cJSON *brdur = cJSON_GetObjectItemCaseSensitive(d, "btReconnectDurationSec");
            dev->bt_reconnect_duration_sec = (brdur && cJSON_IsNumber(brdur)) ? brdur->valueint : 0;

            const cJSON *brdel = cJSON_GetObjectItemCaseSensitive(d, "btReconnectDelaySec");
            dev->bt_reconnect_delay_sec = (brdel && cJSON_IsNumber(brdel)) ? brdel->valueint : -1;

            const cJSON *j_btd = cJSON_GetObjectItemCaseSensitive(d, "btTriggerDeviceId");
            if (j_btd && cJSON_IsString(j_btd) && j_btd->valuestring) {
                strncpy(dev->bt_trigger_device_id, j_btd->valuestring, sizeof(dev->bt_trigger_device_id) - 1);
            }
            const cJSON *j_btc = cJSON_GetObjectItemCaseSensitive(d, "btTriggerCommandName");
            if (j_btc && cJSON_IsString(j_btc) && j_btc->valuestring) {
                const char *colon = strchr(j_btc->valuestring, ':');
                if (colon && !dev->bt_trigger_device_id[0]) {
                    size_t did_len = (size_t)(colon - j_btc->valuestring);
                    if (did_len >= sizeof(dev->bt_trigger_device_id)) did_len = sizeof(dev->bt_trigger_device_id) - 1;
                    strncpy(dev->bt_trigger_device_id, j_btc->valuestring, did_len);
                    dev->bt_trigger_device_id[did_len] = '\0';
                    strncpy(dev->bt_trigger_command_name, colon + 1, sizeof(dev->bt_trigger_command_name) - 1);
                } else {
                    strncpy(dev->bt_trigger_command_name, j_btc->valuestring, sizeof(dev->bt_trigger_command_name) - 1);
                }
            }

            const cJSON *po = cJSON_GetObjectItemCaseSensitive(d, "powerOnSequence");
            if (po) parse_steps(po, dev->id, &dev->power_on_steps, &dev->power_on_count);

            const cJSON *pf = cJSON_GetObjectItemCaseSensitive(d, "powerOffSequence");
            if (pf) parse_steps(pf, dev->id, &dev->power_off_steps, &dev->power_off_count);

            const cJSON *mqe = cJSON_GetObjectItemCaseSensitive(d, "mqttEnabled");
            const cJSON *mqt = cJSON_GetObjectItemCaseSensitive(d, "mqttTopic");
            const cJSON *mqp = cJSON_GetObjectItemCaseSensitive(d, "mqttPulseMs");
            if (mqe || mqt || mqp) {
                dev->mqtt_enabled = (mqe && (cJSON_IsTrue(mqe) || (cJSON_IsNumber(mqe) && mqe->valueint > 0))) ? 1 : 0;
                if (mqt && cJSON_IsString(mqt) && mqt->valuestring) strncpy(dev->mqtt_topic, mqt->valuestring, sizeof(dev->mqtt_topic) - 1);
                dev->mqtt_pulse_ms = (mqp && cJSON_IsNumber(mqp) && mqp->valueint > 0) ? mqp->valueint : 1000;
            } else {
                const cJSON *mq = cJSON_GetObjectItemCaseSensitive(d, "mqtt");
                if (mq && cJSON_IsObject(mq)) {
                    const cJSON *sub_en = cJSON_GetObjectItemCaseSensitive(mq, "enabled");
                    dev->mqtt_enabled = (sub_en && (cJSON_IsTrue(sub_en) || (cJSON_IsNumber(sub_en) && sub_en->valueint > 0))) ? 1 : 0;
                    const cJSON *sub_top = cJSON_GetObjectItemCaseSensitive(mq, "topic");
                    if (sub_top && cJSON_IsString(sub_top) && sub_top->valuestring) strncpy(dev->mqtt_topic, sub_top->valuestring, sizeof(dev->mqtt_topic) - 1);
                    const cJSON *sub_pls = cJSON_GetObjectItemCaseSensitive(mq, "pulseMs");
                    dev->mqtt_pulse_ms = (sub_pls && cJSON_IsNumber(sub_pls) && sub_pls->valueint > 0) ? sub_pls->valueint : 1000;
                }
            }

            const cJSON *cmds = cJSON_GetObjectItemCaseSensitive(d, "commands");
            if (cmds && cJSON_IsArray(cmds)) {
                int csz = cJSON_GetArraySize(cmds);
                if (csz > 0) {
                    dev->commands = (struct ir_command *)calloc((size_t)csz, sizeof(struct ir_command));
                    if (dev->commands) {
                        dev->command_cap = csz;
                        const cJSON *c = NULL;
                        cJSON_ArrayForEach(c, cmds) {
                            if (dev->command_count >= dev->command_cap) break;
                            struct ir_command *cmd = &dev->commands[dev->command_count];
                            const cJSON *cname = cJSON_GetObjectItemCaseSensitive(c, "name");
                            if (cname && cJSON_IsString(cname) && cname->valuestring) {
                                strncpy(cmd->name, cname->valuestring, sizeof(cmd->name) - 1);
                            }
                            const cJSON *ckey = cJSON_GetObjectItemCaseSensitive(c, "keyCode");
                            if (ckey && cJSON_IsString(ckey) && ckey->valuestring) {
                                strncpy(cmd->keycode, ckey->valuestring, sizeof(cmd->keycode) - 1);
                            }
                            const cJSON *craw = cJSON_GetObjectItemCaseSensitive(c, "raw");
                            if (craw && cJSON_IsString(craw) && craw->valuestring) {
                                strncpy(cmd->raw, craw->valuestring, sizeof(cmd->raw) - 1);
                                cmd->has_raw = 1;
                            }
                            dev->command_count++;
                        }
                    }
                }
            }

            inv->device_count++;
        }
        return 0;
    }

    return 0;
}

void free_ir_inventory(struct ir_inventory *inv) {
    if (!inv) return;
    for (int i = 0; i < inv->device_count; i++) {
        if (inv->devices[i].commands) free(inv->devices[i].commands);
        if (inv->devices[i].power_on_steps) free(inv->devices[i].power_on_steps);
        if (inv->devices[i].power_off_steps) free(inv->devices[i].power_off_steps);
    }
    memset(inv, 0, sizeof(*inv));
}

static void parse_activity_steps_array(const cJSON *arr, struct activity_step *steps, int max_steps, int *count_out) {
    *count_out = 0;
    if (!arr || !cJSON_IsArray(arr) || !steps || max_steps <= 0) return;
    int cnt = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (cnt >= max_steps) break;
        struct activity_step *st = &steps[cnt];
        memset(st, 0, sizeof(*st));

        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "deviceId");
        if (jid) {
            if (cJSON_IsNumber(jid)) snprintf(st->device_id, sizeof(st->device_id), "%ld", (long)jid->valuedouble);
            else if (cJSON_IsString(jid) && jid->valuestring) strncpy(st->device_id, jid->valuestring, sizeof(st->device_id) - 1);
        }

        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(item, "command");
        if (cmd && cJSON_IsString(cmd) && cmd->valuestring) {
            strncpy(st->command, cmd->valuestring, sizeof(st->command) - 1);
        }

        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (type && cJSON_IsString(type) && type->valuestring) {
            strncpy(st->type, type->valuestring, sizeof(st->type) - 1);
        } else {
            strncpy(st->type, "IRCommand", sizeof(st->type) - 1);
        }
        if (strcasecmp(st->type, "Switched") == 0 || strcasecmp(st->command, "Switched") == 0) {
            strncpy(st->type, "Switched", sizeof(st->type) - 1);
            if (!st->command[0]) strncpy(st->command, "Switched", sizeof(st->command) - 1);
        }

        const cJSON *del = cJSON_GetObjectItemCaseSensitive(item, "delay");
        if (del) {
            if (cJSON_IsNumber(del)) st->delay_ms = (int)del->valuedouble;
            else if (cJSON_IsString(del) && del->valuestring) st->delay_ms = atoi(del->valuestring);
        }

        const cJSON *order = cJSON_GetObjectItemCaseSensitive(item, "order");
        if (order && cJSON_IsNumber(order)) st->order = (int)order->valuedouble;

        cnt++;
    }
    *count_out = cnt;
}

int load_activity_inventory(struct activity_inventory *inv) {
    if (!inv) return -1;
    memset(inv, 0, sizeof(*inv));
    strncpy(inv->current_id, "-1", sizeof(inv->current_id) - 1);
    strncpy(inv->current_name, "PowerOff", sizeof(inv->current_name) - 1);

    const char *cur_path = "/data/harmony/current_activity";
    FILE *cf = fopen(cur_path, "r");
    if (cf) {
        if (fscanf(cf, "%31s", inv->current_id) != 1) strncpy(inv->current_id, "-1", sizeof(inv->current_id) - 1);
        fclose(cf);
    }

    cJSON *root = config_store_get_activities_json();
    if (!root) return -1;

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "activities");
    if (arr && cJSON_IsArray(arr)) {
        const cJSON *a = NULL;
        cJSON_ArrayForEach(a, arr) {
            if (inv->count >= MAX_ACTIVITIES) break;

            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(a, "id");
            long id = (jid && cJSON_IsNumber(jid)) ? (long)jid->valuedouble : 0;
            if (id == 0 && jid && cJSON_IsString(jid) && jid->valuestring) id = atol(jid->valuestring);
            if (id == 0) continue;

            struct activity_item *act = &inv->items[inv->count];
            snprintf(act->id, sizeof(act->id), "%ld", id);
            if (id > 0 && id > inv->max_id) inv->max_id = id;

            const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
            if (name && cJSON_IsString(name) && name->valuestring) {
                strncpy(act->name, name->valuestring, sizeof(act->name) - 1);
            } else if (id == -1) {
                strncpy(act->name, "PowerOff", sizeof(act->name) - 1);
            }
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(a, "type");
            if (type && cJSON_IsString(type) && type->valuestring) {
                strncpy(act->type, type->valuestring, sizeof(act->type) - 1);
            }

            const cJSON *devs = cJSON_GetObjectItemCaseSensitive(a, "deviceIds");
            if (devs && cJSON_IsArray(devs)) {
                const cJSON *d = NULL;
                cJSON_ArrayForEach(d, devs) {
                    if (act->device_count >= 16) break;
                    if (cJSON_IsString(d) && d->valuestring) {
                        strncpy(act->device_ids[act->device_count++], d->valuestring, 31);
                    } else if (cJSON_IsNumber(d)) {
                        snprintf(act->device_ids[act->device_count++], 31, "%ld", (long)d->valuedouble);
                    }
                }
            }

            const cJSON *start_arr = cJSON_GetObjectItemCaseSensitive(a, "startSequence");
            parse_activity_steps_array(start_arr, act->start_steps, MAX_ACTIVITY_STEPS, &act->start_count);

            const cJSON *stop_arr = cJSON_GetObjectItemCaseSensitive(a, "stopSequence");
            parse_activity_steps_array(stop_arr, act->stop_steps, MAX_ACTIVITY_STEPS, &act->stop_count);

            /* Extract any devices present in actions if not yet recorded */
            for (int s = 0; s < act->start_count && act->device_count < 16; s++) {
                if (act->start_steps[s].device_id[0] && strcmp(act->start_steps[s].device_id, "0") != 0) {
                    int dup = 0;
                    for (int di = 0; di < act->device_count; di++) {
                        if (strcmp(act->device_ids[di], act->start_steps[s].device_id) == 0) { dup = 1; break; }
                    }
                    if (!dup) strncpy(act->device_ids[act->device_count++], act->start_steps[s].device_id, 31);
                }
            }

            if (strcmp(act->id, inv->current_id) == 0 && act->name[0]) {
                strncpy(inv->current_name, act->name, sizeof(inv->current_name) - 1);
            }

            inv->count++;
        }
    }
    return 0;
}

int run_bt_saved_script(const char *type, const char *bdaddr, const char *script,
                        int gap_ms, char *out, size_t outlen) {
    (void)type;
    if (out && outlen) out[0] = '\0';
    if (!script || !script[0]) return -1;

    if (g_dry_run) {
        printf("[DRY-RUN] BT Send simulated: type=%s bdaddr=%s script=%s\n", type ? type : "", bdaddr ? bdaddr : "", script);
        if (out && outlen) snprintf(out, outlen, "[DRY-RUN] Sent key %s", script);
        return 0;
    }

    int fd = open(BT_TEXT_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        if (bdaddr && bdaddr[0]) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "connect_host %s\n", bdaddr);
            hub_send_btstack_cmd(cmd);
        } else {
            hub_send_btstack_cmd("connect_host\n");
        }
        if (out && outlen) snprintf(out, outlen, "Bluetooth host not connected; initiating connection");
        return -1;
    }

    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "KEY %s\n", script);
    write(fd, buf, (size_t)len);
    close(fd);

    if (gap_ms > 0) usleep((useconds_t)gap_ms * 1000);
    if (out && outlen) snprintf(out, outlen, "Sent key %s", script);
    return 0;
}

int save_bthid_target(const char *type, const char *bdaddr) {
    if (!bdaddr || !bdaddr[0]) return -1;
    const char *target_path = "/data/harmony/bthid_target";
    FILE *f = fopen(target_path, "w");
    if (!f) return -1;
    fprintf(f, "%s %s\n", (type && type[0]) ? type : "btkeyboard", bdaddr);
    fclose(f);
    return 0;
}

int load_bt_inventory(struct bt_inventory *inv) {
    if (!inv) return -1;
    memset(inv, 0, sizeof(*inv));
    const char *bt_path = "/data/harmony/bt-devices.json";
    char *raw = read_file_text(bt_path, 1024 * 1024);
    if (!raw) return 0;
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return 0;

    cJSON *devs = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (devs && cJSON_IsArray(devs)) {
        const cJSON *d = NULL;
        cJSON_ArrayForEach(d, devs) {
            if (inv->device_count >= 12) break;
            struct bt_saved_device *bdev = &inv->devices[inv->device_count];
            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
            if (jid && cJSON_IsString(jid) && jid->valuestring) {
                strncpy(bdev->id, jid->valuestring, sizeof(bdev->id) - 1);
            }
            const cJSON *jname = cJSON_GetObjectItemCaseSensitive(d, "name");
            if (jname && cJSON_IsString(jname) && jname->valuestring) {
                strncpy(bdev->name, jname->valuestring, sizeof(bdev->name) - 1);
            }
            const cJSON *jaddr = cJSON_GetObjectItemCaseSensitive(d, "bdaddr");
            if (!jaddr) jaddr = cJSON_GetObjectItemCaseSensitive(d, "address");
            if (jaddr && cJSON_IsString(jaddr) && jaddr->valuestring) {
                strncpy(bdev->bdaddr, jaddr->valuestring, sizeof(bdev->bdaddr) - 1);
            }
            const cJSON *jtype = cJSON_GetObjectItemCaseSensitive(d, "type");
            if (jtype && cJSON_IsString(jtype) && jtype->valuestring) {
                strncpy(bdev->type, jtype->valuestring, sizeof(bdev->type) - 1);
            }
            inv->device_count++;
        }
    }
    cJSON_Delete(root);
    return 0;
}

int config_store_get_settings(struct hub_settings *settings) {
    if (!settings) return -1;
    settings->dry_run = 0;
    strncpy(settings->hub_name, "Harmony Hub", sizeof(settings->hub_name) - 1);
    settings->hub_name[sizeof(settings->hub_name) - 1] = '\0';
    settings->ntp_server[0] = '\0';
    strncpy(settings->timezone, "UTC", sizeof(settings->timezone) - 1);
    strncpy(settings->led_mode, "always_on", sizeof(settings->led_mode) - 1);

    const char *settings_path = HARMONY_SETTINGS_FILE;
    char *raw = read_file_text(settings_path, 64 * 1024);
    if (!raw) return 0;
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return 0;

    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "dry_run");
    if (item && cJSON_IsBool(item)) settings->dry_run = cJSON_IsTrue(item) ? 1 : 0;
    else if (item && cJSON_IsNumber(item)) settings->dry_run = (int)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(root, "hub_name");
    if (item && cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        strncpy(settings->hub_name, item->valuestring, sizeof(settings->hub_name) - 1);
        settings->hub_name[sizeof(settings->hub_name) - 1] = '\0';
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "ntp_server");
    if (item && cJSON_IsString(item) && item->valuestring) {
        strncpy(settings->ntp_server, item->valuestring, sizeof(settings->ntp_server) - 1);
        settings->ntp_server[sizeof(settings->ntp_server) - 1] = '\0';
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    if (item && cJSON_IsString(item) && item->valuestring) {
        strncpy(settings->timezone, item->valuestring, sizeof(settings->timezone) - 1);
        settings->timezone[sizeof(settings->timezone) - 1] = '\0';
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "led_mode");
    if (item && cJSON_IsString(item) && item->valuestring) {
        strncpy(settings->led_mode, item->valuestring, sizeof(settings->led_mode) - 1);
        settings->led_mode[sizeof(settings->led_mode) - 1] = '\0';
    }

    cJSON_Delete(root);
    return 0;
}

int config_store_save_settings(const struct hub_settings *settings) {
    if (!settings) return -1;
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddBoolToObject(root, "dry_run", settings->dry_run ? 1 : 0);
    cJSON_AddStringToObject(root, "hub_name", settings->hub_name[0] ? settings->hub_name : "Harmony Hub");
    cJSON_AddStringToObject(root, "ntp_server", settings->ntp_server);
    cJSON_AddStringToObject(root, "timezone", settings->timezone[0] ? settings->timezone : "UTC");
    cJSON_AddStringToObject(root, "led_mode", settings->led_mode[0] ? settings->led_mode : "always_on");

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out) return -1;

    int rc = write_file_atomic(HARMONY_SETTINGS_FILE, out);
    free(out);
    return rc;
}

cJSON *config_store_get_mqtt_json(void) {
    char *raw = read_file_text(HARMONY_MQTT_FILE, 64 * 1024);
    if (!raw) return NULL;
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    return root;
}

int config_store_save_mqtt_json(cJSON *mqtt_json) {
    if (!mqtt_json) return -1;
    char *out_json = cJSON_Print(mqtt_json);
    if (!out_json) return -1;
    int rc = write_file_atomic(HARMONY_MQTT_FILE, out_json);
    free(out_json);
    return rc;
}

