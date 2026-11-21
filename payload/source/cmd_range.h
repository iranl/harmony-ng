#ifndef CMD_RANGE_H
#define CMD_RANGE_H

/* A command's optional "range": [min, max], the values its plugin accepts (a slider, a level).
 * Kept through device saves and reloads; hw_action refuses a value outside it. */

#include "cJSON.h"

/* 1 for an array of two integers with min <= max */
static inline int cmd_range_valid(const cJSON *r) {
    if (!cJSON_IsArray(r) || cJSON_GetArraySize(r) != 2) return 0;
    const cJSON *lo = cJSON_GetArrayItem(r, 0), *hi = cJSON_GetArrayItem(r, 1);
    if (!cJSON_IsNumber(lo) || !cJSON_IsNumber(hi)) return 0;
    if (lo->valuedouble != (double)lo->valueint || hi->valuedouble != (double)hi->valueint) return 0;
    return lo->valueint <= hi->valueint;
}

/* Copies a valid "range" of src to dst (replacing dst's); an invalid or missing one is left out */
static inline void cmd_range_copy(cJSON *dst, const cJSON *src) {
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(src, "range");
    if (!cmd_range_valid(r)) return;
    cJSON_DeleteItemFromObjectCaseSensitive(dst, "range");
    cJSON *n = cJSON_CreateArray();
    cJSON_AddItemToArray(n, cJSON_CreateNumber(cJSON_GetArrayItem(r, 0)->valueint));
    cJSON_AddItemToArray(n, cJSON_CreateNumber(cJSON_GetArrayItem(r, 1)->valueint));
    cJSON_AddItemToObject(dst, "range", n);
}

#endif
