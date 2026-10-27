#include "orchestrator.h"
#include "orchestrator_queue.h"
#include "hw_action.h"
#include "cJSON.h"
#include "config_store.h"
#include "resource_cache.h"
#include "bt_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#define PRIMARY_CURRENT_ACT_FILE "/data/harmony/current_activity"

static orch_state_t g_state = ORCH_STATE_IDLE;
static char g_current_act[32] = ORCH_POWEROFF_ID;
static char g_target_act[32] = ORCH_POWEROFF_ID;
static pthread_mutex_t g_act_lock = PTHREAD_MUTEX_INITIALIZER;

static orch_step_t g_queue[ORCH_MAX_STEPS];
static size_t g_queue_len = 0;
static size_t g_current_step = 0;
static uint64_t g_next_deadline_ms = 0;

static orch_bg_step_t g_bg_queue[ORCH_MAX_STEPS];
static size_t g_bg_len = 0;
static uint64_t g_t_switched_step1 = 0;
static uint64_t g_t_switched_step4 = 0;

static orch_progress_cb g_progress_cb = NULL;
static void *g_cb_user_data = NULL;

static uint64_t get_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void save_current_activity(const char *act_id) {
    if (!act_id || !act_id[0]) return;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", PRIMARY_CURRENT_ACT_FILE);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%s\n", act_id);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp, PRIMARY_CURRENT_ACT_FILE);
    sync();
}

static void load_current_activity(void) {
    const char *path = PRIMARY_CURRENT_ACT_FILE;
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[32] = {0};
        if (fscanf(f, "%31s", buf) == 1 && buf[0]) {
            strncpy(g_current_act, buf, sizeof(g_current_act) - 1);
            g_current_act[sizeof(g_current_act) - 1] = '\0';
        }
        fclose(f);
    }
}

int orch_init(void) {
    pthread_mutex_lock(&g_act_lock);
    load_current_activity();
    g_state = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
    pthread_mutex_unlock(&g_act_lock);
    g_queue_len = 0;
    g_current_step = 0;
    g_next_deadline_ms = 0;
    g_bg_len = 0;
    memset(g_bg_queue, 0, sizeof(g_bg_queue));
    return 0;
}

void orch_set_progress_callback(orch_progress_cb cb, void *user_data) {
    g_progress_cb = cb;
    g_cb_user_data = user_data;
}

int orch_is_busy(void) {
    return (g_state == ORCH_STATE_STARTING || g_state == ORCH_STATE_STOPPING);
}

const char *orch_get_current_activity(void) {
    pthread_mutex_lock(&g_act_lock);
    const char *ret = g_current_act;
    pthread_mutex_unlock(&g_act_lock);
    return ret;
}

