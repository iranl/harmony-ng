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
    g_bg_len = 0;
    memset(g_bg_queue, 0, sizeof(g_bg_queue));
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
                codex_send_btstack_cmd("disconnect_host\n");
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
        codex_send_btstack_cmd("disconnect_host\n");
        system("killall -9 bt_reconnect.sh 2>/dev/null");
    }

    if (g_progress_cb) {
        g_progress_cb(g_current_act, g_state, 0, 0, "Transition completed", g_cb_user_data);
    }
    return (g_bg_len > 0) ? 1 : 0;
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

static int orch_build_steps(const char *target_id,
                            orch_step_t *main_steps, size_t max_main, size_t *main_count_out,
                            orch_bg_step_t *bg_steps, size_t max_bg, size_t *bg_count_out,
                            uint32_t *switched_at_ms, uint32_t *true_end_ms) {
    if (main_count_out) *main_count_out = 0;
    if (bg_count_out) *bg_count_out = 0;
    if (switched_at_ms) *switched_at_ms = 0;
    if (true_end_ms) *true_end_ms = 0;
    if (!main_steps || max_main == 0) return -1;
    if (!target_id || !target_id[0]) target_id = ORCH_POWEROFF_ID;

    const char *leaving_id = g_current_act[0] ? g_current_act : ORCH_POWEROFF_ID;
    const char *entering_id = target_id;

    if (strcmp(leaving_id, entering_id) == 0) {
        return 0;
    }

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

    struct activity_item *leaving_act = NULL;
    struct activity_item *entering_act = NULL;
    for (int i = 0; i < act_inv->count; i++) {
        if (strcmp(act_inv->items[i].id, leaving_id) == 0) leaving_act = &act_inv->items[i];
        if (strcmp(act_inv->items[i].id, entering_id) == 0) entering_act = &act_inv->items[i];
    }

    size_t main_count = 0;
    size_t bg_count = 0;
    uint32_t t_sim = 0;
    uint32_t t_sw1 = 0;
    uint32_t t_sw4 = 0;

    /* 1. Stop sequence of leaving activity */
    if (leaving_act && leaving_act->stop_count > 0) {
        int sw1_idx = -1;
        for (int k = 0; k < leaving_act->stop_count; k++) {
            if (strcasecmp(leaving_act->stop_steps[k].type, "Switched") == 0 ||
                strcasecmp(leaving_act->stop_steps[k].type, "ActivitySwitched") == 0 ||
                strcasecmp(leaving_act->stop_steps[k].command, "Switched") == 0) {
                sw1_idx = k;
                break;
            }
        }

        int stop_limit = (sw1_idx >= 0) ? sw1_idx : leaving_act->stop_count;
        for (int k = 0; k < stop_limit && main_count < max_main; k++) {
            append_orch_step(main_steps, &main_count, &leaving_act->stop_steps[k], "Stop");
            t_sim += main_steps[main_count - 1].delay_ms;
        }

        if (sw1_idx >= 0) {
            t_sw1 = t_sim;
            if (main_count < max_main) {
                memset(&main_steps[main_count], 0, sizeof(orch_step_t));
                main_steps[main_count].type = STEP_ACT_STOP_SWITCHED;
                main_steps[main_count].delay_ms = 0;
                snprintf(main_steps[main_count].description, sizeof(main_steps[main_count].description), "Leaving activity switched marker");
                main_count++;
            }

            uint32_t cur_offset = 0;
            for (int k = sw1_idx + 1; k < leaving_act->stop_count; k++) {
                const struct activity_step *st = &leaving_act->stop_steps[k];
                int is_dl = (strcasecmp(st->type, "Delay") == 0);
                uint32_t dl = st->delay_ms ? (uint32_t)st->delay_ms : (is_dl ? 50 : 250);
                if (is_dl) {
                    cur_offset += dl;
                } else if (bg_steps && bg_count < max_bg) {
                    orch_bg_step_t *b = &bg_steps[bg_count++];
                    memset(b, 0, sizeof(*b));
                    size_t dummy = 0;
                    append_orch_step(&b->step, &dummy, st, "Stop tail");
                    b->relative_offset_ms = cur_offset;
                    b->anchor_step = 1;
                    cur_offset += b->step.delay_ms;
                }
            }
        }
    }

    /* 2. Power OFF departing devices (in leaving_act, but not in entering_act) */
    if (has_ir && leaving_act) {
        for (int i = leaving_act->device_count - 1; i >= 0 && main_count < max_main; i--) {
            const char *dev_id = leaving_act->device_ids[i];
            int in_new = 0;
            if (entering_act) {
                for (int j = 0; j < entering_act->device_count; j++) {
                    if (strcmp(entering_act->device_ids[j], dev_id) == 0) { in_new = 1; break; }
                }
            }
            if (!in_new) {
                struct ir_device *dev = NULL;
                for (int j = 0; j < ir_inv->device_count; j++) {
                    if (strcmp(ir_inv->devices[j].id, dev_id) == 0) {
                        dev = &ir_inv->devices[j];
                        break;
                    }
                }
                if (!dev || dev->is_power_always_on) continue;

                if (dev->power_off_count > 0 && dev->power_off_steps) {
                    for (int k = 0; k < dev->power_off_count && main_count < max_main; k++) {
                        const struct activity_step *st = &dev->power_off_steps[k];
                        memset(&main_steps[main_count], 0, sizeof(orch_step_t));
                        if (strcasecmp(st->type, "Delay") == 0) {
                            main_steps[main_count].type = STEP_ACT_DELAY;
                            main_steps[main_count].delay_ms = st->delay_ms ? (uint32_t)st->delay_ms : 50;
                            snprintf(main_steps[main_count].description, sizeof(main_steps[main_count].description),
                                     "Power off %s: Delay %dms", dev->name, st->delay_ms);
                        } else if (strcasecmp(st->type, "BTCommand") == 0) {
                            main_steps[main_count].type = STEP_ACT_BTHID;
                            strncpy(main_steps[main_count].command_name, st->command, sizeof(main_steps[main_count].command_name) - 1);
                            main_steps[main_count].delay_ms = st->delay_ms > 0 ? (uint32_t)st->delay_ms : 250;
                            snprintf(main_steps[main_count].description, sizeof(main_steps[main_count].description),
                                     "Power off %s: %s", dev->name, st->command);
                        } else {
                            main_steps[main_count].type = STEP_ACT_DEVICE_CMD;
                            strncpy(main_steps[main_count].device_id, st->device_id, sizeof(main_steps[main_count].device_id) - 1);
                            strncpy(main_steps[main_count].command_name, st->command, sizeof(main_steps[main_count].command_name) - 1);
                            main_steps[main_count].delay_ms = st->delay_ms > 0 ? (uint32_t)st->delay_ms : 250;
                            snprintf(main_steps[main_count].description, sizeof(main_steps[main_count].description),
                                     "Power off %s: %s", dev->name, st->command);
                        }
                        t_sim += main_steps[main_count].delay_ms;
                        main_count++;
                    }
                } else {
                    const char *cmd_name = NULL;
                    for (int k = 0; k < dev->command_count; k++) {
                        if (strcasecmp(dev->commands[k].name, "PowerOff") == 0) { cmd_name = dev->commands[k].name; break; }
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
                    if (cmd_name && main_count < max_main) {
                        memset(&main_steps[main_count], 0, sizeof(orch_step_t));
                        main_steps[main_count].type = STEP_ACT_DEVICE_CMD;
                        strncpy(main_steps[main_count].device_id, dev->id, sizeof(main_steps[main_count].device_id) - 1);
                        strncpy(main_steps[main_count].command_name, cmd_name, sizeof(main_steps[main_count].command_name) - 1);
                        main_steps[main_count].delay_ms = dev->inter_device_delay > 0 ? (uint32_t)dev->inter_device_delay : 250;
                        snprintf(main_steps[main_count].description, sizeof(main_steps[main_count].description), "Power off %s", dev->name);
                        t_sim += main_steps[main_count].delay_ms;
                        main_count++;
                    }
                }
            }
        }
    }

    /* 3. Power ON arriving devices (in entering_act, but not in leaving_act) */
    if (has_ir && entering_act) {
        int max_warmup = 0;
        for (int i = 0; i < entering_act->device_count && main_count < max_main; i++) {
            const char *dev_id = entering_act->device_ids[i];
            int in_old = 0;
            if (leaving_act) {
                for (int j = 0; j < leaving_act->device_count; j++) {
                    if (strcmp(leaving_act->device_ids[j], dev_id) == 0) { in_old = 1; break; }
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

                int in_start_seq = 0;
                for (int s = 0; s < entering_act->start_count; s++) {
                    if (strcmp(entering_act->start_steps[s].device_id, dev_id) == 0 &&
                        (strcasecmp(entering_act->start_steps[s].command, "PowerOn") == 0 ||
                         strcasecmp(entering_act->start_steps[s].command, "PowerToggle") == 0 ||
                         strcasecmp(entering_act->start_steps[s].command, "Power") == 0)) {
                        in_start_seq = 1;
                        break;
                    }
                }
                if (in_start_seq) continue;

                if (dev->power_on_count > 0 && dev->power_on_steps) {
                    for (int k = 0; k < dev->power_on_count && main_count < max_main; k++) {
                        const struct activity_step *st = &dev->power_on_steps[k];
                        memset(&main_steps[main_count], 0, sizeof(orch_step_t));
                        if (strcasecmp(st->type, "Delay") == 0) {
                            main_steps[main_count].type = STEP_ACT_DELAY;
                            main_steps[main_count].delay_ms = st->delay_ms ? (uint32_t)st->delay_ms : 50;
                            snprintf(main_steps[main_count].description, sizeof(main_steps[main_count].description),
                                     "Power on %s: Delay %dms", dev->name, st->delay_ms);
                        } else if (strcasecmp(st->type, "BTCommand") == 0) {
                            main_steps[main_count].type = STEP_ACT_BTHID;
                            strncpy(main_steps[main_count].command_name, st->command, sizeof(main_steps[main_count].command_name) - 1);
                            main_steps[main_count].delay_ms = st->delay_ms > 0 ? (uint32_t)st->delay_ms : 250;
                            snprintf(main_steps[main_count].description, sizeof(main_steps[main_count].description),
                                     "Power on %s: %s", dev->name, st->command);
                        } else {
                            main_steps[main_count].type = STEP_ACT_DEVICE_CMD;
                            strncpy(main_steps[main_count].device_id, st->device_id, sizeof(main_steps[main_count].device_id) - 1);
                            strncpy(main_steps[main_count].command_name, st->command, sizeof(main_steps[main_count].command_name) - 1);
                            main_steps[main_count].delay_ms = st->delay_ms > 0 ? (uint32_t)st->delay_ms : 250;
                            snprintf(main_steps[main_count].description, sizeof(main_steps[main_count].description),
                                     "Power on %s: %s", dev->name, st->command);
                        }
                        t_sim += main_steps[main_count].delay_ms;
                        main_count++;
                    }
                } else {
                    const char *cmd_name = NULL;
                    for (int k = 0; k < dev->command_count; k++) {
                        if (strcasecmp(dev->commands[k].name, "PowerOn") == 0) { cmd_name = dev->commands[k].name; break; }
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
                    if (cmd_name && main_count < max_main) {
                        memset(&main_steps[main_count], 0, sizeof(orch_step_t));
                        main_steps[main_count].type = STEP_ACT_DEVICE_CMD;
                        strncpy(main_steps[main_count].device_id, dev->id, sizeof(main_steps[main_count].device_id) - 1);
                        strncpy(main_steps[main_count].command_name, cmd_name, sizeof(main_steps[main_count].command_name) - 1);
                        main_steps[main_count].delay_ms = dev->inter_device_delay > 0 ? (uint32_t)dev->inter_device_delay : 250;
                        snprintf(main_steps[main_count].description, sizeof(main_steps[main_count].description), "Power on %s", dev->name);
                        t_sim += main_steps[main_count].delay_ms;
                        main_count++;
                    }
                }
                if (dev->power_on_delay > max_warmup) {
                    max_warmup = dev->power_on_delay;
                }
            }
        }
        if (max_warmup > 0 && main_count < max_main) {
            memset(&main_steps[main_count], 0, sizeof(orch_step_t));
            main_steps[main_count].type = STEP_ACT_DELAY;
            main_steps[main_count].delay_ms = (uint32_t)max_warmup;
            snprintf(main_steps[main_count].description, sizeof(main_steps[main_count].description), "Warmup delay %dms", max_warmup);
            t_sim += max_warmup;
            main_count++;
        }
    }

    /* 4. Start sequence of entering activity */
    if (entering_act && entering_act->start_count > 0) {
        int sw4_idx = -1;
        for (int k = 0; k < entering_act->start_count; k++) {
            if (strcasecmp(entering_act->start_steps[k].type, "Switched") == 0 ||
                strcasecmp(entering_act->start_steps[k].type, "ActivitySwitched") == 0 ||
                strcasecmp(entering_act->start_steps[k].command, "Switched") == 0) {
                sw4_idx = k;
                break;
            }
        }

        int start_limit = (sw4_idx >= 0) ? sw4_idx : entering_act->start_count;
        const char *pfx = (strcmp(entering_id, ORCH_POWEROFF_ID) == 0) ? "PowerOff" : "Starting";
        for (int k = 0; k < start_limit && main_count < max_main; k++) {
            append_orch_step(main_steps, &main_count, &entering_act->start_steps[k], pfx);
            t_sim += main_steps[main_count - 1].delay_ms;
        }

        t_sw4 = t_sim;
        if (main_count < max_main) {
            memset(&main_steps[main_count], 0, sizeof(orch_step_t));
            main_steps[main_count].type = STEP_ACT_SWITCHED;
            main_steps[main_count].delay_ms = 0;
            snprintf(main_steps[main_count].description, sizeof(main_steps[main_count].description), "Activity switched");
            main_count++;
        }

        if (sw4_idx >= 0) {
            uint32_t cur_offset = 0;
            for (int k = sw4_idx + 1; k < entering_act->start_count; k++) {
                const struct activity_step *st = &entering_act->start_steps[k];
                int is_dl = (strcasecmp(st->type, "Delay") == 0);
                uint32_t dl = st->delay_ms ? (uint32_t)st->delay_ms : (is_dl ? 50 : 250);
                if (is_dl) {
                    cur_offset += dl;
                } else if (bg_steps && bg_count < max_bg) {
                    orch_bg_step_t *b = &bg_steps[bg_count++];
                    memset(b, 0, sizeof(*b));
                    size_t dummy = 0;
                    append_orch_step(&b->step, &dummy, st, "Start tail");
                    b->relative_offset_ms = cur_offset;
                    b->anchor_step = 4;
                    cur_offset += b->step.delay_ms;
                }
            }
        }
    } else {
        /* No start sequence, evaluate Switched immediately */
        t_sw4 = t_sim;
        if (main_count < max_main) {
            memset(&main_steps[main_count], 0, sizeof(orch_step_t));
            main_steps[main_count].type = STEP_ACT_SWITCHED;
            main_steps[main_count].delay_ms = 0;
            snprintf(main_steps[main_count].description, sizeof(main_steps[main_count].description), "Activity switched");
            main_count++;
        }
    }

    if (has_ir) free_ir_inventory(ir_inv);
    free(act_inv);
    free(ir_inv);

    if (switched_at_ms) *switched_at_ms = t_sw4;
    uint32_t calculated_end = t_sw4;
    if (bg_steps) {
        for (size_t b = 0; b < bg_count; b++) {
            uint32_t base = (bg_steps[b].anchor_step == 1) ? t_sw1 : t_sw4;
            uint32_t finish = base + bg_steps[b].relative_offset_ms + bg_steps[b].step.delay_ms;
            if (finish > calculated_end) calculated_end = finish;
        }
    }
    if (true_end_ms) *true_end_ms = calculated_end;

    if (main_count_out) *main_count_out = main_count;
    if (bg_count_out) *bg_count_out = bg_count;
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
    orch_step_t *main_steps = (orch_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_step_t));
    orch_bg_step_t *bg_steps = (orch_bg_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_bg_step_t));
    if (!main_steps || !bg_steps) {
        free(main_steps);
        free(bg_steps);
        return -1;
    }
    size_t main_count = 0, bg_count = 0;
    uint32_t sw_ms = 0, end_ms = 0;
    if (orch_build_steps(target_id, main_steps, ORCH_MAX_STEPS, &main_count,
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
                orch_build_steps(target_activity_id, main_steps, ORCH_MAX_STEPS, &main_count,
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

    if (strcmp(target_activity_id, ORCH_POWEROFF_ID) != 0) {
        orch_trigger_bt_host_connect(target_activity_id);
    }

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
            codex_send_btstack_cmd("disconnect_host\n");
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
