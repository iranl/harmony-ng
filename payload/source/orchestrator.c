#include "orchestrator.h"
#include "hw_action.h"
#include "cJSON.h"
#include "config_store.h"
#include "resource_cache.h"
#include "codex_bt_common.h"

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

#define CURRENT_ACT_FILE "/data/codex/current_activity"

static orch_state_t g_state = ORCH_STATE_IDLE;
static char g_current_act[32] = ORCH_POWEROFF_ID;
static char g_target_act[32] = ORCH_POWEROFF_ID;
static pthread_mutex_t g_act_lock = PTHREAD_MUTEX_INITIALIZER;

static orch_step_t g_queue[ORCH_MAX_STEPS];
static size_t g_queue_len = 0;
static size_t g_current_step = 0;
static uint64_t g_next_deadline_ms = 0;

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
    snprintf(tmp, sizeof(tmp), "%s.tmp", CURRENT_ACT_FILE);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "%s\n", act_id);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
        rename(tmp, CURRENT_ACT_FILE);
        sync();
    }
}

static void load_current_activity(void) {
    FILE *f = fopen(CURRENT_ACT_FILE, "r");
    if (f) {
        char buf[32] = {0};
        if (fscanf(f, "%31s", buf) == 1 && buf[0]) {
            strncpy(g_current_act, buf, sizeof(g_current_act) - 1);
            g_current_act[sizeof(g_current_act) - 1] = '\0';
        }
        fclose(f);
    }
}

static void orch_trigger_bt_host_connect(const char *target_activity_id);

