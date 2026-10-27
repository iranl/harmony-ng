#define _GNU_SOURCE
#include "orchestrator_queue.h"
#include "config_store.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void append_orch_step(orch_step_t *steps, size_t *count, const struct activity_step *st, const char *prefix) {
    if (!steps || !count || !st || *count >= ORCH_MAX_STEPS) return;
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

int orch_build_steps(const char *current_id, const char *target_id,
                     orch_step_t *main_steps, size_t max_main, size_t *main_count_out,
                     orch_bg_step_t *bg_steps, size_t max_bg, size_t *bg_count_out,
                     uint32_t *switched_at_ms, uint32_t *true_end_ms) {
    if (main_count_out) *main_count_out = 0;
    if (bg_count_out) *bg_count_out = 0;
    if (switched_at_ms) *switched_at_ms = 0;
    if (true_end_ms) *true_end_ms = 0;
    if (!main_steps || max_main == 0) return -1;
    if (!target_id || !target_id[0]) target_id = ORCH_POWEROFF_ID;

    const char *leaving_id = (current_id && current_id[0]) ? current_id : ORCH_POWEROFF_ID;
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
        for (int i = 0; i < leaving_act->device_count && main_count < max_main; i++) {
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

                int in_stop_seq = 0;
                for (int s = 0; s < leaving_act->stop_count; s++) {
                    if (strcmp(leaving_act->stop_steps[s].device_id, dev_id) == 0 &&
                        (strcasecmp(leaving_act->stop_steps[s].command, "PowerOff") == 0 ||
                         strcasecmp(leaving_act->stop_steps[s].command, "PowerToggle") == 0 ||
                         strcasecmp(leaving_act->stop_steps[s].command, "Power") == 0)) {
                        in_stop_seq = 1;
                        break;
                    }
                }
                if (in_stop_seq) continue;

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
