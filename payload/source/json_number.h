#ifndef JSON_NUMBER_H
#define JSON_NUMBER_H

/* JSON numbers used as ids and integer settings. cJSON keeps every number as a double, and
 * converting a double outside the range of the target type is undefined behaviour; long has 32
 * bits on the Hub. Header only, so every daemon and test can use it without a source list
 * change. */

#include <limits.h>
#include <stdio.h>
#include "cJSON.h"

/* Writes the whole number j as text ("%ld") when it is a number in the 32-bit range and returns
 * 1; otherwise writes "" and returns 0 (a bad id). */
static inline int json_number_text(const cJSON *j, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!cJSON_IsNumber(j)) return 0;
    double v = j->valuedouble;
    if (!(v > -2147483649.0 && v < 2147483648.0)) return 0; /* also NaN */
    snprintf(out, cap, "%ld", (long)v);
    return 1;
}

/* j as an int clamped to lo..hi; def when it is not a number */
static inline int json_number_int(const cJSON *j, int lo, int hi, int def) {
    if (!cJSON_IsNumber(j)) return def;
    double v = j->valuedouble;
    if (v != v) return def;
    if (v <= (double)lo) return lo;
    if (v >= (double)hi) return hi;
    return (int)v;
}

#endif /* JSON_NUMBER_H */