int orch_init(void) {
    pthread_mutex_lock(&g_act_lock);
    load_current_activity();
    g_state = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
    pthread_mutex_unlock(&g_act_lock);
    g_queue_len = 0;
    g_current_step = 0;
    g_next_deadline_ms = 0;
    if (g_state == ORCH_STATE_RUNNING && strcmp(g_current_act, ORCH_POWEROFF_ID) != 0) {
        orch_trigger_bt_host_connect(g_current_act);
    }
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
        codex_send_btstack_cmd("disconnect_host\n");
        system("killall -9 bt_reconnect.sh 2>/dev/null");
    } else {
        g_state = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
        pthread_mutex_unlock(&g_act_lock);
    }
    g_queue_len = 0;
    g_current_step = 0;
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
                    codex_send_btstack_cmd(cmd);
                } else {
                    codex_send_btstack_cmd("CONNECT");
                }
            } else if (strcasecmp(step->command_name, "DISCONNECT") == 0) {
                codex_send_btstack_cmd("DISCONNECT");
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
    if (g_queue_len == 0) {
        return 0;
    }

    uint64_t now = get_now_ms();
    if (now < g_next_deadline_ms) {
        return 1;
    }

    if (g_current_step < g_queue_len) {
        const orch_step_t *step = &g_queue[g_current_step];
        if (step->type == STEP_ACT_SWITCHED) {
            pthread_mutex_lock(&g_act_lock);
            strncpy(g_current_act, g_target_act, sizeof(g_current_act) - 1);
            save_current_activity(g_current_act);
            g_state = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
            pthread_mutex_unlock(&g_act_lock);
            if (g_progress_cb) {
                g_progress_cb(g_target_act, g_state, (int)g_current_step + 1,
                              (int)g_queue_len, "Activity switched", g_cb_user_data);
            }
        } else {
            execute_step(step);
        }

        uint32_t delay = step->delay_ms ? step->delay_ms : 50;
        g_next_deadline_ms = now + delay;

        if (g_progress_cb && step->type != STEP_ACT_SWITCHED) {
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
        codex_send_btstack_cmd("disconnect_host\n");
        system("killall -9 bt_reconnect.sh 2>/dev/null");
    }

    if (g_progress_cb) {
        g_progress_cb(g_current_act, g_state, 0, 0, "Transition completed", g_cb_user_data);
    }
    return 0;
}

static void append_orch_step(orch_step_t *steps, size_t *count, const struct activity_step *st, const char *prefix) {
    if (*count >= ORCH_MAX_STEPS) return;
    memset(&steps[*count], 0, sizeof(orch_step_t));
    if (strcasecmp(st->type, "Switched") == 0 ||
        strcasecmp(st->type, "ActivitySwitched") == 0 ||
        strcasecmp(st->command, "Switched") == 0) {
        steps[*count].type = STEP_ACT_SWITCHED;
        steps[*count].delay_ms = 0;
        snprintf(steps[*count].description, sizeof(steps[*count].description), "Activity switch completed (tail commands continue)");
    } else if (strcasecmp(st->type, "Delay") == 0) {
        steps[*count].type = STEP_ACT_DELAY;
        steps[*count].delay_ms = st->delay_ms ? (uint32_t)st->delay_ms : 50;
        snprintf(steps[*count].description, sizeof(steps[*count].description), "%s: Delay %dms", prefix, st->delay_ms);
    } else if (strcasecmp(st->type, "BTCommand") == 0) {
        steps[*count].type = STEP_ACT_BTHID;
        strncpy(steps[*count].command_name, st->command, sizeof(steps[*count].command_name) - 1);
        steps[*count].delay_ms = st->delay_ms > 0 ? (uint32_t)st->delay_ms : 250;
        snprintf(steps[*count].description, sizeof(steps[*count].description), "%s: %s", prefix, st->command);
    } else {
        steps[*count].type = STEP_ACT_DEVICE_CMD;
        strncpy(steps[*count].device_id, st->device_id, sizeof(steps[*count].device_id) - 1);
        strncpy(steps[*count].command_name, st->command, sizeof(steps[*count].command_name) - 1);
        steps[*count].delay_ms = st->delay_ms > 0 ? (uint32_t)st->delay_ms : 250;
        snprintf(steps[*count].description, sizeof(steps[*count].description), "%s: %s", prefix,
                 st->command[0] ? st->command : st->type);
    }
    (*count)++;
}

static int orch_build_steps(const char *target_id, orch_step_t *steps, size_t max_steps, size_t *count_out) {
    if (count_out) *count_out = 0;
    if (!steps || max_steps == 0) return -1;
    if (!target_id || !target_id[0]) target_id = ORCH_POWEROFF_ID;

    struct activity_inventory *act_inv = (struct activity_inventory *)calloc(1, sizeof(*act_inv));
    struct ir_inventory *ir_inv = (struct ir_inventory *)calloc(1, sizeof(*ir_inv));
    if (!act_inv || !ir_inv) {
        free(act_inv);
        free(ir_inv);
        return -1;
    }

    if (load_activity_inventory(act_inv) != 0) {
        free(act_inv);
        free(ir_inv);
        return -1;
    }

    int has_ir = (load_ir_inventory(ir_inv) == 0);

    int cur_idx = -1, target_idx = -1;
    for (int i = 0; i < act_inv->count; i++) {
        if (g_current_act[0] && strcmp(act_inv->items[i].id, g_current_act) == 0) cur_idx = i;
        if (strcmp(act_inv->items[i].id, target_id) == 0) target_idx = i;
    }

    size_t count = 0;

    /* Detect if departing activity stop sequence has a Switched point for early handover */
    int stop_switched_idx = -1;
    struct activity_item *old_act = NULL;
    if (cur_idx >= 0 && strcmp(g_current_act, target_id) != 0 && strcmp(g_current_act, ORCH_POWEROFF_ID) != 0) {
        old_act = &act_inv->items[cur_idx];
        for (int k = 0; k < old_act->stop_count; k++) {
            if (strcasecmp(old_act->stop_steps[k].type, "Switched") == 0 ||
                strcasecmp(old_act->stop_steps[k].type, "ActivitySwitched") == 0 ||
                strcasecmp(old_act->stop_steps[k].command, "Switched") == 0) {
                stop_switched_idx = k;
                break;
            }
        }
    }

    /* 1. Stop sequence head of departing activity (before Switched, or all if no Switched/powering off) */
    if (old_act) {
        int stop_limit = (stop_switched_idx >= 0 && strcmp(target_id, ORCH_POWEROFF_ID) != 0)
                         ? stop_switched_idx : old_act->stop_count;
        for (int k = 0; k < stop_limit && count < ORCH_MAX_STEPS; k++) {
            append_orch_step(steps, &count, &old_act->stop_steps[k], "Stop");
        }
    }

    /* 2. Power OFF departing devices (not present in new activity) */
    if (has_ir && cur_idx >= 0 && strcmp(g_current_act, target_id) != 0 && strcmp(g_current_act, ORCH_POWEROFF_ID) != 0) {
        struct activity_item *new_act = (target_idx >= 0) ? &act_inv->items[target_idx] : NULL;

        for (int i = old_act->device_count - 1; i >= 0 && count < ORCH_MAX_STEPS; i--) {
            const char *dev_id = old_act->device_ids[i];
            int in_new = 0;
            if (new_act) {
                for (int j = 0; j < new_act->device_count; j++) {
                    if (strcmp(new_act->device_ids[j], dev_id) == 0) { in_new = 1; break; }
                }
            }
            if (!in_new) {
                if (strcmp(target_id, ORCH_POWEROFF_ID) == 0 && target_idx >= 0 && act_inv->items[target_idx].start_count > 0) {
                    int in_poweroff_seq = 0;
                    for (int j = 0; j < act_inv->items[target_idx].start_count; j++) {
                        if (strcmp(act_inv->items[target_idx].start_steps[j].device_id, dev_id) == 0) {
                            in_poweroff_seq = 1;
                            break;
                        }
                    }
                    if (in_poweroff_seq) continue;
                }

                struct ir_device *dev = NULL;
                for (int j = 0; j < ir_inv->device_count; j++) {
                    if (strcmp(ir_inv->devices[j].id, dev_id) == 0) {
                        dev = &ir_inv->devices[j];
                        break;
                    }
                }
                if (!dev || dev->is_power_always_on) continue;

                if (dev->power_off_count > 0 && dev->power_off_steps) {
                    for (int k = 0; k < dev->power_off_count && count < ORCH_MAX_STEPS; k++) {
                        const struct activity_step *st = &dev->power_off_steps[k];
                        memset(&steps[count], 0, sizeof(orch_step_t));
                        if (strcasecmp(st->type, "Delay") == 0) {
                            steps[count].type = STEP_ACT_DELAY;
                            steps[count].delay_ms = st->delay_ms ? (uint32_t)st->delay_ms : 50;
                            snprintf(steps[count].description, sizeof(steps[count].description), "Power off %s: Delay %dms", dev->name, st->delay_ms);
                        } else if (strcasecmp(st->type, "BTCommand") == 0) {
                            steps[count].type = STEP_ACT_BTHID;
                            strncpy(steps[count].command_name, st->command, sizeof(steps[count].command_name) - 1);
                            steps[count].delay_ms = st->delay_ms > 0 ? (uint32_t)st->delay_ms : 250;
                            snprintf(steps[count].description, sizeof(steps[count].description), "Power off %s: %s", dev->name, st->command);
                        } else {
                            steps[count].type = STEP_ACT_DEVICE_CMD;
                            strncpy(steps[count].device_id, st->device_id, sizeof(steps[count].device_id) - 1);
                            strncpy(steps[count].command_name, st->command, sizeof(steps[count].command_name) - 1);
                            steps[count].delay_ms = st->delay_ms > 0 ? (uint32_t)st->delay_ms : 250;
                            snprintf(steps[count].description, sizeof(steps[count].description), "Power off %s: %s", dev->name, st->command);
                        }
                        count++;
                    }
                } else {
                    const char *cmd_name = NULL;
                    for (int k = 0; k < dev->command_count; k++) {
                        if (strcasecmp(dev->commands[k].name, "PowerOff") == 0) {
                            cmd_name = dev->commands[k].name;
                            break;
                        }
                    }
                    if (!cmd_name) {
                        for (int k = 0; k < dev->command_count; k++) {
                            if (strcasecmp(dev->commands[k].name, "PowerToggle") == 0 ||
                                strcasecmp(dev->commands[k].name, "Power") == 0) {
                                cmd_name = dev->commands[k].name;
                                break;
                            }
                        }
                    }
                    if (cmd_name && count < ORCH_MAX_STEPS) {
                        memset(&steps[count], 0, sizeof(orch_step_t));
                        steps[count].type = STEP_ACT_DEVICE_CMD;
                        strncpy(steps[count].device_id, dev->id, sizeof(steps[count].device_id) - 1);
                        strncpy(steps[count].command_name, cmd_name, sizeof(steps[count].command_name) - 1);
                        steps[count].delay_ms = dev->inter_device_delay > 0 ? (uint32_t)dev->inter_device_delay : 250;
                        snprintf(steps[count].description, sizeof(steps[count].description), "Power off %s", dev->name);
                        count++;
                    }
                }
            }
        }
    }

    /* 3. Power ON arriving devices (not present in old activity) */
    if (has_ir && target_idx >= 0 && strcmp(target_id, ORCH_POWEROFF_ID) != 0) {
        struct activity_item *new_act = &act_inv->items[target_idx];
        int max_warmup = 0;

        for (int i = 0; i < new_act->device_count && count < ORCH_MAX_STEPS; i++) {
            const char *dev_id = new_act->device_ids[i];
            int in_old = 0;
            if (old_act) {
                for (int j = 0; j < old_act->device_count; j++) {
                    if (strcmp(old_act->device_ids[j], dev_id) == 0) { in_old = 1; break; }
                }
            }
            if (!in_old) {
                struct ir_device *dev = NULL;
                for (int j = 0; j < ir_inv->device_count; j++) {
                    if (strcmp(ir_inv->devices[j].id, dev_id) == 0) {
                        dev = &ir_inv->devices[j];
                        break;
                    }
                }
                if (!dev || dev->is_power_always_on) continue;

                /* Skip power-on if activity start steps already explicitly issue power command */
                int in_start_seq = 0;
                for (int s = 0; s < new_act->start_count; s++) {
                    if (strcmp(new_act->start_steps[s].device_id, dev_id) == 0 &&
                        (strcasecmp(new_act->start_steps[s].command, "PowerOn") == 0 ||
                         strcasecmp(new_act->start_steps[s].command, "PowerToggle") == 0 ||
                         strcasecmp(new_act->start_steps[s].command, "Power") == 0)) {
                        in_start_seq = 1;
                        break;
                    }
                }
                if (in_start_seq) continue;

                if (dev->power_on_count > 0 && dev->power_on_steps) {
                    for (int k = 0; k < dev->power_on_count && count < ORCH_MAX_STEPS; k++) {
                        const struct activity_step *st = &dev->power_on_steps[k];
                        memset(&steps[count], 0, sizeof(orch_step_t));
                        if (strcasecmp(st->type, "Delay") == 0) {
                            steps[count].type = STEP_ACT_DELAY;
                            steps[count].delay_ms = st->delay_ms ? (uint32_t)st->delay_ms : 50;
                            snprintf(steps[count].description, sizeof(steps[count].description), "Power on %s: Delay %dms", dev->name, st->delay_ms);
                        } else if (strcasecmp(st->type, "BTCommand") == 0) {
                            steps[count].type = STEP_ACT_BTHID;
                            strncpy(steps[count].command_name, st->command, sizeof(steps[count].command_name) - 1);
                            steps[count].delay_ms = st->delay_ms > 0 ? (uint32_t)st->delay_ms : 250;
                            snprintf(steps[count].description, sizeof(steps[count].description), "Power on %s: %s", dev->name, st->command);
                        } else {
                            steps[count].type = STEP_ACT_DEVICE_CMD;
                            strncpy(steps[count].device_id, st->device_id, sizeof(steps[count].device_id) - 1);
                            strncpy(steps[count].command_name, st->command, sizeof(steps[count].command_name) - 1);
                            steps[count].delay_ms = st->delay_ms > 0 ? (uint32_t)st->delay_ms : 250;
                            snprintf(steps[count].description, sizeof(steps[count].description), "Power on %s: %s", dev->name, st->command);
                        }
                        count++;
                    }
                } else {
                    const char *cmd_name = NULL;
                    for (int k = 0; k < dev->command_count; k++) {
                        if (strcasecmp(dev->commands[k].name, "PowerOn") == 0) {
                            cmd_name = dev->commands[k].name;
                            break;
                        }
                    }
                    if (!cmd_name) {
                        for (int k = 0; k < dev->command_count; k++) {
                            if (strcasecmp(dev->commands[k].name, "PowerToggle") == 0 ||
                                strcasecmp(dev->commands[k].name, "Power") == 0) {
                                cmd_name = dev->commands[k].name;
                                break;
                            }
                        }
                    }
                    if (cmd_name && count < ORCH_MAX_STEPS) {
                        memset(&steps[count], 0, sizeof(orch_step_t));
                        steps[count].type = STEP_ACT_DEVICE_CMD;
                        strncpy(steps[count].device_id, dev->id, sizeof(steps[count].device_id) - 1);
                        strncpy(steps[count].command_name, cmd_name, sizeof(steps[count].command_name) - 1);
                        steps[count].delay_ms = dev->inter_device_delay > 0 ? (uint32_t)dev->inter_device_delay : 250;
                        snprintf(steps[count].description, sizeof(steps[count].description), "Power on %s", dev->name);
                        count++;
                    }
                }
                if (dev->power_on_delay > max_warmup) {
                    max_warmup = dev->power_on_delay;
                }
            }
        }
        if (max_warmup > 0 && count < ORCH_MAX_STEPS) {
            memset(&steps[count], 0, sizeof(orch_step_t));
            steps[count].type = STEP_ACT_DELAY;
            steps[count].delay_ms = (uint32_t)max_warmup;
            snprintf(steps[count].description, sizeof(steps[count].description), "Warmup delay %dms", max_warmup);
            count++;
        }
    }

    /* 4. Start sequence of arriving activity */
    if (target_idx >= 0) {
        struct activity_item *new_act = &act_inv->items[target_idx];
        const char *pfx = (strcmp(target_id, ORCH_POWEROFF_ID) == 0) ? "PowerOff" : "Starting";
        for (int k = 0; k < new_act->start_count && count < ORCH_MAX_STEPS; k++) {
            append_orch_step(steps, &count, &new_act->start_steps[k], pfx);
        }
    }

    /* 5. Departing activity stop sequence tail (after Switched marker) */
    if (old_act && stop_switched_idx >= 0 && strcmp(target_id, ORCH_POWEROFF_ID) != 0) {
        int already_switched = 0;
        for (size_t s = 0; s < count; s++) {
            if (steps[s].type == STEP_ACT_SWITCHED) { already_switched = 1; break; }
        }
        if (!already_switched && count < ORCH_MAX_STEPS) {
            memset(&steps[count], 0, sizeof(orch_step_t));
            steps[count].type = STEP_ACT_SWITCHED;
            steps[count].delay_ms = 0;
            snprintf(steps[count].description, sizeof(steps[count].description), "Activity switch completed (tail commands continue)");
            count++;
        }
        for (int k = stop_switched_idx + 1; k < old_act->stop_count && count < ORCH_MAX_STEPS; k++) {
            append_orch_step(steps, &count, &old_act->stop_steps[k], "Stop tail");
        }
    }

    if (has_ir) free_ir_inventory(ir_inv);
    free(act_inv);
    free(ir_inv);

    if (count_out) *count_out = count;
    return 0;
}

static void orch_trigger_bt_host_connect(const char *target_activity_id) {
    if (!target_activity_id || !target_activity_id[0] || strcmp(target_activity_id, ORCH_POWEROFF_ID) == 0) return;

    struct activity_inventory *act_inv = (struct activity_inventory *)calloc(1, sizeof(*act_inv));
    if (!act_inv) return;
    if (load_activity_inventory(act_inv) != 0) {
        free(act_inv);
        return;
    }

    int act_idx = -1;
    for (int i = 0; i < act_inv->count; i++) {
        if (strcmp(act_inv->items[i].id, target_activity_id) == 0) {
            act_idx = i;
            break;
        }
    }
    if (act_idx < 0) {
        free(act_inv);
        return;
    }

    const struct activity_item *act = &act_inv->items[act_idx];
    char target_bdaddr[32] = {0};
    int found_bt = 0;
    int target_dev_bt_reconnect = -1;
    int target_dev_duration = 0;
    int target_dev_delay = -1;

    /* 1. Check BT inventory */
    struct bt_inventory *bt_inv = (struct bt_inventory *)calloc(1, sizeof(*bt_inv));
    if (bt_inv) {
        if (load_bt_inventory(bt_inv) == 0) {
            for (int i = 0; i < act->device_count; i++) {
                for (int j = 0; j < bt_inv->device_count; j++) {
                    if (strcmp(act->device_ids[i], bt_inv->devices[j].id) == 0) {
                        found_bt = 1;
                        if (bt_inv->devices[j].bdaddr[0] && codex_valid_bdaddr(bt_inv->devices[j].bdaddr)) {
                            strncpy(target_bdaddr, bt_inv->devices[j].bdaddr, sizeof(target_bdaddr) - 1);
                            break;
                        }
                    }
                }
                if (target_bdaddr[0]) break;
            }
        }
        free(bt_inv);
    }

    /* 2. Check DeviceList.json */
    cJSON *root = get_device_list_cached();
    if (root) {
        cJSON *dwf = cJSON_GetObjectItemCaseSensitive(root, "devices");
        if (!dwf) dwf = cJSON_GetObjectItemCaseSensitive(root, "DevicesWithFeatures");
        if (dwf && cJSON_IsArray(dwf)) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, dwf) {
                cJSON *dev = cJSON_GetObjectItemCaseSensitive(item, "Device");
                if (!dev) dev = item;
                cJSON *jid = cJSON_GetObjectItemCaseSensitive(dev, "id");
                if (!jid) jid = cJSON_GetObjectItemCaseSensitive(dev, "Id-");
                if (!jid) jid = cJSON_GetObjectItemCaseSensitive(dev, "Id");
                char id_str[32] = {0};
                if (jid) {
                    if (cJSON_IsNumber(jid)) snprintf(id_str, sizeof(id_str), "%ld", (long)jid->valuedouble);
                    else if (cJSON_IsString(jid) && jid->valuestring) strncpy(id_str, jid->valuestring, sizeof(id_str) - 1);
                }
                if (!id_str[0]) continue;
                for (int i = 0; i < act->device_count; i++) {
                    if (strcmp(act->device_ids[i], id_str) == 0) {
                        cJSON *j_trans = cJSON_GetObjectItemCaseSensitive(dev, "transport");
                        if (!j_trans) j_trans = cJSON_GetObjectItemCaseSensitive(dev, "Transport");
                        if (!j_trans) j_trans = cJSON_GetObjectItemCaseSensitive(dev, "TransportType");
                        cJSON *j_isbt = cJSON_GetObjectItemCaseSensitive(dev, "isBt");
                        int is_bt = (j_trans && cJSON_IsNumber(j_trans) && (int)j_trans->valuedouble == 32) ||
                                    (j_isbt && cJSON_IsTrue(j_isbt)) || target_bdaddr[0];
                        if (is_bt) {
                            found_bt = 1;
                            cJSON *j_bta = cJSON_GetObjectItemCaseSensitive(dev, "BTAddress");
                            if (!j_bta) j_bta = cJSON_GetObjectItemCaseSensitive(dev, "bt_host");
                            if (!j_bta) j_bta = cJSON_GetObjectItemCaseSensitive(dev, "bdaddr");
                            if (!target_bdaddr[0] && j_bta && cJSON_IsString(j_bta) && j_bta->valuestring && codex_valid_bdaddr(j_bta->valuestring)) {
                                strncpy(target_bdaddr, j_bta->valuestring, sizeof(target_bdaddr) - 1);
                            }
                            cJSON *j_bar = cJSON_GetObjectItemCaseSensitive(dev, "bt_auto_reconnect");
                            if (!j_bar) j_bar = cJSON_GetObjectItemCaseSensitive(dev, "btAutoReconnect");
                            if (j_bar) {
                                if (cJSON_IsNumber(j_bar)) target_dev_bt_reconnect = j_bar->valueint;
                                else if (cJSON_IsBool(j_bar)) target_dev_bt_reconnect = cJSON_IsTrue(j_bar) ? 1 : 0;
                            }
                            cJSON *j_bdur = cJSON_GetObjectItemCaseSensitive(dev, "bt_reconnect_duration_sec");
                            if (!j_bdur) j_bdur = cJSON_GetObjectItemCaseSensitive(dev, "btReconnectDurationSec");
                            if (j_bdur && cJSON_IsNumber(j_bdur)) target_dev_duration = j_bdur->valueint;

                            cJSON *j_bdel = cJSON_GetObjectItemCaseSensitive(dev, "bt_reconnect_delay_sec");
                            if (!j_bdel) j_bdel = cJSON_GetObjectItemCaseSensitive(dev, "btReconnectDelaySec");
                            if (j_bdel && cJSON_IsNumber(j_bdel)) target_dev_delay = j_bdel->valueint;
                            break;
                        }
                    }
                }
                if (target_bdaddr[0]) break;
            }
        }
    }

    /* 3. Check start steps for BTCommand */
    if (!found_bt) {
        for (int k = 0; k < act->start_count; k++) {
            if (strcasecmp(act->start_steps[k].type, "BTCommand") == 0) {
                found_bt = 1;
                break;
            }
        }
    }

    if (found_bt || target_bdaddr[0]) {
        struct codex_settings settings;
        config_store_get_settings(&settings);

        int should_reconnect = settings.bt_auto_reconnect;
        if (target_dev_bt_reconnect == 0) should_reconnect = 0;
        else if (target_dev_bt_reconnect == 1) should_reconnect = 1;

        if (!should_reconnect) {
            free(act_inv);
            return;
        }

        int delay_sec = target_dev_delay >= 0 ? target_dev_delay : (settings.bt_reconnect_delay_sec >= 0 ? settings.bt_reconnect_delay_sec : 2);
        int duration = target_dev_duration > 0 ? target_dev_duration : (settings.bt_reconnect_duration_sec > 0 ? settings.bt_reconnect_duration_sec : 30);

        char sh_cmd[160];
        if (target_bdaddr[0] && codex_valid_bdaddr(target_bdaddr)) {
            save_bthid_target("btkeyboard", target_bdaddr);
            snprintf(sh_cmd, sizeof(sh_cmd), "/data/codex/bt_reconnect.sh %s %d %d >/dev/null 2>&1 &", target_bdaddr, duration, delay_sec);
            system(sh_cmd);
        } else {
            snprintf(sh_cmd, sizeof(sh_cmd), "/data/codex/bt_reconnect.sh \"\" %d %d >/dev/null 2>&1 &", duration, delay_sec);
            system(sh_cmd);
        }
        if (delay_sec == 0) {
            char cmd[128];
            if (target_bdaddr[0]) snprintf(cmd, sizeof(cmd), "connect_host %s %d\n", target_bdaddr, duration);
            else snprintf(cmd, sizeof(cmd), "connect_host %d\n", duration);
            codex_send_btstack_cmd(cmd);
        }
    }
    free(act_inv);
}

