#ifndef CONFIG_SANITIZE_H
#define CONFIG_SANITIZE_H

#include <stddef.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

void sanitize_devices_json(cJSON *root);
void sanitize_activity_steps(cJSON *steps);
void sanitize_activities_json(cJSON *root);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_SANITIZE_H */
