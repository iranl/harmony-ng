#ifndef CONFIG_SANITIZE_H
#define CONFIG_SANITIZE_H

#include <stddef.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The per-device send delays (interDeviceDelay, interKeyDelay, holdInterKeyDelay) are waited out
 * by the sender, which blocks the caller: a save outside 0..DEVICE_DELAY_MAX_MS is refused and a
 * loaded value above it is lowered to it. */
#define DEVICE_DELAY_MAX_MS 10000

/* Waits of an activity: a device's warm-up (powerOnDelay) and a Delay step of a sequence. A start
 * runs them in order and every request queues behind it, so a value with a few zeros too many
 * kept the Hub in "starting" for hours: a loaded or saved value above the limit is lowered. */
#define POWER_ON_DELAY_MAX_MS 120000
#define STEP_DELAY_MAX_MS 120000

void sanitize_devices_json(cJSON *root);
void sanitize_activity_steps(cJSON *steps);
void sanitize_activities_json(cJSON *root);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_SANITIZE_H */
