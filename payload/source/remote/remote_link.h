#ifndef REMOTE_LINK_H
#define REMOTE_LINK_H

#include <stdbool.h>
#include <stdint.h>

/* RF link quality, measured from what the remote observes over the last 30 s */
typedef enum {
    LINK_OK,         /* bars 1-4 */
    LINK_NO_HUB,     /* the CC2544 reports the link lost */
    LINK_HUB_SILENT  /* the radio link is up, but the Hub doesn't answer */
} link_state_t;

typedef struct {
    link_state_t state;
    int bars;        /* 1-4 */
    int status_pct;  /* CC2544 link reports saying established; -1 without reports */
    int reply_pct;   /* sync queries the Hub answered within 1 s; -1 without queries */
} link_quality_t;

void link_note_status(bool established, uint64_t now_ms);
void link_note_query(uint8_t seq, uint64_t now_ms);
void link_note_reply(uint8_t seq, uint64_t now_ms);
void link_quality(uint64_t now_ms, link_quality_t *q);
void link_reset(void);

#endif
