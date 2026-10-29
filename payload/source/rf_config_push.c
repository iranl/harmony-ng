#define _GNU_SOURCE
#include "rf_config_push.h"
#include "rf_spi.h"
#include "rf_keymap.h"
#include "rf_ota.h"
#include "rf_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>

/* Config frames are lost when the CC2544 NAKs a send; retry until the remote ACKs. */
static int rf_send_config_frame(rf_frame_t *frame) {
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (rf_send_frame_acked(frame, 300) == 0) return 0;
    }
    printf("[-] No ACK for config frame type=0x%02X seq=%d after 3 attempts\n", frame->msg_type, frame->seq);
    return -1;
}

void *config_push_worker(void *arg) {
    (void)arg;
    static pthread_mutex_t push_lock = PTHREAD_MUTEX_INITIALIZER;
    if (pthread_mutex_trylock(&push_lock) != 0) {
        printf("[-] Config push already in progress\n");
        return NULL;
    }
    if (is_ota_busy()) {
        printf("[-] Config push skipped: RF OTA in progress on slot %d\n", g_rf_ota.target_slot);
        pthread_mutex_unlock(&push_lock);
        return NULL;
    }
    printf("[*] Starting config push to remote...\n");

    if (g_config_version == 0) {
        FILE *vf = fopen("/data/harmony/config_version", "r");
        if (vf) {
            int v = 1;
            if (fscanf(vf, "%d", &v) == 1 && v > 0) g_config_version = (uint8_t)v;
            fclose(vf);
        } else {
            g_config_version = 1;
        }
    }
    uint8_t config_ver = g_config_version;

    /* 1. CONFIG_END (clear) */
    rf_frame_t f;
    uint8_t target_slots[2] = {0};
    int num_targets = 0;
    uint64_t now_ms = get_monotonic_ms();

    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        uint8_t s = g_paired_remotes[i].slot;
        if (is_companion_slot(s)) continue;
        if (g_paired_remotes[i].active && !g_paired_remotes[i].link_lost) {
            if ((g_paired_remotes[i].last_seen_ms > 0 && (now_ms - g_paired_remotes[i].last_seen_ms < 60000)) ||
                (s == g_remote_slot && g_remote_slot != 0)) {
                target_slots[num_targets++] = s;
            }
        }
    }
    if (num_targets == 0) {
        if (g_remote_slot != 0 && !is_companion_slot(g_remote_slot)) {
            target_slots[num_targets++] = g_remote_slot;
        } else if (!is_companion_slot(0x02) && (g_paired_remotes[1].active || !g_paired_remotes[0].active)) {
            target_slots[num_targets++] = 0x02;
        }
    }

    if (num_targets == 0) {
        printf("[*] Config push skipped: no active Elite remotes\n");
        unlink("/data/harmony/rf_sync_pending.flag");
        unlink("/data/harmony/rf_sync_pending_details.json");
        pthread_mutex_unlock(&push_lock);
        return NULL;
    }

    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        uint8_t payload[26] = {0};
        payload[0] = 0x00; /* clear */
        payload[1] = config_ver;
        rf_build_frame(&f, target_slot, RF_MSG_CONFIG_END, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 2);
        rf_send_config_frame(&f);
    }
    usleep(20000);

    /* 2. Activities */
    struct {
        char id[13];
        char name[24];
        uint8_t icon_type;
    } acts[16];
    int act_count = 0;

    FILE *fa = fopen("/data/harmony/config/activities.json", "r");
    if (fa) {
        fseek(fa, 0, SEEK_END);
        long sz = ftell(fa);
        fseek(fa, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000) {
            char *buf = malloc(sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, sz, fa);
                buf[n] = '\0';
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *acts_arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "Activities");
                    if (!acts_arr) acts_arr = cJSON_GetObjectItem(root, "activities");
                    if (acts_arr && cJSON_IsArray(acts_arr)) {
                        cJSON *elem = NULL;
                        cJSON_ArrayForEach(elem, acts_arr) {
                            if (act_count >= 16) break;
                            cJSON *act_obj = elem;
                            cJSON *inner = cJSON_GetObjectItemCaseSensitive(elem, "Activity");
                            if (inner && cJSON_IsObject(inner)) act_obj = inner;

                            cJSON *j_name = cJSON_GetObjectItem(act_obj, "Name");
                            if (!j_name) j_name = cJSON_GetObjectItem(act_obj, "name");
                            cJSON *j_id = cJSON_GetObjectItem(act_obj, "Id-");
                            if (!j_id) j_id = cJSON_GetObjectItem(act_obj, "Id");
                            if (!j_id) j_id = cJSON_GetObjectItem(act_obj, "id");
                            cJSON *j_icon = cJSON_GetObjectItem(act_obj, "icon");
                            if (!j_icon) j_icon = cJSON_GetObjectItem(act_obj, "Icon");
                            const char *icon_str = (j_icon && cJSON_IsString(j_icon)) ? j_icon->valuestring : "";
                            cJSON *j_type = cJSON_GetObjectItem(act_obj, "type");
                            const char *type_str = (j_type && cJSON_IsString(j_type)) ? j_type->valuestring : "";

                            if (j_name && cJSON_IsString(j_name) && j_id) {
                                char id_str[32] = {0};
                                if (cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%d", j_id->valueint);
                                else if (cJSON_IsString(j_id)) strncpy(id_str, j_id->valuestring, sizeof(id_str) - 1);
                                if (strcmp(id_str, "-1") != 0) {
                                    strncpy(acts[act_count].id, id_str, sizeof(acts[0].id) - 1);
                                    acts[act_count].id[sizeof(acts[0].id) - 1] = '\0';
                                    strncpy(acts[act_count].name, j_name->valuestring, sizeof(acts[0].name) - 1);
                                    acts[act_count].name[sizeof(acts[0].name) - 1] = '\0';
                                    acts[act_count].icon_type = map_activity_icon_type(icon_str, j_name->valuestring, type_str);
                                    act_count++;
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(fa);
    }

    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        for (int i = 0; i < act_count; i++) {
            uint8_t payload[26] = {0};
            payload[0] = (uint8_t)i;
            payload[1] = (uint8_t)act_count;
            payload[2] = acts[i].icon_type;
            if (strlen(acts[i].id) <= 2 || strcmp(acts[i].id, "-1") == 0) {
                strncpy((char *)&payload[3], acts[i].id, 3);
                strncpy((char *)&payload[6], acts[i].name, 19);
            } else {
                strncpy((char *)&payload[3], acts[i].id, 8);
                strncpy((char *)&payload[11], acts[i].name, 14);
            }
            rf_build_frame(&f, target_slot, RF_MSG_ACTIVITY_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 26);
            rf_send_config_frame(&f);
            usleep(15000);
        }
    }

    /* 3. Devices */
    struct {
        char id[8];
        char name[24];
    } devs[16];
    cJSON *dev_cmds[16] = {0}; /* each device's "commands" array, owned by dev_root */
    cJSON *dev_root = NULL;
    int dev_count = 0;

    FILE *fd = fopen("/data/harmony/config/devices.json", "r");
    if (fd) {
        fseek(fd, 0, SEEK_END);
        long sz = ftell(fd);
        fseek(fd, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000) {
            char *buf = malloc(sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, sz, fd);
                buf[n] = '\0';
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *devs_arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "DevicesWithFeatures");
                    if (!devs_arr) devs_arr = cJSON_GetObjectItem(root, "devices");
                    if (devs_arr && cJSON_IsArray(devs_arr)) {
                        cJSON *elem = NULL;
                        cJSON_ArrayForEach(elem, devs_arr) {
                            if (dev_count >= 16) break;
                            cJSON *d_obj = elem;
                            cJSON *inner = cJSON_GetObjectItemCaseSensitive(elem, "Device");
                            if (inner && cJSON_IsObject(inner)) d_obj = inner;

                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(d_obj, "Id-");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d_obj, "Id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d_obj, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d_obj, "ContentProfileKey");

                            cJSON *jname = cJSON_GetObjectItem(d_obj, "name");
                            if (!jname) jname = cJSON_GetObjectItem(d_obj, "Name");
                            if (!jname) jname = cJSON_GetObjectItem(d_obj, "FriendlyName");
                            if (!jname) jname = cJSON_GetObjectItem(d_obj, "label");
                            if (!jname) jname = cJSON_GetObjectItem(d_obj, "Model");
                            if (!jname) jname = cJSON_GetObjectItem(d_obj, "ParentDeviceModel");

                            if (jid && jname && cJSON_IsString(jname)) {
                                char id_str[32] = {0};
                                if (cJSON_IsNumber(jid)) snprintf(id_str, sizeof(id_str), "%d", jid->valueint);
                                else if (cJSON_IsString(jid)) strncpy(id_str, jid->valuestring, sizeof(id_str) - 1);

                                if (id_str[0]) {
                                    strncpy(devs[dev_count].id, id_str, 7);
                                    devs[dev_count].id[7] = '\0';
                                    strncpy(devs[dev_count].name, jname->valuestring, 21);
                                    devs[dev_count].name[21] = '\0';
                                    dev_cmds[dev_count] = cJSON_GetObjectItem(d_obj, "commands");
                                    dev_count++;
                                }
                            }
                        }
                    }
                    dev_root = root;
                }
            }
        }
        fclose(fd);
    }

    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        for (int i = 0; i < dev_count; i++) {
            uint8_t payload[26] = {0};
            payload[0] = (uint8_t)i;
            payload[1] = (uint8_t)dev_count;
            memcpy(&payload[2], devs[i].id, 4);
            memcpy(&payload[6], devs[i].name, 20);
            rf_build_frame(&f, target_slot, RF_MSG_DEVICE_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 26);
            rf_send_config_frame(&f);
            usleep(15000);
        }
    }

    /* 3b. Device command names, for browsing devices on the remote. A remote without
       device browsing drops these frames, so stop if the first one is never ACKed. */
    int dev_cmd_count = 0;
    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        dev_cmd_count = 0;
        bool supported = true;
        for (int i = 0; i < dev_count && supported; i++) {
            if (!cJSON_IsArray(dev_cmds[i])) continue;
            int total = cJSON_GetArraySize(dev_cmds[i]);
            if (total > 255) total = 255;
            int c = 0;
            cJSON *cmd = NULL;
            cJSON_ArrayForEach(cmd, dev_cmds[i]) {
                if (c >= total) break;
                cJSON *jn = cJSON_IsString(cmd) ? cmd : cJSON_GetObjectItem(cmd, "name");
                if (jn && cJSON_IsString(jn) && jn->valuestring[0]) {
                    uint8_t payload[25] = {0};
                    payload[0] = (uint8_t)i;
                    payload[1] = (uint8_t)c;
                    payload[2] = (uint8_t)total;
                    strncpy((char *)&payload[3], jn->valuestring, 22);
                    rf_build_frame(&f, target_slot, RF_MSG_DEVICE_CMD_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 25);
                    if (rf_send_config_frame(&f) == 0) {
                        dev_cmd_count++;
                    } else if (dev_cmd_count == 0) {
                        printf("[-] Slot %d did not ACK device commands; remote has no device browsing\n", target_slot);
                        supported = false;
                        break;
                    }
                    usleep(15000);
                }
                c++;
            }
        }
    }
    cJSON_Delete(dev_root);

    /* 4. Buttons: Direct Mode vs Activity Mode per target slot */
    int btn_count = 0;
    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        btn_count = 0;
        if (is_slot_direct_mode(target_slot)) {
            /* Direct Device Mode: Push Page 1 and Page 2 screen buttons + direct buttons */
            cJSON *dmap = get_elite_direct_map(target_slot);
            if (dmap) {
                const char *pages[2] = { "page1", "page2" };
                for (int p = 0; p < 2; p++) {
                    cJSON *sbtns = cJSON_GetObjectItem(dmap, pages[p]);
                    if (sbtns && cJSON_IsArray(sbtns)) {
                        int sidx = 0;
                        cJSON *sitem = NULL;
                        cJSON_ArrayForEach(sitem, sbtns) {
                            if (btn_count >= 48) break;
                            if (!cJSON_IsObject(sitem)) continue;
                            cJSON *jlbl = cJSON_GetObjectItem(sitem, "label");
                            const char *label = (jlbl && jlbl->valuestring) ? jlbl->valuestring : "Button";
                            uint8_t payload[26] = {0};
                            payload[0] = (uint8_t)btn_count;
                            payload[1] = (uint8_t)sidx;
                            payload[2] = 0x01; /* screen button */
                            strncpy((char *)&payload[3], pages[p], 9);
                            strncpy((char *)&payload[12], label, 12);
                            payload[24] = 0x01; /* device_cmd */

                            rf_build_frame(&f, target_slot, RF_MSG_BUTTON_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 25);
                            rf_send_config_frame(&f);
                            btn_count++;
                            sidx++;
                            usleep(15000);
                        }
                    }
                }
                cJSON_Delete(dmap);
            }
        } else {
            /* Standard Activity Mode */
            cJSON *map = get_elite_map();
            bool has_pwroff = false;
            if (map) {
                pthread_mutex_lock(&g_map_lock);
                cJSON *acts_map = cJSON_GetObjectItemCaseSensitive(map, "activities");
                if (acts_map && cJSON_IsObject(acts_map)) {
                    cJSON *act_entry = NULL;
                    cJSON_ArrayForEach(act_entry, acts_map) {
                        const char *ctx_id = act_entry->string ? act_entry->string : "-1";
                        if (!cJSON_IsObject(act_entry)) continue;

                        cJSON *sbtns = cJSON_GetObjectItemCaseSensitive(act_entry, "screen_buttons");
                        if (sbtns && cJSON_IsArray(sbtns) && cJSON_GetArraySize(sbtns) > 0) {
                            int sidx = 0;
                            cJSON *sitem = NULL;
                            cJSON_ArrayForEach(sitem, sbtns) {
                                if (btn_count >= 48) break;
                                if (!cJSON_IsObject(sitem)) continue;
                                cJSON *jlbl = cJSON_GetObjectItemCaseSensitive(sitem, "label");
                                const char *label = (jlbl && jlbl->valuestring) ? jlbl->valuestring : "Button";

                                uint8_t payload[26] = {0};
                                payload[0] = (uint8_t)btn_count;
                                payload[1] = (uint8_t)sidx;
                                payload[2] = 0x01; /* screen button */
                                strncpy((char *)&payload[3], ctx_id, 9);
                                strncpy((char *)&payload[12], label, 12);
                                payload[24] = 0x01; /* device_cmd */

                                rf_build_frame(&f, target_slot, RF_MSG_BUTTON_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 25);
                                rf_send_config_frame(&f);
                                btn_count++;
                                sidx++;
                                usleep(15000);
                            }
                        } else {
                            cJSON *b_entry = NULL;
                            int b_idx = 0;
                            cJSON_ArrayForEach(b_entry, act_entry) {
                                if (btn_count >= 48) break;
                                const char *btn_name = b_entry->string;
                                if (!btn_name || !cJSON_IsObject(b_entry)) continue;
                                if (strcmp(btn_name, "screen_buttons") == 0) continue;
                                if (strcasecmp(btn_name, "PwrOff") == 0 || strcasecmp(btn_name, "PowerOffActivity") == 0) {
                                    has_pwroff = true;
                                }

                                cJSON *j_act = cJSON_GetObjectItemCaseSensitive(b_entry, "action");
                                uint8_t atype = 0x01;
                                char target_id[16] = {0};
                                strncpy(target_id, ctx_id, sizeof(target_id) - 1);

                                if (j_act && cJSON_IsString(j_act)) {
                                    if (strcmp(j_act->valuestring, "device_cmd") == 0) {
                                        atype = 0x01;
                                    } else if (strcmp(j_act->valuestring, "activity_start") == 0) {
                                        atype = 0x02;
                                        cJSON *j_actid = cJSON_GetObjectItemCaseSensitive(b_entry, "activityId");
                                        if (j_actid && cJSON_IsString(j_actid)) {
                                            strncpy(target_id, j_actid->valuestring, sizeof(target_id) - 1);
                                        }
                                    } else if (strcmp(j_act->valuestring, "activity_stop") == 0) {
                                        atype = 0x03;
                                        strncpy(target_id, "-1", sizeof(target_id) - 1);
                                    }
                                }

                                uint8_t payload[26] = {0};
                                payload[0] = (uint8_t)btn_count;
                                payload[1] = (uint8_t)b_idx;
                                payload[2] = 0x00; /* physical button */
                                strncpy((char *)&payload[3], target_id, 9);
                                strncpy((char *)&payload[12], btn_name, 12);
                                payload[24] = atype;

                                rf_build_frame(&f, target_slot, RF_MSG_BUTTON_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 25);
                                rf_send_config_frame(&f);
                                btn_count++;
                                b_idx++;
                                usleep(15000);
                            }
                        }
                    }
                }
                pthread_mutex_unlock(&g_map_lock);
            }
            if (!has_pwroff && btn_count < 48) {
                uint8_t payload[26] = {0};
                payload[0] = (uint8_t)btn_count;
                payload[1] = 0;
                payload[2] = 0x00; /* physical button */
                strncpy((char *)&payload[3], "-1", 9);
                strncpy((char *)&payload[12], "PwrOff", 12);
                payload[24] = 0x03; /* activity_stop */

                rf_build_frame(&f, target_slot, RF_MSG_BUTTON_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 25);
                rf_send_config_frame(&f);
                btn_count++;
                usleep(15000);
            }
        }
    }

    /* 5. Activity Transition Steps & Delays (RF_MSG_STEP_LIST_CHUNK) */
    struct rf_step_send {
        char act_id[12];
        uint8_t step_idx;
        uint8_t total_steps;
        uint16_t delay_ms;
        char name[16];
    };
    struct rf_step_send act_steps[64];
    int total_step_count = 0;

    FILE *fsteps = fopen("/data/harmony/config/activities.json", "r");
    if (fsteps) {
        fseek(fsteps, 0, SEEK_END);
        long sz = ftell(fsteps);
        fseek(fsteps, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000) {
            char *buf = malloc(sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, sz, fsteps);
                buf[n] = '\0';
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *acts_arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "Activities");
                    if (!acts_arr) acts_arr = cJSON_GetObjectItem(root, "activities");
                    if (acts_arr && cJSON_IsArray(acts_arr)) {
                        cJSON *elem = NULL;
                        cJSON_ArrayForEach(elem, acts_arr) {
                            if (total_step_count >= 60) break;
                            cJSON *act_obj = elem;
                            cJSON *inner = cJSON_GetObjectItemCaseSensitive(elem, "Activity");
                            if (inner && cJSON_IsObject(inner)) act_obj = inner;

                            cJSON *j_id = cJSON_GetObjectItem(act_obj, "Id-");
                            if (!j_id) j_id = cJSON_GetObjectItem(act_obj, "Id");
                            if (!j_id) j_id = cJSON_GetObjectItem(act_obj, "id");
                            char act_id[32] = {0};
                            if (j_id) {
                                if (cJSON_IsNumber(j_id)) snprintf(act_id, sizeof(act_id), "%d", j_id->valueint);
                                else if (cJSON_IsString(j_id)) strncpy(act_id, j_id->valuestring, sizeof(act_id) - 1);
                            }
                            if (!act_id[0]) continue;

                            struct rf_step_send local_steps[8];
                            int local_count = 0;

                            /* startSequence -> Modern Harmony format */
                            cJSON *start_seq = cJSON_GetObjectItem(act_obj, "startSequence");
                            if (start_seq && cJSON_IsArray(start_seq)) {
                                cJSON *sitem = NULL;
                                cJSON_ArrayForEach(sitem, start_seq) {
                                    if (local_count >= 8) break;
                                    cJSON *cname = cJSON_GetObjectItem(sitem, "command");
                                    cJSON *ctype = cJSON_GetObjectItem(sitem, "type");
                                    cJSON *cdelay = cJSON_GetObjectItem(sitem, "delay");
                                    int d = (cdelay && cJSON_IsNumber(cdelay)) ? cdelay->valueint : 0;
                                    if (cname && cJSON_IsString(cname) && cname->valuestring[0]) {
                                        strncpy(local_steps[local_count].act_id, act_id, 11);
                                        local_steps[local_count].act_id[11] = '\0';
                                        strncpy(local_steps[local_count].name, cname->valuestring, 15);
                                        local_steps[local_count].name[15] = '\0';
                                        if (d <= 0) d = 300;
                                        if (d > 20000) d = 20000;
                                        local_steps[local_count].delay_ms = (uint16_t)d;
                                        local_count++;
                                    } else if (ctype && cJSON_IsString(ctype) && strcmp(ctype->valuestring, "Delay") == 0) {
                                        if (local_count > 0 && d > 0) {
                                            local_steps[local_count - 1].delay_ms += (uint16_t)d;
                                        }
                                    }
                                }
                            }

                            /* Roles -> Input selection steps */
                            cJSON *roles = cJSON_GetObjectItem(act_obj, "Roles");
                            if (roles && cJSON_IsArray(roles)) {
                                cJSON *r_item = NULL;
                                cJSON_ArrayForEach(r_item, roles) {
                                    if (local_count >= 3) break;
                                    cJSON *sel_in = cJSON_GetObjectItem(r_item, "SelectedInput");
                                    if (sel_in && cJSON_IsObject(sel_in)) {
                                        cJSON *in_name = cJSON_GetObjectItem(sel_in, "Name");
                                        if (in_name && cJSON_IsString(in_name) && in_name->valuestring[0]) {
                                            strncpy(local_steps[local_count].act_id, act_id, 11);
                                            local_steps[local_count].act_id[11] = '\0';
                                            snprintf(local_steps[local_count].name, sizeof(local_steps[local_count].name), "%s", in_name->valuestring);
                                            local_steps[local_count].delay_ms = 1500;
                                            local_count++;
                                        }
                                    }
                                }
                            }

                            /* EnterActions -> Power / Initial Command steps */
                            cJSON *enter_acts = cJSON_GetObjectItem(act_obj, "EnterActions");
                            if (enter_acts && cJSON_IsArray(enter_acts)) {
                                cJSON *ea = NULL;
                                cJSON_ArrayForEach(ea, enter_acts) {
                                    if (local_count >= 6) break;
                                    cJSON *cname = cJSON_GetObjectItem(ea, "CommandName");
                                    if (!cname) cname = cJSON_GetObjectItem(ea, "Command");
                                    cJSON *cdelay = cJSON_GetObjectItem(ea, "Delay");
                                    if (cname && cJSON_IsString(cname)) {
                                        strncpy(local_steps[local_count].act_id, act_id, 11);
                                        local_steps[local_count].act_id[11] = '\0';
                                        strncpy(local_steps[local_count].name, cname->valuestring, 15);
                                        local_steps[local_count].name[15] = '\0';
                                        int d = (cdelay && cJSON_IsNumber(cdelay)) ? cdelay->valueint : 1000;
                                        if (d <= 0) d = 300;
                                        if (d > 20000) d = 20000;
                                        local_steps[local_count].delay_ms = (uint16_t)d;
                                        local_count++;
                                    }
                                }
                            }

                            /* Sensible default transition steps if not explicitly in JSON */
                            if (local_count == 0) {
                                strncpy(local_steps[0].act_id, act_id, 11);
                                if (strcmp(act_id, "-1") == 0) {
                                    strncpy(local_steps[0].name, "Power Off", sizeof(local_steps[0].name) - 1);
                                    local_steps[0].delay_ms = 1500;
                                    local_count = 1;
                                } else {
                                    strncpy(local_steps[0].name, "Power On", sizeof(local_steps[0].name) - 1);
                                    local_steps[0].delay_ms = 2500;
                                    strncpy(local_steps[1].act_id, act_id, 11);
                                    strncpy(local_steps[1].name, "Set Inputs", sizeof(local_steps[1].name) - 1);
                                    local_steps[1].delay_ms = 1500;
                                    local_count = 2;
                                }
                            }

                            for (int s = 0; s < local_count && total_step_count < 64; s++) {
                                act_steps[total_step_count] = local_steps[s];
                                act_steps[total_step_count].step_idx = (uint8_t)s;
                                act_steps[total_step_count].total_steps = (uint8_t)local_count;
                                total_step_count++;
                            }

                            /* Append stop/leave sequence step(s) for this activity */
                            if (total_step_count < 63) {
                                cJSON *stop_seq = cJSON_GetObjectItem(act_obj, "stopSequence");
                                if (!stop_seq) stop_seq = cJSON_GetObjectItem(act_obj, "StopSequence");
                                if (!stop_seq) stop_seq = cJSON_GetObjectItem(act_obj, "LeaveActions");
                                int stop_cnt = 0;
                                if (stop_seq && cJSON_IsArray(stop_seq)) {
                                    cJSON *st_item = NULL;
                                    cJSON_ArrayForEach(st_item, stop_seq) {
                                        if (total_step_count >= 63 || stop_cnt >= 8) break;
                                        cJSON *cname = cJSON_GetObjectItem(st_item, "command");
                                        if (!cname) cname = cJSON_GetObjectItem(st_item, "Command");
                                        cJSON *ctype = cJSON_GetObjectItem(st_item, "type");
                                        if (!ctype) ctype = cJSON_GetObjectItem(st_item, "Type");
                                        cJSON *cdelay = cJSON_GetObjectItem(st_item, "delay");
                                        if (!cdelay) cdelay = cJSON_GetObjectItem(st_item, "Delay");
                                        if (!cdelay) cdelay = cJSON_GetObjectItem(st_item, "Duration");
                                        int d = (cdelay && cJSON_IsNumber(cdelay)) ? cdelay->valueint : 0;

                                        if (cname && cJSON_IsString(cname) && cname->valuestring[0]) {
                                            if (d <= 0) d = 250;
                                            if (d > 20000) d = 20000;
                                            strncpy(act_steps[total_step_count].act_id, act_id, 11);
                                            act_steps[total_step_count].act_id[11] = '\0';
                                            strncpy(act_steps[total_step_count].name, cname->valuestring, 15);
                                            act_steps[total_step_count].name[15] = '\0';
                                            act_steps[total_step_count].delay_ms = (uint16_t)d;
                                            act_steps[total_step_count].step_idx = 0x80 | (uint8_t)stop_cnt;
                                            act_steps[total_step_count].total_steps = 1;
                                            total_step_count++;
                                            stop_cnt++;
                                        } else if (ctype && cJSON_IsString(ctype) && strcasecmp(ctype->valuestring, "Delay") == 0) {
                                            if (total_step_count > 0 && stop_cnt > 0 && d > 0) {
                                                act_steps[total_step_count - 1].delay_ms += (uint16_t)d;
                                            }
                                        }
                                    }
                                }
                                if (stop_cnt == 0 && total_step_count < 63 && strcmp(act_id, "-1") != 0) {
                                    strncpy(act_steps[total_step_count].act_id, act_id, 11);
                                    act_steps[total_step_count].act_id[11] = '\0';
                                    strncpy(act_steps[total_step_count].name, "Stop", 15);
                                    act_steps[total_step_count].name[15] = '\0';
                                    act_steps[total_step_count].delay_ms = 1500;
                                    act_steps[total_step_count].step_idx = 0x80;
                                    act_steps[total_step_count].total_steps = 1;
                                    total_step_count++;
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(fsteps);
    }

    /* Append PowerOff sequence step for "-1" */
    if (total_step_count < 64) {
        strncpy(act_steps[total_step_count].act_id, "-1", 11);
        act_steps[total_step_count].act_id[11] = '\0';
        strncpy(act_steps[total_step_count].name, "Power Off", 15);
        act_steps[total_step_count].name[15] = '\0';
        act_steps[total_step_count].delay_ms = 1500;
        act_steps[total_step_count].step_idx = 0;
        act_steps[total_step_count].total_steps = 1;
        total_step_count++;
    }

    /* Send Step Chunks to target slots */
    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        if (is_slot_direct_mode(target_slot)) continue;
        for (int i = 0; i < total_step_count; i++) {
            uint8_t payload[26] = {0};
            payload[0] = (uint8_t)i;
            payload[1] = act_steps[i].step_idx;
            payload[2] = act_steps[i].total_steps;
            strncpy((char *)&payload[3], act_steps[i].act_id, 9);
            payload[12] = (uint8_t)(act_steps[i].delay_ms & 0xFF);
            payload[13] = (uint8_t)((act_steps[i].delay_ms >> 8) & 0xFF);
            strncpy((char *)&payload[14], act_steps[i].name, 11);

            rf_build_frame(&f, target_slot, RF_MSG_STEP_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 26);
            rf_send_config_frame(&f);
            usleep(15000);
        }
    }

    /* 6. Settings push */
    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        int bright = 2, timeout = 10, touch = 1, gyro = 1, nav_key = 139, nav_mode = 1, confirm_act = 1, touch_sens = 2, remote_mode = 0;
        int gyro_rate = 25, btn_backlight = 2, night_start = 22, night_end = 7, usbeth = 0, power_debug = 0;
        char spath[64];
        snprintf(spath, sizeof(spath), "/data/harmony/elite_slot%d_config.json", target_slot);
        FILE *sf = fopen(spath, "r");
        if (sf) {
            char sbuf[4096] = {0};
            size_t sn = fread(sbuf, 1, sizeof(sbuf) - 1, sf);
            sbuf[sn] = '\0';
            fclose(sf);
            cJSON *sroot = cJSON_Parse(sbuf);
            if (sroot) {
                cJSON *jb = cJSON_GetObjectItem(sroot, "brightness");
                cJSON *jt = cJSON_GetObjectItem(sroot, "timeout");
                cJSON *jtouch = cJSON_GetObjectItem(sroot, "touchEnabled");
                if (!jtouch) jtouch = cJSON_GetObjectItem(sroot, "touch");
                cJSON *jgyro = cJSON_GetObjectItem(sroot, "gyroEnabled");
                if (!jgyro) jgyro = cJSON_GetObjectItem(sroot, "gyro");
                cJSON *jnk = cJSON_GetObjectItem(sroot, "navToggleKey");
                if (!jnk) jnk = cJSON_GetObjectItem(sroot, "nav_key");
                cJSON *jnm = cJSON_GetObjectItem(sroot, "navToggleMode");
                if (!jnm) jnm = cJSON_GetObjectItem(sroot, "nav_mode");
                cJSON *jca = cJSON_GetObjectItem(sroot, "confirmActivity");
                if (!jca) jca = cJSON_GetObjectItem(sroot, "confirm_act");
                cJSON *jts = cJSON_GetObjectItem(sroot, "touchSensitivity");
                if (!jts) jts = cJSON_GetObjectItem(sroot, "touch_sensitivity");
                cJSON *jrm = cJSON_GetObjectItem(sroot, "remoteMode");
                if (!jrm) jrm = cJSON_GetObjectItem(sroot, "remote_mode");
                cJSON *jgr = cJSON_GetObjectItem(sroot, "gyroRate");
                if (!jgr) jgr = cJSON_GetObjectItem(sroot, "gyro_rate");
                cJSON *jbb = cJSON_GetObjectItem(sroot, "btnBacklight");
                if (!jbb) jbb = cJSON_GetObjectItem(sroot, "btn_backlight");
                cJSON *jns = cJSON_GetObjectItem(sroot, "nightStart");
                if (!jns) jns = cJSON_GetObjectItem(sroot, "night_start");
                cJSON *jne = cJSON_GetObjectItem(sroot, "nightEnd");
                if (!jne) jne = cJSON_GetObjectItem(sroot, "night_end");
                cJSON *jue = cJSON_GetObjectItem(sroot, "usbEth");
                if (!jue) jue = cJSON_GetObjectItem(sroot, "usbeth");
                cJSON *jpd = cJSON_GetObjectItem(sroot, "powerDebug");
                if (!jpd) jpd = cJSON_GetObjectItem(sroot, "power_debug");
                cJSON *jnscreen = cJSON_GetObjectItem(sroot, "noScreen");
                if (!jnscreen) jnscreen = cJSON_GetObjectItem(sroot, "no_screen");
                int no_screen = 0;
                if (jnscreen) no_screen = cJSON_IsTrue(jnscreen) || (cJSON_IsNumber(jnscreen) && jnscreen->valueint != 0) ? 1 : 0;
                cJSON *jcap = cJSON_GetObjectItem(sroot, "capButtonsEnabled");
                if (!jcap) jcap = cJSON_GetObjectItem(sroot, "cap_buttons");
                int cap_buttons = 1;
                if (jcap) cap_buttons = (cJSON_IsFalse(jcap) || (cJSON_IsNumber(jcap) && jcap->valueint == 0)) ? 0 : 1;

                if (jb) bright = jb->valueint;
                if (jt) timeout = jt->valueint;
                if (jtouch) touch = cJSON_IsTrue(jtouch) || (cJSON_IsNumber(jtouch) && jtouch->valueint != 0) ? 1 : 0;
                if (jgyro) gyro = cJSON_IsTrue(jgyro) || (cJSON_IsNumber(jgyro) && jgyro->valueint != 0) ? 1 : 0;
                if (jnk) nav_key = jnk->valueint;
                if (jnm) nav_mode = jnm->valueint;
                if (jca) confirm_act = cJSON_IsTrue(jca) || (cJSON_IsNumber(jca) && jca->valueint != 0) ? 1 : 0;
                if (jts && jts->valueint >= 1 && jts->valueint <= 3) touch_sens = jts->valueint;
                if (jgr && jgr->valueint > 0) gyro_rate = jgr->valueint;
                if (jbb && jbb->valueint >= 0 && jbb->valueint <= 2) btn_backlight = jbb->valueint;
                if (jns && jns->valueint >= 0 && jns->valueint <= 23) night_start = jns->valueint;
                if (jne && jne->valueint >= 0 && jne->valueint <= 23) night_end = jne->valueint;
                if (jue) usbeth = cJSON_IsTrue(jue) || (cJSON_IsNumber(jue) && jue->valueint != 0) ? 1 : 0;
                if (jpd) power_debug = cJSON_IsTrue(jpd) || (cJSON_IsNumber(jpd) && jpd->valueint != 0) ? 1 : 0;
                cJSON_Delete(sroot);
                rf_send_settings_push_slot(target_slot, bright, timeout, touch, gyro, nav_key, nav_mode, confirm_act, touch_sens, remote_mode, gyro_rate, btn_backlight, night_start, night_end, usbeth, no_screen, cap_buttons, power_debug);
                usleep(15000);
            }
        }

        /* 7. CONFIG_END (complete) */
        uint8_t payload[26] = {0};
        payload[0] = 0x01; /* complete */
        payload[1] = g_config_version;
        rf_build_frame(&f, target_slot, RF_MSG_CONFIG_END, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 2);
        if (rf_send_config_frame(&f) == 0 && target_slot >= 1 && target_slot <= 2) {
            /* The remote applies the config when it receives this frame, but it only sends
               VERSION_REPORT on first link and every 24h, so record its new version here. */
            int idx = target_slot - 1;
            g_remote_versions[idx].config_version = payload[1];
            char vpath[64];
            snprintf(vpath, sizeof(vpath), "/data/harmony/elite_slot%d_version.json", target_slot);
            FILE *vf = fopen(vpath, "w");
            if (vf) {
                fprintf(vf, "{\"slot\":%d,\"version\":\"%s\",\"config_version\":%d,\"timestamp\":%ld}\n",
                        target_slot, g_remote_versions[idx].sw_version, payload[1], (long)time(NULL));
                fclose(vf);
            }
        }
    }

    printf("[+] Config push complete (acts=%d devs=%d dev_cmds=%d btns=%d steps=%d ver=%d)\n",
           act_count, dev_count, dev_cmd_count, btn_count, total_step_count, g_config_version);
    unlink("/data/harmony/rf_sync_pending.flag");
    unlink("/data/harmony/rf_sync_pending_details.json");

    pthread_mutex_unlock(&push_lock);
    return NULL;
}
