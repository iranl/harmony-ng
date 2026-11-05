/* RF link quality. Neither CC2544 exposes RSSI, so the remote measures the link from
 * two things it sees: the CC2544's link status reports, and whether the Hub answers the sync
 * queries the remote sends every 800 ms while the screen is on. */
#include "elite_link.h"
#include "elite_state.h"

#include <pthread.h>
#include <string.h>

#define LINK_WINDOW_MS   30000
#define REPLY_TIMEOUT_MS 1000
#define SILENT_MS        10000
#define SILENT_MIN_QUERIES 8  /* queries in SILENT_MS before "no reply" means the Hub is silent */
#define RING 64

static struct { uint64_t t; bool ok; } s_status[RING];
static struct { uint64_t t; uint8_t seq; bool answered; } s_query[RING];
static int s_status_n, s_status_head, s_query_n, s_query_head;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

void link_note_status(bool established, uint64_t now_ms) {
    pthread_mutex_lock(&s_lock);
    s_status[s_status_head].t = now_ms;
    s_status[s_status_head].ok = established;
    s_status_head = (s_status_head + 1) % RING;
    if (s_status_n < RING) s_status_n++;
    pthread_mutex_unlock(&s_lock);
}

void link_note_query(uint8_t seq, uint64_t now_ms) {
    pthread_mutex_lock(&s_lock);
    s_query[s_query_head].t = now_ms;
    s_query[s_query_head].seq = seq;
    s_query[s_query_head].answered = false;
    s_query_head = (s_query_head + 1) % RING;
    if (s_query_n < RING) s_query_n++;
    pthread_mutex_unlock(&s_lock);
}

/* The Hub answers a sync query with ACTIVITY_SYNC carrying the query's sequence number. Frames
   carry only its low 4 bits; at one query per 800 ms they repeat every 12.8 s, well beyond the
   1 s a reply may take. */
void link_note_reply(uint8_t seq, uint64_t now_ms) {
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_query_n; i++) {
        int k = (s_query_head - 1 - i + RING) % RING;
        if (ms_since(now_ms, s_query[k].t) > REPLY_TIMEOUT_MS) break;
        if ((s_query[k].seq & 0x0F) == (seq & 0x0F)) {
            s_query[k].answered = true;
            break;
        }
    }
    pthread_mutex_unlock(&s_lock);
}

void link_reset(void) {
    pthread_mutex_lock(&s_lock);
    s_status_n = s_status_head = s_query_n = s_query_head = 0;
    pthread_mutex_unlock(&s_lock);
}

static int pct(int part, int total) {
    return total > 0 ? part * 100 / total : -1;
}

void link_quality(uint64_t now_ms, link_quality_t *q) {
    int st_ok = 0, st_n = 0, qr_ok = 0, qr_n = 0, recent_n = 0, recent_ok = 0;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_status_n; i++) {
        if (ms_since(now_ms, s_status[i].t) > LINK_WINDOW_MS) continue;
        st_n++;
        if (s_status[i].ok) st_ok++;
    }
    for (int i = 0; i < s_query_n; i++) {
        uint64_t age = ms_since(now_ms, s_query[i].t);
        /* A query counts once its reply is due, or earlier if the reply already came */
        if (age > LINK_WINDOW_MS || (age < REPLY_TIMEOUT_MS && !s_query[i].answered)) continue;
        qr_n++;
        if (s_query[i].answered) qr_ok++;
        if (age <= SILENT_MS) {
            recent_n++;
            if (s_query[i].answered) recent_ok++;
        }
    }
    pthread_mutex_unlock(&s_lock);

    q->status_pct = pct(st_ok, st_n);
    q->reply_pct = pct(qr_ok, qr_n);
    int low = 100;
    if (q->status_pct >= 0 && q->status_pct < low) low = q->status_pct;
    if (q->reply_pct >= 0 && q->reply_pct < low) low = q->reply_pct;
    q->bars = low >= 90 ? 4 : low >= 70 ? 3 : low >= 40 ? 2 : 1;

    if (!g_rf_link_connected) {
        q->state = LINK_NO_HUB;
    } else if ((q->status_pct < 0 || q->status_pct >= 90) && recent_n >= SILENT_MIN_QUERIES && recent_ok == 0) {
        q->state = LINK_HUB_SILENT;
    } else {
        q->state = LINK_OK;
    }
}
