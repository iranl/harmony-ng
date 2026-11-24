/* The RF slot table (/data/harmony/rf_slot_map.txt): which CC2544 pipe each of the six remote
 * slots is, as six different numbers from 1 to 6 ("1 4 3 2 5 6": slot 2 is pipe 4). One check
 * for hub_rf (reading the file at its start, following a restored file before a restart) and
 * hub_daemon (a backup import), so both agree on what a table is. Header only: no build list
 * changes. */
#ifndef RF_SLOT_TABLE_H
#define RF_SLOT_TABLE_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#define RF_SLOT_TABLE_SLOTS 6

/* 1 and pipes[0..5] filled when text starts with six different numbers from 1 to 6 (separated
 * by blanks; what follows them is ignored, as the fscanf() reader of hub_rf always did), else 0
 * and pipes set to 1..6 (slots and pipes the same, what hub_rf uses without a table) */
static inline int rf_slot_table_parse(const char *text, uint8_t *pipes) {
    unsigned seen = 0;
    const char *p = text;
    int ok = text != NULL;
    for (int i = 0; i < RF_SLOT_TABLE_SLOTS && ok; i++) {
        char *end = NULL;
        long v = p ? strtol(p, &end, 10) : 0;
        if (!p || end == p || v < 1 || v > RF_SLOT_TABLE_SLOTS || (seen & (1u << v))) {
            ok = 0;
        } else {
            seen |= 1u << v;
            pipes[i] = (uint8_t)v;
            p = end;
        }
    }
    if (!ok)
        for (int i = 0; i < RF_SLOT_TABLE_SLOTS; i++) pipes[i] = (uint8_t)(i + 1);
    return ok;
}

/* The same for a file; a missing or unreadable file is 0 (no table) */
static inline int rf_slot_table_read(const char *path, uint8_t *pipes) {
    char text[128] = "";
    FILE *f = fopen(path, "r");
    if (f) {
        size_t n = fread(text, 1, sizeof(text) - 1, f);
        text[n] = '\0';
        fclose(f);
    }
    return rf_slot_table_parse(f ? text : NULL, pipes);
}

/* "a b c d e f\n" into out (at least 16 bytes) */
static inline void rf_slot_table_format(const uint8_t *pipes, char *out, size_t len) {
    snprintf(out, len, "%u %u %u %u %u %u\n", pipes[0], pipes[1], pipes[2], pipes[3], pipes[4], pipes[5]);
}

#endif /* RF_SLOT_TABLE_H */