orch_state_t orch_get_state(void) {
    pthread_mutex_lock(&g_act_lock);
    if (orch_is_busy()) {
        orch_state_t st = (strcmp(g_target_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_STOPPING : ORCH_STATE_STARTING;
        pthread_mutex_unlock(&g_act_lock);
        return st;
    }
    orch_state_t st = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
    pthread_mutex_unlock(&g_act_lock);
    return st;
}

void orch_cancel(void) {
    hw_ir_cancel(IR_PORT_ALL);
    pthread_mutex_lock(&g_act_lock);
    if (g_state == ORCH_STATE_STOPPING || strcmp(g_target_act, ORCH_POWEROFF_ID) == 0) {
        strncpy(g_current_act, ORCH_POWEROFF_ID, sizeof(g_current_act) - 1);
        save_current_activity(g_current_act);
        g_state = ORCH_STATE_IDLE;
        pthread_mutex_unlock(&g_act_lock);
        hub_send_btstack_cmd("disconnect_host\n");
        system("killall -9 bt_reconnect.sh 2>/dev/null");
    } else {
        g_state = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
        pthread_mutex_unlock(&g_act_lock);
    }
    g_queue_len = 0;
    g_current_step = 0;
    g_bg_len = 0;
    memset(g_bg_queue, 0, sizeof(g_bg_queue));
    if (g_progress_cb) {
        g_progress_cb(g_current_act, g_state, 0, 0, "Sequence cancelled", g_cb_user_data);
    }
}

int orch_enqueue_steps(const orch_step_t *steps, size_t count) {
    if (count > ORCH_MAX_STEPS) return -1;
    memcpy(g_queue, steps, count * sizeof(orch_step_t));
    g_queue_len = count;
    g_current_step = 0;
    g_next_deadline_ms = get_now_ms();
    return 0;
}

static void orch_enqueue_bg_steps(const orch_bg_step_t *bg_steps, size_t count) {
    if (count > ORCH_MAX_STEPS) count = ORCH_MAX_STEPS;
    if (bg_steps && count > 0) {
        memcpy(g_bg_queue, bg_steps, count * sizeof(orch_bg_step_t));
        g_bg_len = count;
    } else {
        g_bg_len = 0;
    }
    g_t_switched_step1 = 0;
    g_t_switched_step4 = 0;
}

static void execute_step(const orch_step_t *step) {
    switch (step->type) {
    case STEP_ACT_IR_PRONTO:
        if (step->pronto_hex[0]) {
            hw_ir_send_pronto(step->pronto_hex, step->ir_ports, step->ir_repeats);
        }
        break;
    case STEP_ACT_DEVICE_CMD:
        if (step->device_id[0] && step->command_name[0]) {
            int rc = hw_device_command_send(step->device_id, step->command_name);
            if (rc != 0 && (strncmp(step->command_name, "0000 ", 5) == 0 ||
                            step->command_name[0] == 'F' || step->command_name[0] == 'f')) {
                hw_ir_send_harmony_keycode(step->command_name, IR_PORT_ALL, 3);
            }
        }
        break;
    case STEP_ACT_BTHID:
        if (step->command_name[0]) {
            if (strcasecmp(step->command_name, "CONNECT") == 0 ||
                strcasecmp(step->command_name, "RECONNECT") == 0 ||
                strcasecmp(step->command_name, "bt_connect") == 0) {
                if (step->device_id[0] && strncmp(step->device_id, "bt_", 3) != 0) {
                    char cmd[128];
                    snprintf(cmd, sizeof(cmd), "CONNECT %s", step->device_id);
                    hub_send_btstack_cmd(cmd);
                } else {
                    hub_send_btstack_cmd("CONNECT");
                }
            } else if (strcasecmp(step->command_name, "DISCONNECT") == 0) {
                hub_send_btstack_cmd("DISCONNECT");
            } else {
                char bt_reply[256];
                run_bt_saved_script("btkeyboard", "", step->command_name, 50, bt_reply, sizeof(bt_reply));
            }
        }
        break;
    case STEP_ACT_DELAY:
    default:
        break;
    }
}

int orch_tick(void) {
    uint64_t now = get_now_ms();

    /* Process background queue */
    if (g_bg_len > 0) {
        int any_pending = 0;
        for (size_t b = 0; b < g_bg_len; b++) {
            if (!g_bg_queue[b].executed) {
                if (g_bg_queue[b].target_time_ms > 0 && now >= g_bg_queue[b].target_time_ms) {
                    execute_step(&g_bg_queue[b].step);
                    g_bg_queue[b].executed = 1;
                } else {
                    any_pending = 1;
                }
            }
        }
        if (!any_pending) {
            g_bg_len = 0;
        }
    }

    if (g_queue_len == 0) {
        return (g_bg_len > 0) ? 1 : 0;
    }

    if (now < g_next_deadline_ms) {
        return 1;
    }

    if (g_current_step < g_queue_len) {
        const orch_step_t *step = &g_queue[g_current_step];
        if (step->type == STEP_ACT_STOP_SWITCHED) {
            g_t_switched_step1 = now;
            for (size_t b = 0; b < g_bg_len; b++) {
                if (g_bg_queue[b].anchor_step == 1) {
                    g_bg_queue[b].target_time_ms = g_t_switched_step1 + g_bg_queue[b].relative_offset_ms;
                }
            }
            g_current_step++;
            g_next_deadline_ms = now;
            return 1;
        } else if (step->type == STEP_ACT_SWITCHED) {
            g_t_switched_step4 = now;
            for (size_t b = 0; b < g_bg_len; b++) {
                if (g_bg_queue[b].anchor_step == 4) {
                    g_bg_queue[b].target_time_ms = g_t_switched_step4 + g_bg_queue[b].relative_offset_ms;
                }
            }
            pthread_mutex_lock(&g_act_lock);
            strncpy(g_current_act, g_target_act, sizeof(g_current_act) - 1);
            save_current_activity(g_current_act);
            g_state = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
            pthread_mutex_unlock(&g_act_lock);

            if (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) {
                hub_send_btstack_cmd("disconnect_host\n");
                system("killall -9 bt_reconnect.sh 2>/dev/null");
            }

            if (g_progress_cb) {
                g_progress_cb(g_target_act, g_state, (int)g_current_step + 1,
                              (int)g_queue_len, "Activity switched", g_cb_user_data);
            }
            g_current_step++;
            g_next_deadline_ms = now;
            return 1;
        } else {
            execute_step(step);
        }

        uint32_t delay = step->delay_ms ? step->delay_ms : 50;
        g_next_deadline_ms = now + delay;

        if (g_progress_cb) {
            g_progress_cb(g_target_act, g_state, (int)g_current_step + 1,
                          (int)g_queue_len, step->description, g_cb_user_data);
        }
        g_current_step++;
        return 1;
    }

    if (g_state != ORCH_STATE_RUNNING && g_state != ORCH_STATE_IDLE) {
        pthread_mutex_lock(&g_act_lock);
        strncpy(g_current_act, g_target_act, sizeof(g_current_act) - 1);
        save_current_activity(g_current_act);
        g_state = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
        pthread_mutex_unlock(&g_act_lock);
    }
    g_queue_len = 0;
    g_current_step = 0;

    pthread_mutex_lock(&g_act_lock);
    int is_poweroff = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0);
    pthread_mutex_unlock(&g_act_lock);

    if (is_poweroff) {
        hub_send_btstack_cmd("disconnect_host\n");
        system("killall -9 bt_reconnect.sh 2>/dev/null");
    }

    if (g_progress_cb) {
        g_progress_cb(g_current_act, g_state, 0, 0, "Transition completed", g_cb_user_data);
    }
    return (g_bg_len > 0) ? 1 : 0;
}

static int orch_compile_transition(const char *target_id) {
    orch_step_t *main_steps = (orch_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_step_t));
    orch_bg_step_t *bg_steps = (orch_bg_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_bg_step_t));
    if (!main_steps || !bg_steps) {
        free(main_steps);
        free(bg_steps);
        return -1;
    }
    size_t main_count = 0, bg_count = 0;
    uint32_t sw_ms = 0, end_ms = 0;
    if (orch_build_steps(g_current_act, target_id, main_steps, ORCH_MAX_STEPS, &main_count,
                         bg_steps, ORCH_MAX_STEPS, &bg_count, &sw_ms, &end_ms) != 0) {
        free(main_steps);
        free(bg_steps);
        return -1;
    }
    int rc = orch_enqueue_steps(main_steps, main_count);
    if (rc == 0) {
        orch_enqueue_bg_steps(bg_steps, bg_count);
    }
    free(main_steps);
    free(bg_steps);
    return rc;
}

