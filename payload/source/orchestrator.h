#ifndef CODEX_ORCHESTRATOR_H
#define CODEX_ORCHESTRATOR_H

#include <stddef.h>
#include <stdint.h>
#include "hw_action.h"

#define ORCH_MAX_STEPS 128
#define ORCH_POWEROFF_ID "-1"

typedef enum {
    ORCH_STATE_IDLE = 0,
    ORCH_STATE_STARTING,
    ORCH_STATE_RUNNING,
    ORCH_STATE_STOPPING
} orch_state_t;

typedef enum {
    STEP_ACT_DELAY = 0,
    STEP_ACT_IR_PRONTO,
    STEP_ACT_DEVICE_CMD,
    STEP_ACT_BTHID,
    STEP_ACT_SWITCHED,
    STEP_ACT_STOP_SWITCHED
} step_action_type_t;

typedef struct {
    step_action_type_t type;
    char device_id[32];
    char command_name[64];
    char pronto_hex[512];
    uint8_t ir_ports;
    uint8_t ir_repeats;
    uint32_t delay_ms;
    char description[128];
} orch_step_t;

typedef struct {
    orch_step_t step;
    uint64_t target_time_ms;
    uint32_t relative_offset_ms;
    int anchor_step; /* 1 = Step 1 stop switched, 4 = Step 4 start switched */
    int executed;
} orch_bg_step_t;

/* Progress callback for WebSocket broadcasts */
typedef void (*orch_progress_cb)(const char *activity_id, orch_state_t state,
                                 int current_step, int total_steps,
                                 const char *step_desc, void *user_data);

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the orchestrator */
int orch_init(void);

/* Register progress notification callback */
void orch_set_progress_callback(orch_progress_cb cb, void *user_data);

/* Check if an activity transition is currently executing (interlock) */
int orch_is_busy(void);

/* Current activity ID string ("-1" = PowerOff) */
const char *orch_get_current_activity(void);

/* Get current state enum */
orch_state_t orch_get_state(void);

/*
 * Switch activity with interlock checks.
 * If dry_run != 0, does not execute; populates preview_json with the step list.
 * Returns 0 on success, -1 if busy, negative on error.
 */
int orch_switch_activity(const char *target_activity_id, int dry_run,
                         char *preview_json, size_t preview_len);

/*
 * Power off all devices in current activity.
 */
int orch_power_off(int dry_run, char *preview_json, size_t preview_len);

/*
 * Directly enqueue a step sequence for execution.
 */
int orch_enqueue_steps(const orch_step_t *steps, size_t count);

/*
 * Non-blocking event tick called from main event loop.
 * Executes queued steps when timers expire.
 * Returns 1 if busy/running, 0 if idle.
 */
int orch_tick(void);

/*
 * Cancel running sequence immediately.
 */
void orch_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* CODEX_ORCHESTRATOR_H */
