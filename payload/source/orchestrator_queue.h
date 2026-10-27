#ifndef ORCHESTRATOR_QUEUE_H
#define ORCHESTRATOR_QUEUE_H

#include "orchestrator.h"
#include "config_store.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void append_orch_step(orch_step_t *steps, size_t *count, const struct activity_step *st, const char *prefix);

int orch_build_steps(const char *current_id, const char *target_id,
                     orch_step_t *main_steps, size_t max_main, size_t *main_count_out,
                     orch_bg_step_t *bg_steps, size_t max_bg, size_t *bg_count_out,
                     uint32_t *switched_at_ms, uint32_t *true_end_ms);

#ifdef __cplusplus
}
#endif

#endif /* ORCHESTRATOR_QUEUE_H */