static void orch_format_preview_json(const char *target_id,
                                     const orch_step_t *steps, size_t count,
                                     const orch_bg_step_t *bg_steps, size_t bg_count,
                                     uint32_t switched_at_ms, uint32_t true_end_ms,
                                     char *preview_json, size_t preview_len) {
    if (!preview_json || preview_len == 0) return;
    cJSON *pj = cJSON_CreateObject();
    cJSON_AddStringToObject(pj, "target", target_id ? target_id : ORCH_POWEROFF_ID);
    cJSON_AddNumberToObject(pj, "switchedDurationMs", switched_at_ms);
    cJSON_AddNumberToObject(pj, "trueEndMs", true_end_ms);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddStringToObject(st, "description", steps[i].description);
        cJSON_AddNumberToObject(st, "type", (int)steps[i].type);
        if (steps[i].type == STEP_ACT_SWITCHED || steps[i].type == STEP_ACT_STOP_SWITCHED) {
            cJSON_AddStringToObject(st, "stepType", "Switched");
        }
        if (steps[i].device_id[0]) cJSON_AddStringToObject(st, "deviceId", steps[i].device_id);
        if (steps[i].command_name[0]) cJSON_AddStringToObject(st, "command", steps[i].command_name);
        cJSON_AddNumberToObject(st, "delayMs", steps[i].delay_ms);
        cJSON_AddItemToArray(arr, st);
    }
    cJSON_AddItemToObject(pj, "steps", arr);

    cJSON *bg_arr = cJSON_CreateArray();
    for (size_t i = 0; i < bg_count; i++) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddStringToObject(st, "description", bg_steps[i].step.description);
        cJSON_AddNumberToObject(st, "type", (int)bg_steps[i].step.type);
        if (bg_steps[i].step.device_id[0]) cJSON_AddStringToObject(st, "deviceId", bg_steps[i].step.device_id);
        if (bg_steps[i].step.command_name[0]) cJSON_AddStringToObject(st, "command", bg_steps[i].step.command_name);
        cJSON_AddNumberToObject(st, "delayMs", bg_steps[i].step.delay_ms);
        cJSON_AddNumberToObject(st, "relativeOffsetMs", bg_steps[i].relative_offset_ms);
        cJSON_AddNumberToObject(st, "anchorStep", bg_steps[i].anchor_step);
        cJSON_AddItemToArray(bg_arr, st);
    }
    cJSON_AddItemToObject(pj, "backgroundSteps", bg_arr);

    char *pstr = cJSON_PrintUnformatted(pj);
    if (pstr) {
        snprintf(preview_json, preview_len, "%s", pstr);
        free(pstr);
    }
    cJSON_Delete(pj);
}

