/*
 * Event bus: a mutex-protected ring filled from any thread, drained on the main loop.
 * See hub_events.h.
 */
#include "hub_events.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>

static const char *const k_type_names[HUB_EV_COUNT] = {
    "activity_state",
    "activity_progress",
    "config_changed",
    "bt_status",
    "rf_status",
    "button",
};

typedef struct {
    hub_event_type_t type;
    time_t ts;
    cJSON *payload;
} queued_event_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static queued_event_t g_ring[HUB_EVENTS_RING];
static unsigned g_head;          /* next slot to pop */
static unsigned g_count;         /* queued entries */
static uint32_t g_overflows;     /* events dropped because the ring was full */
static uint32_t g_overflow_logged; /* value of g_overflows at the last log line */
static uint32_t g_seq;           /* last sequence number handed out */
static struct {
    hub_event_sink_fn fn;
    void *user;
} g_sinks[HUB_EVENTS_MAX_SINKS];
static int g_sink_count;
static cJSON *g_last[HUB_EV_COUNT]; /* last dispatched payload per type (main loop only) */

int hub_events_init(void) {
    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < g_count; i++) {
        cJSON_Delete(g_ring[(g_head + i) % HUB_EVENTS_RING].payload);
    }
    memset(g_ring, 0, sizeof(g_ring));
    g_head = 0;
    g_count = 0;
    g_overflows = 0;
    g_overflow_logged = 0;
    g_seq = 0;
    g_sink_count = 0;
    memset(g_sinks, 0, sizeof(g_sinks));
    pthread_mutex_unlock(&g_lock);
    for (int t = 0; t < HUB_EV_COUNT; t++) {
        cJSON_Delete(g_last[t]);
        g_last[t] = NULL;
    }
    return 0;
}

int hub_events_subscribe(hub_event_sink_fn fn, void *user) {
    if (!fn) return -1;
    pthread_mutex_lock(&g_lock);
    if (g_sink_count >= HUB_EVENTS_MAX_SINKS) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    g_sinks[g_sink_count].fn = fn;
    g_sinks[g_sink_count].user = user;
    g_sink_count++;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void hub_events_emit(hub_event_type_t type, cJSON *payload) {
    if ((int)type < 0 || type >= HUB_EV_COUNT) {
        cJSON_Delete(payload);
        return;
    }
    if (!payload || !cJSON_IsObject(payload)) {
        cJSON_Delete(payload);
        payload = cJSON_CreateObject();
        if (!payload) return;
    }
    time_t now = time(NULL);
    pthread_mutex_lock(&g_lock);
    if (g_count == HUB_EVENTS_RING) {
        /* Full: drop the oldest so the newest state always gets through */
        cJSON_Delete(g_ring[g_head].payload);
        g_ring[g_head].payload = NULL;
        g_head = (g_head + 1) % HUB_EVENTS_RING;
        g_count--;
        g_overflows++;
    }
    queued_event_t *slot = &g_ring[(g_head + g_count) % HUB_EVENTS_RING];
    slot->type = type;
    slot->ts = now;
    slot->payload = payload;
    g_count++;
    pthread_mutex_unlock(&g_lock);
}

void hub_events_dispatch(void) {
    for (;;) {
        queued_event_t ev;
        hub_event_sink_fn fns[HUB_EVENTS_MAX_SINKS];
        void *users[HUB_EVENTS_MAX_SINKS];
        int nsinks;
        uint32_t overflows;

        pthread_mutex_lock(&g_lock);
        overflows = g_overflows;
        if (g_count == 0) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        ev = g_ring[g_head];
        g_ring[g_head].payload = NULL;
        g_head = (g_head + 1) % HUB_EVENTS_RING;
        g_count--;
        uint32_t seq = ++g_seq;
        nsinks = g_sink_count;
        for (int i = 0; i < nsinks; i++) {
            fns[i] = g_sinks[i].fn;
            users[i] = g_sinks[i].user;
        }
        pthread_mutex_unlock(&g_lock);

        if (overflows != g_overflow_logged) {
            /* One line per burst: logged when the loop gets to drain again */
            fprintf(stderr, "[!] events: ring full, %u event(s) dropped so far\n",
                    (unsigned)overflows);
            g_overflow_logged = overflows;
        }

        /* Rebuild the payload with "type", "seq" and "ts" first, then the emitter's fields
         * (minus any of those three names) in their original order */
        cJSON *msg = cJSON_CreateObject();
        if (!msg) {
            cJSON_Delete(ev.payload);
            continue;
        }
        cJSON_AddStringToObject(msg, "type", k_type_names[ev.type]);
        cJSON_AddNumberToObject(msg, "seq", (double)seq);
        cJSON_AddNumberToObject(msg, "ts", (double)ev.ts);
        cJSON *child;
        while ((child = ev.payload->child) != NULL) {
            cJSON_DetachItemViaPointer(ev.payload, child);
            if (!child->string || strcmp(child->string, "type") == 0 ||
                strcmp(child->string, "seq") == 0 || strcmp(child->string, "ts") == 0) {
                cJSON_Delete(child);
                continue;
            }
            /* AddItemToObject copies the key before freeing the old one */
            cJSON_AddItemToObject(msg, child->string, child);
        }
        cJSON_Delete(ev.payload);

        hub_event_t out;
        out.type = ev.type;
        out.seq = seq;
        out.ts = ev.ts;
        out.payload = msg;

        for (int i = 0; i < nsinks; i++) {
            fns[i](&out, users[i]);
        }

        cJSON_Delete(g_last[ev.type]);
        g_last[ev.type] = out.payload;
    }
}

const char *hub_event_type_name(hub_event_type_t type) {
    if ((int)type < 0 || type >= HUB_EV_COUNT) return "unknown";
    return k_type_names[type];
}

uint32_t hub_events_last_seq(void) {
    pthread_mutex_lock(&g_lock);
    uint32_t s = g_seq;
    pthread_mutex_unlock(&g_lock);
    return s;
}

const cJSON *hub_events_last(hub_event_type_t type) {
    if ((int)type < 0 || type >= HUB_EV_COUNT) return NULL;
    return g_last[type];
}

uint32_t hub_events_overflows(void) {
    pthread_mutex_lock(&g_lock);
    uint32_t n = g_overflows;
    pthread_mutex_unlock(&g_lock);
    return n;
}