static int orch_compile_transition(const char *target_id) {
    orch_step_t *steps = (orch_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_step_t));
    if (!steps) return -1;
    size_t count = 0;
    if (orch_build_steps(target_id, steps, ORCH_MAX_STEPS, &count) != 0) {
        free(steps);
        return -1;
    }
    int rc = orch_enqueue_steps(steps, count);
    free(steps);
    return rc;
}

static void orch_format_preview_json(const char *target_id, const orch_step_t *steps, size_t count,
                                     char *preview_json, size_t preview_len) {
    if (!preview_json || preview_len == 0) return;
    cJSON *pj = cJSON_CreateObject();
    cJSON_AddStringToObject(pj, "target", target_id ? target_id : ORCH_POWEROFF_ID);
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddStringToObject(st, "description", steps[i].description);
        cJSON_AddNumberToObject(st, "type", (int)steps[i].type);
        if (steps[i].type == STEP_ACT_SWITCHED) {
            cJSON_AddStringToObject(st, "stepType", "Switched");
        }
        if (steps[i].device_id[0]) cJSON_AddStringToObject(st, "deviceId", steps[i].device_id);
        if (steps[i].command_name[0]) cJSON_AddStringToObject(st, "command", steps[i].command_name);
        cJSON_AddNumberToObject(st, "delayMs", steps[i].delay_ms);
        cJSON_AddItemToArray(arr, st);
    }
    cJSON_AddItemToObject(pj, "steps", arr);
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

    if (strcmp(target_activity_id, ORCH_POWEROFF_ID) == 0) {
        return orch_power_off(dry_run, preview_json, preview_len);
    }

    if (dry_run) {
        if (preview_json && preview_len > 0) {
            orch_step_t *steps = (orch_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_step_t));
            if (steps) {
                size_t count = 0;
                orch_build_steps(target_activity_id, steps, ORCH_MAX_STEPS, &count);
                orch_format_preview_json(target_activity_id, steps, count, preview_json, preview_len);
                free(steps);
            }
        }
        return 0;
    }

    strncpy(g_target_act, target_activity_id, sizeof(g_target_act) - 1);
    g_state = ORCH_STATE_STARTING;

    orch_trigger_bt_host_connect(target_activity_id);

    int rc = orch_compile_transition(target_activity_id);
    if (rc != 0) {
        g_state = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
        return -1;
    }

    if (g_queue_len == 0) {
        pthread_mutex_lock(&g_act_lock);
        strncpy(g_current_act, g_target_act, sizeof(g_current_act) - 1);
        save_current_activity(g_current_act);
        g_state = ORCH_STATE_RUNNING;
        pthread_mutex_unlock(&g_act_lock);
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
    if (orch_is_busy()) return -2;

    if (dry_run) {
        if (preview_json && preview_len > 0) {
            orch_step_t *steps = (orch_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_step_t));
            if (steps) {
                size_t count = 0;
                orch_build_steps(ORCH_POWEROFF_ID, steps, ORCH_MAX_STEPS, &count);
                orch_format_preview_json(ORCH_POWEROFF_ID, steps, count, preview_json, preview_len);
                free(steps);
            }
        }
        return 0;
    }

    strncpy(g_target_act, ORCH_POWEROFF_ID, sizeof(g_target_act) - 1);
    g_state = ORCH_STATE_STOPPING;
    codex_send_btstack_cmd("disconnect_host\n");
    system("killall -9 bt_reconnect.sh 2>/dev/null");

    int rc = orch_compile_transition(ORCH_POWEROFF_ID);
    if (rc != 0) {
        g_state = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
        return -1;
    }

    if (g_queue_len == 0) {
        pthread_mutex_lock(&g_act_lock);
        strncpy(g_current_act, ORCH_POWEROFF_ID, sizeof(g_current_act) - 1);
        save_current_activity(g_current_act);
        g_state = ORCH_STATE_IDLE;
        pthread_mutex_unlock(&g_act_lock);
        codex_send_btstack_cmd("disconnect_host\n");
        system("killall -9 bt_reconnect.sh 2>/dev/null");
        if (g_progress_cb) {
            g_progress_cb(ORCH_POWEROFF_ID, g_state, 0, 0, "Transition completed", g_cb_user_data);
        }
        return 0;
    }

    if (g_progress_cb) {
        g_progress_cb(g_target_act, g_state, 0, (int)g_queue_len, "Starting switch...", g_cb_user_data);
    }
    return 0;
}