int orch_switch_activity(const char *target_activity_id, int dry_run,
                         char *preview_json, size_t preview_len) {
    if (!target_activity_id || !target_activity_id[0]) return -1;
    if (orch_is_busy()) return -2;

    if (dry_run) {
        if (preview_json && preview_len > 0) {
            orch_step_t *main_steps = (orch_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_step_t));
            orch_bg_step_t *bg_steps = (orch_bg_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_bg_step_t));
            if (main_steps && bg_steps) {
                size_t main_count = 0, bg_count = 0;
                uint32_t sw_ms = 0, end_ms = 0;
                orch_build_steps(g_current_act, target_activity_id, main_steps, ORCH_MAX_STEPS, &main_count,
                                 bg_steps, ORCH_MAX_STEPS, &bg_count, &sw_ms, &end_ms);
                orch_format_preview_json(target_activity_id, main_steps, main_count,
                                         bg_steps, bg_count, sw_ms, end_ms,
                                         preview_json, preview_len);
            }
            free(main_steps);
            free(bg_steps);
        }
        return 0;
    }

    /* Cancel any queued background commands from previous switch immediately */
    g_bg_len = 0;
    memset(g_bg_queue, 0, sizeof(g_bg_queue));

    strncpy(g_target_act, target_activity_id, sizeof(g_target_act) - 1);
    g_state = (strcmp(target_activity_id, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_STOPPING : ORCH_STATE_STARTING;

    int rc = orch_compile_transition(target_activity_id);
    if (rc != 0) {
        g_state = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
        return -1;
    }

    if (g_queue_len == 0) {
        pthread_mutex_lock(&g_act_lock);
        strncpy(g_current_act, g_target_act, sizeof(g_current_act) - 1);
        save_current_activity(g_current_act);
        g_state = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
        pthread_mutex_unlock(&g_act_lock);

        if (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) {
            hub_send_btstack_cmd("disconnect_host\n");
            system("killall -9 bt_reconnect.sh 2>/dev/null");
        }

        if (g_progress_cb) {
            g_progress_cb(g_target_act, g_state, 0, 0, "Transition completed", g_cb_user_data);
        }
        return 0;
    }

    if (g_progress_cb) {
        g_progress_cb(g_target_act, g_state, 0, (int)g_queue_len, "Starting switch...", g_cb_user_data);
    }
    return 0;
}

int orch_power_off(int dry_run, char *preview_json, size_t preview_len) {
    return orch_switch_activity(ORCH_POWEROFF_ID, dry_run, preview_json, preview_len);
}
