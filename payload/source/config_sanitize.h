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

/* A device's "pressMinRepeats" (the frames an infrared press sends after the first, as the
 * Logitech account's PressMinRepeats): 0..PRESS_MIN_REPEATS_MAX, a larger number is lowered.
 * Returns 1 with the value in *out for a number or a numeric text, 0 for "not set" (null, "",
 * a negative number: the Hub's default applies) and -1 for anything else. */
#define PRESS_MIN_REPEATS_MAX 20
int press_min_repeats_value(const cJSON *j, int *out);

void sanitize_devices_json(cJSON *root);
void sanitize_activity_steps(cJSON *steps);
void sanitize_activities_json(cJSON *root);
/* The same; returns 1 when the list was put in Display Order or an activity got an order */
int sanitize_activities_json_changed(cJSON *root);
/* HTTP/TCP devices (net_device.h): the stored httpPassword/httpToken as "" plus
 * hasHttpPassword/hasHttpToken; returns how many were masked */
int config_mask_device_secrets(cJSON *root);
int config_devices_have_secrets(const cJSON *root);
/* host name or IP address of an HTTP/TCP device */
int net_host_valid(const char *host);
/* an HTTP header name an HTTP device may set */
int net_header_name_ok(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_SANITIZE_H */
