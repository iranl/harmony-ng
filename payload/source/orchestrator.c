#include "orchestrator.h"
#include "hub_events.h"
#include "orchestrator_queue.h"
#include "hw_action.h"
#include "cJSON.h"
#include "config_store.h"
#include "atomic_file.h"
#include "resource_cache.h"
#include "bt_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#define PRIMARY_CURRENT_ACT_FILE "/data/harmony/current_activity"

static int activity_known(const char *id);

static orch_state_t g_state = ORCH_STATE_IDLE;
static char g_current_act[32] = ORCH_POWEROFF_ID;
static char g_target_act[32] = ORCH_POWEROFF_ID;
static pthread_mutex_t g_act_lock = PTHREAD_MUTEX_INITIALIZER;

/* The step queues (g_queue, g_bg_queue and their counters) are run by the main loop (orch_tick)
 * and replaced by requests from HTTP workers and shim clients. Without one lock over both, two
 * requests at the same moment could both pass the busy check and build their transitions into
 * the same queue, and a request could rewrite the queue orch_tick was running. Recursive: the
 * main loop starts a queued request from inside orch_tick. Always taken before g_act_lock. */
static pthread_mutex_t g_queue_lock;
static pthread_once_t g_queue_lock_once = PTHREAD_ONCE_INIT;
static void queue_lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_queue_lock, &a);
    pthread_mutexattr_destroy(&a);
}
static void queue_lock(void) {
    pthread_once(&g_queue_lock_once, queue_lock_init);
    pthread_mutex_lock(&g_queue_lock);
}
static void queue_unlock(void) {
    pthread_mutex_unlock(&g_queue_lock);
}

/* A start or power off asked while a transition runs: it starts when that one has completed.
 * One slot, the latest request wins (the user's last word), the running target is never queued. */
static char g_queued_act[32];

static orch_step_t g_queue[ORCH_MAX_STEPS];
static size_t g_queue_len = 0;
static size_t g_current_step = 0;
static uint64_t g_next_deadline_ms = 0;

static orch_bg_step_t g_bg_queue[ORCH_MAX_STEPS];
static size_t g_bg_len = 0;
static uint64_t g_t_switched_step1 = 0;
static uint64_t g_t_switched_step4 = 0;
/* A background step is claimed (marked executed) under this lock before it is sent: a new
 * transition, which may start on a request thread, takes the steps still waiting into its own
 * queue (orch_take_pending_tail), and no step may go out twice */
static pthread_mutex_t g_bg_lock = PTHREAD_MUTEX_INITIALIZER;

/* Steps of the running transition whose command could not be sent */
static int g_failed_steps = 0;

static orch_progress_cb g_progress_cb = NULL;
static void *g_cb_user_data = NULL;

static uint64_t get_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static char s_last_persisted_act[64] = "";

static void save_current_activity(const char *act_id) {
    if (!act_id || !act_id[0]) return;
    /* Mirror to /tmp in RAM for atomic zero-contention reads */
    FILE *ft = fopen("/tmp/current_activity", "w");
    if (ft) {
        fprintf(ft, "%s\n", act_id);
        fclose(ft);
    }
    if (strcmp(s_last_persisted_act, act_id) == 0) return;

    char line[72];
    snprintf(line, sizeof(line), "%s\n", act_id);
    if (atomic_file_write_str(PRIMARY_CURRENT_ACT_FILE, line) != 0) return; /* retried at the next save */
    strncpy(s_last_persisted_act, act_id, sizeof(s_last_persisted_act) - 1);
}

/* Mirror active activity to /tmp/current_activity in RAM for atomic zero-contention
 * reads. Updates when activity actually switches (STEP_ACT_SWITCHED) or reaches idle;
 * in-flight target during switch is tracked in /tmp/activity_target. */
/* These files are written whole (temporary file, then rename): hub_rf reads them at any time,
   and a read of a truncated file would give it no activity or no switch */
static void write_activity_mirror(const char *act_id) {
    if (!act_id || !act_id[0]) return;
    char line[40];
    snprintf(line, sizeof(line), "%s\n", act_id);
    (void)atomic_file_write_str("/tmp/current_activity", line);
}

static void write_activity_target(const char *act_id) {
    if (!act_id || !act_id[0]) return;
    char line[40];
    snprintf(line, sizeof(line), "%s\n", act_id);
    (void)atomic_file_write_str("/tmp/activity_target", line);
}

static void clear_activity_target(void) {
    unlink("/tmp/activity_target");
}

static void load_current_activity(void) {
    const char *path = PRIMARY_CURRENT_ACT_FILE;
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[32] = {0};
        if (fscanf(f, "%31s", buf) == 1 && buf[0]) {
            strncpy(g_current_act, buf, sizeof(g_current_act) - 1);
            g_current_act[sizeof(g_current_act) - 1] = '\0';
            strncpy(s_last_persisted_act, g_current_act, sizeof(s_last_persisted_act) - 1);
        }
        fclose(f);
    }
}

/* Activity state event (spec 3.2): one HUB_EV_ACTIVITY_STATE per change of state, activity,
 * queued request or sending flag, same shape as the WebSocket get_state reply and GET
 * /api/activity. The first call (from orch_init) always emits, so sinks have a baseline. Callers
 * may hold g_act_lock; the queue paths call set_state(g_state) under it to publish a queue or
 * sending change alone. */
static int g_last_emitted_state = -1;
static char g_last_emitted_act[32] = "";
static char g_last_emitted_queued[32] = "";
static int g_last_emitted_sending = -1;

static const char *orch_state_name(orch_state_t st) {
    switch (st) {
        case ORCH_STATE_STARTING: return "starting";
        case ORCH_STATE_RUNNING: return "running";
        case ORCH_STATE_STOPPING: return "stopping";
        default: return "idle";
    }
}

/* emit 0: the state changes without an event, for a transition whose steps are not queued yet
 * (its event follows once they are, so it carries the right sending flag) */
static void set_state_ex(orch_state_t st, int emit) {
    g_state = st;
    char line[16];
    snprintf(line, sizeof(line), "%s\n", orch_state_name(st));
    (void)atomic_file_write_str("/tmp/activity_state", line);
    if (!emit) return;
    int sending = (g_queue_len > 0 || g_bg_len > 0) ? 1 : 0;
    if ((int)st == g_last_emitted_state && strcmp(g_current_act, g_last_emitted_act) == 0 &&
        strcmp(g_queued_act, g_last_emitted_queued) == 0 && sending == g_last_emitted_sending) return;
    g_last_emitted_state = (int)st;
    g_last_emitted_sending = sending;
    strncpy(g_last_emitted_act, g_current_act, sizeof(g_last_emitted_act) - 1);
    g_last_emitted_act[sizeof(g_last_emitted_act) - 1] = '\0';
    strncpy(g_last_emitted_queued, g_queued_act, sizeof(g_last_emitted_queued) - 1);
    g_last_emitted_queued[sizeof(g_last_emitted_queued) - 1] = '\0';
    cJSON *p = cJSON_CreateObject();
    if (!p) return;
    cJSON_AddStringToObject(p, "activity", g_current_act[0] ? g_current_act : ORCH_POWEROFF_ID);
    cJSON_AddNumberToObject(p, "state", (int)st);
    cJSON_AddStringToObject(p, "stateName", orch_state_name(st));
    cJSON_AddBoolToObject(p, "busy", (st == ORCH_STATE_STARTING || st == ORCH_STATE_STOPPING) ? 1 : 0);
    cJSON_AddBoolToObject(p, "sending", sending);
    /* the request queued behind the running transition, null when none (spec 3.2, #105) */
    if (g_queued_act[0]) cJSON_AddStringToObject(p, "queued", g_queued_act);
    else cJSON_AddNullToObject(p, "queued");
    hub_events_emit(HUB_EV_ACTIVITY_STATE, p);
}

static void set_state(orch_state_t st) {
    set_state_ex(st, 1);
}

/* The activity file changed (a restore, an import, a reload): the activity the Hub reports as
 * running may be gone. Staying "running" in it until the next restart left a power off with
 * nothing to send. Event bus sink, main loop. */
static int g_recheck_current = 0; /* the file changed during a transition: check at its end */

static void orch_check_current_known(void) {
    char cur[32];
    pthread_mutex_lock(&g_act_lock);
    snprintf(cur, sizeof(cur), "%s", g_current_act);
    pthread_mutex_unlock(&g_act_lock);
    if (strcmp(cur, ORCH_POWEROFF_ID) == 0 || activity_known(cur)) return;
    pthread_mutex_lock(&g_act_lock);
    if (!orch_is_busy() && strcmp(g_current_act, cur) == 0) {
        printf("[!] Orch: activity %s is no longer configured, now PowerOff\n", cur);
        fflush(stdout);
        snprintf(g_current_act, sizeof(g_current_act), "%s", ORCH_POWEROFF_ID);
        save_current_activity(g_current_act); /* writes /tmp/current_activity too */
        set_state(ORCH_STATE_IDLE);
    }
    pthread_mutex_unlock(&g_act_lock);
}

static void orch_config_sink(const hub_event_t *ev, void *user) {
    (void)user;
    if (!ev || ev->type != HUB_EV_CONFIG_CHANGED) return;
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(ev->payload, "files");
    const cJSON *f = NULL;
    int activities = 0;
    cJSON_ArrayForEach(f, files) {
        if (cJSON_IsString(f) && strcmp(f->valuestring, "activities") == 0) activities = 1;
    }
    if (!activities) return;
    if (orch_is_busy() || orch_is_sending()) {
        g_recheck_current = 1;
        return;
    }
    orch_check_current_known();
}

int orch_init(void) {
    g_queue_len = 0;
    g_current_step = 0;
    g_next_deadline_ms = 0;
    g_bg_len = 0;
    memset(g_bg_queue, 0, sizeof(g_bg_queue));
    pthread_mutex_lock(&g_act_lock);
    load_current_activity();
    /* The saved activity may no longer exist (deleted, or a restored configuration without it):
     * staying "running" in it left a power off with nothing to send */
    if (!activity_known(g_current_act)) {
        printf("[!] Orch: saved activity %s is not configured, starting as PowerOff\n", g_current_act);
        snprintf(g_current_act, sizeof(g_current_act), "%s", ORCH_POWEROFF_ID);
    }
    write_activity_mirror(g_current_act);
    clear_activity_target();
    /* Emits the baseline activity_state event */
    set_state((strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING);
    pthread_mutex_unlock(&g_act_lock);
    if (hub_events_subscribe(orch_config_sink, NULL) != 0) {
        fprintf(stderr, "[-] Orch: could not subscribe to configuration changes\n");
    }
    return 0;
}

void orch_set_progress_callback(orch_progress_cb cb, void *user_data) {
    g_progress_cb = cb;
    g_cb_user_data = user_data;
}

int orch_is_busy(void) {
    return (g_state == ORCH_STATE_STARTING || g_state == ORCH_STATE_STOPPING);
}

int orch_get_queued_activity(char *buf, size_t len) {
    if (!buf || len == 0) return 0;
    pthread_mutex_lock(&g_act_lock);
    snprintf(buf, len, "%s", g_queued_act);
    pthread_mutex_unlock(&g_act_lock);
    return buf[0] != '\0';
}

/* True when id names a configured activity (PowerOff's "-1" included) */
static int activity_known(const char *id) {
    if (strcmp(id, ORCH_POWEROFF_ID) == 0) return 1;
    cfg_snap_t *snap = NULL;
    const cJSON *root = config_store_activities_acquire(&snap);
    const cJSON *list = root ? (cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "activities")) : NULL;
    const cJSON *a = NULL;
    int found = 0;
    cJSON_ArrayForEach(a, list) {
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(a, "id");
        char aid[32] = "";
        if (j && cJSON_IsString(j) && j->valuestring) snprintf(aid, sizeof(aid), "%s", j->valuestring);
        else if (j && cJSON_IsNumber(j)) snprintf(aid, sizeof(aid), "%ld", (long)j->valuedouble);
        if (strcmp(aid, id) == 0) {
            found = 1;
            break;
        }
    }
    config_store_release(&snap);
    return found;
}

/* g_current_act copied under g_act_lock, for code that reads it without holding the lock: the
 * main loop and request threads both write it (switch, cancel, check of the saved activity) */
static void current_act_copy(char *buf, size_t len) {
    pthread_mutex_lock(&g_act_lock);
    snprintf(buf, len, "%s", g_current_act);
    pthread_mutex_unlock(&g_act_lock);
}

/* After a transition completed: start the queued request, if any */
static void orch_start_queued(void) {
    char next[32];
    pthread_mutex_lock(&g_act_lock);
    if (!g_queued_act[0]) {
        pthread_mutex_unlock(&g_act_lock);
        return;
    }
    strncpy(next, g_queued_act, sizeof(next) - 1);
    next[sizeof(next) - 1] = '\0';
    g_queued_act[0] = '\0';
    char cur[sizeof(g_current_act)];
    snprintf(cur, sizeof(cur), "%s", g_current_act);
    pthread_mutex_unlock(&g_act_lock);
    if (strcmp(next, cur) == 0) {
        printf("[*] Orch: queued request %s is the activity reached, nothing to do\n", next);
        pthread_mutex_lock(&g_act_lock);
        set_state(g_state); /* publishes the emptied queue */
        pthread_mutex_unlock(&g_act_lock);
        return;
    }
    printf("[*] Orch: starting the queued request %s\n", next);
    fflush(stdout);
    if (orch_switch_activity(next, 0, NULL, 0) < 0) {
        /* deleted since it was queued, or its transition cannot be built: nothing published the
         * emptied queue yet */
        pthread_mutex_lock(&g_act_lock);
        set_state(g_state);
        pthread_mutex_unlock(&g_act_lock);
    }
}

int orch_is_sending(void) {
    return g_queue_len > 0 || g_bg_len > 0;
}

/* Returns a copy taken under the lock, one buffer per thread (as config_store.c does): a caller
 * on an HTTP worker reads it while the main loop may be switching the activity. The copy stays
 * valid until the same thread calls again. */
const char *orch_get_current_activity(void) {
    static __thread char s_copy[sizeof(g_current_act)];
    current_act_copy(s_copy, sizeof(s_copy));
    return s_copy;
}

orch_state_t orch_get_state(void) {
    pthread_mutex_lock(&g_act_lock);
    if (orch_is_busy()) {
        orch_state_t st = (strcmp(g_target_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_STOPPING : ORCH_STATE_STARTING;
        pthread_mutex_unlock(&g_act_lock);
        return st;
    }
    orch_state_t st = (strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING;
    pthread_mutex_unlock(&g_act_lock);
    return st;
}

static void cancel_locked(void);

void orch_cancel(void) {
    queue_lock();
    cancel_locked();
    queue_unlock();
}

static void cancel_locked(void) {
    hw_ir_cancel(IR_PORT_ALL);
    clear_activity_target();
    /* empty the queues first, so the state event below says nothing is being sent */
    g_queue_len = 0;
    g_current_step = 0;
    pthread_mutex_lock(&g_bg_lock);
    g_bg_len = 0;
    memset(g_bg_queue, 0, sizeof(g_bg_queue));
    pthread_mutex_unlock(&g_bg_lock);
    pthread_mutex_lock(&g_act_lock);
    g_queued_act[0] = '\0'; /* a cancel is the user's last word too */
    /* Only a power off in progress ends as PowerOff. g_target_act is "-1" by default and stays so
     * when the activity was restored from flash and never switched in this run: a clean stop of
     * the daemon (SIGTERM at a reboot) used to take that for a power off and saved "-1", so every
     * reboot forgot the running activity. */
    if (g_state == ORCH_STATE_STOPPING) {
        strncpy(g_current_act, ORCH_POWEROFF_ID, sizeof(g_current_act) - 1);
        save_current_activity(g_current_act);
        set_state(ORCH_STATE_IDLE);
    } else {
        set_state((strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING);
        write_activity_mirror(g_current_act); /* a cancelled start: back to the activity the Hub is in */
    }
    /* what the progress event reports, taken in the same hold of the lock */
    char cur[sizeof(g_current_act)];
    snprintf(cur, sizeof(cur), "%s", g_current_act);
    orch_state_t st = g_state;
    pthread_mutex_unlock(&g_act_lock);
    if (g_progress_cb) g_progress_cb(cur, st, 0, 0, "Sequence cancelled", 0, -1, g_cb_user_data);
}

int orch_enqueue_steps(const orch_step_t *steps, size_t count) {
    if (count > ORCH_MAX_STEPS) return -1;
    queue_lock();
    memcpy(g_queue, steps, count * sizeof(orch_step_t));
    g_queue_len = count;
    g_current_step = 0;
    g_next_deadline_ms = get_now_ms();
    queue_unlock();
    return 0;
}

static void orch_enqueue_bg_steps(const orch_bg_step_t *bg_steps, size_t count) {
    if (count > ORCH_MAX_STEPS) count = ORCH_MAX_STEPS;
    pthread_mutex_lock(&g_bg_lock);
    if (bg_steps && count > 0) {
        memcpy(g_bg_queue, bg_steps, count * sizeof(orch_bg_step_t));
        g_bg_len = count;
    } else {
        g_bg_len = 0;
    }
    g_t_switched_step1 = 0;
    g_t_switched_step4 = 0;
    pthread_mutex_unlock(&g_bg_lock);
}

/*
 * The background steps of the last transition that have not gone out yet (the commands after its
 * Switched markers: the input a start selects, the devices a stop tail switches off), as main
 * steps for the front of the next transition, in the order they were due and with the same gaps
 * (a leading delay when the first one is not due yet). A new request used to drop them. The
 * steps taken are marked sent, so the main loop does not send them as well. Returns the count.
 */
static size_t orch_take_pending_tail(orch_step_t *out, size_t max) {
    size_t n = 0;
    if (!out || max < 2) return 0;
    pthread_mutex_lock(&g_bg_lock);
    size_t idx[ORCH_MAX_STEPS];
    size_t cnt = 0;
    for (size_t b = 0; b < g_bg_len && cnt < ORCH_MAX_STEPS; b++) {
        if (!g_bg_queue[b].executed) idx[cnt++] = b;
    }
    /* by due time; a step whose marker was not reached yet (no time) keeps its place after them */
    for (size_t i = 1; i < cnt; i++) {
        size_t v = idx[i];
        uint64_t tv = g_bg_queue[v].target_time_ms ? g_bg_queue[v].target_time_ms : UINT64_MAX;
        size_t j = i;
        while (j > 0) {
            uint64_t tp = g_bg_queue[idx[j - 1]].target_time_ms ? g_bg_queue[idx[j - 1]].target_time_ms : UINT64_MAX;
            if (tp <= tv) break;
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = v;
    }
    uint64_t now = get_now_ms();
    if (cnt > 0 && g_bg_queue[idx[0]].target_time_ms > now) {
        orch_step_t *d = &out[n++];
        memset(d, 0, sizeof(*d));
        d->type = STEP_ACT_DELAY;
        d->delay_ms = (uint32_t)(g_bg_queue[idx[0]].target_time_ms - now);
        snprintf(d->description, sizeof(d->description), "Tail of the last switch: Delay %ums", d->delay_ms);
    }
    for (size_t i = 0; i < cnt && n < max; i++) {
        orch_bg_step_t *b = &g_bg_queue[idx[i]];
        orch_step_t *s = &out[n++];
        *s = b->step;
        /* the gap to the next one as it was planned; the step's own delay after the last */
        if (i + 1 < cnt && b->target_time_ms && g_bg_queue[idx[i + 1]].target_time_ms > b->target_time_ms) {
            s->delay_ms = (uint32_t)(g_bg_queue[idx[i + 1]].target_time_ms - b->target_time_ms);
        }
        snprintf(s->description, sizeof(s->description), "Tail of the last switch: %.100s", b->step.description);
        b->executed = 1;
    }
    for (size_t i = 0; i < cnt; i++) {
        if (!g_bg_queue[idx[i]].executed) {
            printf("[!] Orch: no room for the tail step '%s' of the last switch, dropped\n",
                   g_bg_queue[idx[i]].step.description);
            g_bg_queue[idx[i]].executed = 1;
        }
    }
    pthread_mutex_unlock(&g_bg_lock);
    if (n > 0) {
        printf("[*] Orch: %zu tail step(s) of the last switch go out before the new one\n", n);
        fflush(stdout);
    }
    return n;
}

/* Sends one step; returns 0, or the negative result of the send (the command did not go out) */
static int execute_step(const orch_step_t *step) {
    int rc = 0;
    switch (step->type) {
    case STEP_ACT_IR_PRONTO:
        if (step->pronto_hex[0]) {
            rc = hw_ir_send_pronto(step->pronto_hex, step->ir_ports, step->ir_repeats);
        }
        break;
    case STEP_ACT_DEVICE_CMD:
        if (step->device_id[0] && step->command_name[0]) {
            rc = hw_device_command_send(step->device_id, step->command_name);
            if (rc != 0 && (strncmp(step->command_name, "0000 ", 5) == 0 ||
                            step->command_name[0] == 'F' || step->command_name[0] == 'f')) {
                rc = hw_ir_send_harmony_keycode(step->command_name, IR_PORT_ALL, 3);
            }
        }
        break;
    case STEP_ACT_BTHID:
        if (step->command_name[0]) {
            if (strcasecmp(step->command_name, "CONNECT") == 0 ||
                strcasecmp(step->command_name, "RECONNECT") == 0 ||
                strcasecmp(step->command_name, "bt_connect") == 0) {
                /* hub_btstack's commands are connect_host / disconnect_host [<bdaddr>]; it has no
                   CONNECT or DISCONNECT and dropped them */
                char addr[32];
                if (step->device_id[0] && strncmp(step->device_id, "bt_", 3) != 0 &&
                    hw_device_bt_host(step->device_id, addr, sizeof(addr))) {
                    char cmd[64];
                    snprintf(cmd, sizeof(cmd), "connect_host %s", addr);
                    rc = hub_send_btstack_cmd(cmd);
                } else {
                    rc = hub_send_btstack_cmd("connect_host");
                }
            } else if (strcasecmp(step->command_name, "DISCONNECT") == 0) {
                /* With a device: close that device's host link (power off, see orchestrator_queue.c) */
                char addr[32];
                if (step->device_id[0] && strncmp(step->device_id, "bt_", 3) != 0 &&
                    hw_device_bt_host(step->device_id, addr, sizeof(addr))) {
                    rc = hw_device_bt_disconnect(step->device_id);
                } else {
                    rc = hub_send_btstack_cmd("disconnect_host");
                }
            } else {
                /* To the host of the step's device. Without it the key went to whichever host
                 * the last key had addressed. */
                char addr[32] = "";
                if (!step->device_id[0] || strncmp(step->device_id, "bt_", 3) == 0 ||
                    !hw_device_bt_host(step->device_id, addr, sizeof(addr))) {
                    addr[0] = '\0';
                }
                char bt_reply[256];
                rc = run_bt_saved_script("btkeyboard", addr, step->command_name, 50, bt_reply, sizeof(bt_reply));
            }
        }
        break;
    case STEP_ACT_DELAY:
    default:
        break;
    }
    if (rc == HW_ERR_QUEUE_FULL) {
        /* called from orch_tick under the queue lock, which every write of g_target_act holds */
        printf("[!] Orch: activity '%s' step '%s' lost: the HTTP/TCP send queue is full (device '%s', command '%s')\n",
               g_target_act, step->description, step->device_id, step->command_name);
        fflush(stdout);
    } else if (rc != 0) {
        printf("[!] Orch: step '%s' not sent (device '%s', command '%s', result %d)\n", step->description,
               step->device_id, step->command_name, rc);
        fflush(stdout);
        if (rc > 0) rc = -rc;
    }
    return rc;
}

static int orch_tick_locked(void);

int orch_tick(void) {
    queue_lock();
    int r = orch_tick_locked();
    queue_unlock();
    return r;
}

static int orch_tick_locked(void) {
    uint64_t now = get_now_ms();

    /* Process background queue: each due step is claimed under the lock, then sent */
    if (g_bg_len > 0) {
        int any_pending = 0;
        for (;;) {
            orch_step_t due;
            size_t pos = 0, len = 0;
            int found = 0;
            any_pending = 0;
            pthread_mutex_lock(&g_bg_lock);
            len = g_bg_len;
            for (size_t b = 0; b < g_bg_len; b++) {
                if (g_bg_queue[b].executed) continue;
                if (!found && g_bg_queue[b].target_time_ms > 0 && now >= g_bg_queue[b].target_time_ms) {
                    due = g_bg_queue[b].step;
                    g_bg_queue[b].executed = 1;
                    pos = b;
                    found = 1;
                } else {
                    any_pending = 1;
                }
            }
            pthread_mutex_unlock(&g_bg_lock);
            if (!found) break;
            printf("[*] Orch bg step %zu/%zu: %s (type=%d)\n", pos + 1, len, due.description, due.type);
            fflush(stdout);
            execute_step(&due);
        }
        if (!any_pending) {
            pthread_mutex_lock(&g_bg_lock);
            g_bg_len = 0;
            pthread_mutex_unlock(&g_bg_lock);
            if (g_queue_len == 0) {
                /* the last background step went out: publish sending false */
                pthread_mutex_lock(&g_act_lock);
                set_state(g_state);
                pthread_mutex_unlock(&g_act_lock);
                if (g_recheck_current && !orch_is_busy()) {
                    g_recheck_current = 0;
                    orch_check_current_known();
                }
            }
        }
    }

    if (g_queue_len == 0) {
        return (g_bg_len > 0) ? 1 : 0;
    }

    if (now < g_next_deadline_ms) {
        return 1;
    }

    if (g_current_step < g_queue_len) {
        const orch_step_t *step = &g_queue[g_current_step];
        if (step->type == STEP_ACT_STOP_SWITCHED) {
            pthread_mutex_lock(&g_bg_lock);
            g_t_switched_step1 = now;
            for (size_t b = 0; b < g_bg_len; b++) {
                if (g_bg_queue[b].anchor_step == 1) {
                    g_bg_queue[b].target_time_ms = g_t_switched_step1 + g_bg_queue[b].relative_offset_ms;
                }
            }
            pthread_mutex_unlock(&g_bg_lock);
            g_current_step++;
            g_next_deadline_ms = now;
            return 1;
        } else if (step->type == STEP_ACT_SWITCHED) {
            pthread_mutex_lock(&g_bg_lock);
            g_t_switched_step4 = now;
            for (size_t b = 0; b < g_bg_len; b++) {
                if (g_bg_queue[b].anchor_step == 4) {
                    g_bg_queue[b].target_time_ms = g_t_switched_step4 + g_bg_queue[b].relative_offset_ms;
                }
            }
            pthread_mutex_unlock(&g_bg_lock);
            pthread_mutex_lock(&g_act_lock);
            strncpy(g_current_act, g_target_act, sizeof(g_current_act) - 1);
            save_current_activity(g_current_act);
            clear_activity_target();
            set_state((strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING);
            pthread_mutex_unlock(&g_act_lock);

            if (g_progress_cb) {
                g_progress_cb(g_target_act, g_state, (int)g_current_step + 1,
                              (int)g_queue_len, "Activity switched", 0, -1, g_cb_user_data);
            }
            g_current_step++;
            g_next_deadline_ms = now;
            return 1;
        } else {
            printf("[*] Orch step %zu/%zu: %s (type=%d, delay=%ums)\n",
                   g_current_step + 1, g_queue_len, step->description, step->type, step->delay_ms);
            fflush(stdout);
        }
        int step_rc = execute_step(step);
        if (step_rc != 0) g_failed_steps++;

        /* The delay runs from the end of the step: a send blocks for the length of its signal,
         * and counting from before it shortened the gap to the next step (two equal commands
         * then fell under the dispatcher's same-button filter and the second was dropped). */
        uint32_t delay = step->delay_ms ? step->delay_ms : 50;
        g_next_deadline_ms = get_now_ms() + delay;

        if (g_progress_cb) {
            g_progress_cb(g_target_act, g_state, (int)g_current_step + 1,
                          (int)g_queue_len, step->description, step_rc, -1, g_cb_user_data);
        }
        g_current_step++;
        return 1;
    }

    /* the main queue is done: empty it before the state event, so the event says whether
     * background steps are still to be sent */
    g_queue_len = 0;
    g_current_step = 0;
    pthread_mutex_lock(&g_act_lock);
    if (g_state != ORCH_STATE_RUNNING && g_state != ORCH_STATE_IDLE) {
        strncpy(g_current_act, g_target_act, sizeof(g_current_act) - 1);
        save_current_activity(g_current_act);
        set_state((strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING);
    } else {
        set_state(g_state); /* after a Switched marker: publishes sending false when nothing is left */
    }
    char reached[sizeof(g_current_act)];
    snprintf(reached, sizeof(reached), "%s", g_current_act);
    orch_state_t reached_state = g_state;
    pthread_mutex_unlock(&g_act_lock);
    clear_activity_target();

    if (g_progress_cb) {
        g_progress_cb(reached, reached_state, 0, 0, "Transition completed", 0, g_failed_steps, g_cb_user_data);
    }
    if (g_failed_steps > 0) {
        printf("[!] Orch: %d step(s) of the switch to %s could not be sent\n", g_failed_steps, reached);
        fflush(stdout);
    }
    if (g_recheck_current && g_bg_len == 0) {
        g_recheck_current = 0;
        orch_check_current_known();
    }
    orch_start_queued();
    return (g_bg_len > 0 || g_queue_len > 0) ? 1 : 0;
}

static int orch_compile_transition(const char *target_id) {
    orch_step_t *main_steps = (orch_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_step_t));
    orch_bg_step_t *bg_steps = (orch_bg_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_bg_step_t));
    if (!main_steps || !bg_steps) {
        free(main_steps);
        free(bg_steps);
        return -1;
    }
    size_t main_count = 0, bg_count = 0;
    uint32_t sw_ms = 0, end_ms = 0;
    char cur[sizeof(g_current_act)];
    current_act_copy(cur, sizeof(cur));
    if (orch_build_steps(cur, target_id, main_steps, ORCH_MAX_STEPS, &main_count,
                         bg_steps, ORCH_MAX_STEPS, &bg_count, &sw_ms, &end_ms) != 0) {
        free(main_steps);
        free(bg_steps);
        return -1;
    }
    /* The tail of the last transition still waiting goes out first (see orch_take_pending_tail) */
    orch_step_t *all = (orch_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_step_t));
    if (!all) {
        free(main_steps);
        free(bg_steps);
        return -1;
    }
    size_t tail = orch_take_pending_tail(all, ORCH_MAX_STEPS - main_count);
    memcpy(all + tail, main_steps, main_count * sizeof(orch_step_t));
    g_failed_steps = 0;
    int rc = orch_enqueue_steps(all, tail + main_count);
    if (rc == 0) {
        orch_enqueue_bg_steps(bg_steps, bg_count);
    }
    free(all);
    free(main_steps);
    free(bg_steps);
    return rc;
}

static void orch_format_preview_json(const char *target_id,
                                     const orch_step_t *steps, size_t count,
                                     const orch_bg_step_t *bg_steps, size_t bg_count,
                                     uint32_t switched_at_ms, uint32_t true_end_ms,
                                     char *preview_json, size_t preview_len) {
    if (!preview_json || preview_len == 0) return;
    cJSON *pj = cJSON_CreateObject();
    cJSON_AddStringToObject(pj, "target", target_id ? target_id : ORCH_POWEROFF_ID);
    cJSON_AddNumberToObject(pj, "switchedDurationMs", switched_at_ms);
    cJSON_AddNumberToObject(pj, "trueEndMs", true_end_ms);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddStringToObject(st, "description", steps[i].description);
        cJSON_AddNumberToObject(st, "type", (int)steps[i].type);
        if (steps[i].type == STEP_ACT_SWITCHED || steps[i].type == STEP_ACT_STOP_SWITCHED) {
            cJSON_AddStringToObject(st, "stepType", "Switched");
        }
        if (steps[i].device_id[0]) cJSON_AddStringToObject(st, "deviceId", steps[i].device_id);
        if (steps[i].command_name[0]) cJSON_AddStringToObject(st, "command", steps[i].command_name);
        cJSON_AddNumberToObject(st, "delayMs", steps[i].delay_ms);
        cJSON_AddItemToArray(arr, st);
    }
    cJSON_AddItemToObject(pj, "steps", arr);

    cJSON *bg_arr = cJSON_CreateArray();
    for (size_t i = 0; i < bg_count; i++) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddStringToObject(st, "description", bg_steps[i].step.description);
        cJSON_AddNumberToObject(st, "type", (int)bg_steps[i].step.type);
        if (bg_steps[i].step.device_id[0]) cJSON_AddStringToObject(st, "deviceId", bg_steps[i].step.device_id);
        if (bg_steps[i].step.command_name[0]) cJSON_AddStringToObject(st, "command", bg_steps[i].step.command_name);
        cJSON_AddNumberToObject(st, "delayMs", bg_steps[i].step.delay_ms);
        cJSON_AddNumberToObject(st, "relativeOffsetMs", bg_steps[i].relative_offset_ms);
        cJSON_AddNumberToObject(st, "anchorStep", bg_steps[i].anchor_step);
        cJSON_AddItemToArray(bg_arr, st);
    }
    cJSON_AddItemToObject(pj, "backgroundSteps", bg_arr);

    char *pstr = cJSON_PrintUnformatted(pj);
    if (pstr) {
        snprintf(preview_json, preview_len, "%s", pstr);
        free(pstr);
    }
    cJSON_Delete(pj);
}

static int switch_activity_locked(const char *target_activity_id, int dry_run,
                                  char *preview_json, size_t preview_len);

int orch_switch_activity(const char *target_activity_id, int dry_run,
                         char *preview_json, size_t preview_len) {
    queue_lock();
    int rc = switch_activity_locked(target_activity_id, dry_run, preview_json, preview_len);
    queue_unlock();
    return rc;
}

static int switch_activity_locked(const char *target_activity_id, int dry_run,
                                  char *preview_json, size_t preview_len) {
    if (!target_activity_id || !target_activity_id[0]) return -1;
    /* an unknown id fails here, whether it would run now or be queued (a dry run keeps its
     * empty preview) */
    if (!dry_run && !activity_known(target_activity_id)) return -1;
    /* the queue is read and cleared by the main loop (orch_start_queued) and written from the
     * request threads: the busy check and the queue update are one step under the lock */
    pthread_mutex_lock(&g_act_lock);
    if (orch_is_busy()) {
        if (dry_run) {
            pthread_mutex_unlock(&g_act_lock);
            return -2;
        }
        if (strcmp(target_activity_id, g_target_act) == 0) {
            g_queued_act[0] = '\0'; /* the running transition already goes there */
            set_state(g_state);     /* publishes the emptied queue (no event when it was empty) */
            pthread_mutex_unlock(&g_act_lock);
            return 0;
        }
        int changed = (strcmp(g_queued_act, target_activity_id) != 0);
        if (changed) {
            strncpy(g_queued_act, target_activity_id, sizeof(g_queued_act) - 1);
            g_queued_act[sizeof(g_queued_act) - 1] = '\0';
            set_state(g_state); /* same state and activity: the event carries the new queued id */
        }
        char running[32];
        snprintf(running, sizeof(running), "%s", g_target_act);
        pthread_mutex_unlock(&g_act_lock);
        if (changed) {
            printf("[*] Orch: %s requested while switching to %s, queued\n", target_activity_id, running);
            fflush(stdout);
        }
        return 1;
    }
    /* The activity the Hub is already in, and no transition runs: nothing to do. Going on cleared
     * the background queue below, so a second press right after the Switched marker dropped the
     * tail of the sequence that had just run (the input selection after a start, the devices the
     * stop tail switches off), and a power off while off sent stop_reconnect again. */
    if (!dry_run && strcmp(target_activity_id, g_current_act) == 0) {
        /* The events stay those of an empty transition (starting or stopping, then running or
         * idle): the Logitech shim answers a runactivity on them. */
        int off = (strcmp(target_activity_id, ORCH_POWEROFF_ID) == 0);
        set_state(off ? ORCH_STATE_STOPPING : ORCH_STATE_STARTING);
        set_state(off ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING);
        pthread_mutex_unlock(&g_act_lock);
        printf("[*] Orch: %s is the current activity, nothing to do\n", target_activity_id);
        fflush(stdout);
        if (g_progress_cb) {
            g_progress_cb(target_activity_id, g_state, 0, 0, "Transition completed", 0, 0, g_cb_user_data);
        }
        return 0;
    }
    pthread_mutex_unlock(&g_act_lock);

    if (dry_run) {
        if (preview_json && preview_len > 0) {
            orch_step_t *main_steps = (orch_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_step_t));
            orch_bg_step_t *bg_steps = (orch_bg_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_bg_step_t));
            if (main_steps && bg_steps) {
                size_t main_count = 0, bg_count = 0;
                uint32_t sw_ms = 0, end_ms = 0;
                char cur[sizeof(g_current_act)];
                current_act_copy(cur, sizeof(cur));
                orch_build_steps(cur, target_activity_id, main_steps, ORCH_MAX_STEPS, &main_count,
                                 bg_steps, ORCH_MAX_STEPS, &bg_count, &sw_ms, &end_ms);
                orch_format_preview_json(target_activity_id, main_steps, main_count,
                                         bg_steps, bg_count, sw_ms, end_ms,
                                         preview_json, preview_len);
            }
            free(main_steps);
            free(bg_steps);
        }
        return 0;
    }

    strncpy(g_target_act, target_activity_id, sizeof(g_target_act) - 1);
    /* busy from here on; the event follows once the steps are queued */
    set_state_ex((strcmp(target_activity_id, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_STOPPING : ORCH_STATE_STARTING, 0);

    write_activity_target(target_activity_id);
    if (strcmp(target_activity_id, ORCH_POWEROFF_ID) == 0) {
        hub_send_btstack_cmd("stop_reconnect\n");
    }

    int rc = orch_compile_transition(target_activity_id);
    if (rc != 0) {
        clear_activity_target();
        pthread_mutex_lock(&g_act_lock);
        set_state((strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING);
        write_activity_mirror(g_current_act);
        pthread_mutex_unlock(&g_act_lock);
        return -1;
    }

    if (g_queue_len == 0) {
        pthread_mutex_lock(&g_act_lock);
        /* nothing to send: still publish starting (or stopping) before running (or idle), so a
         * client waiting for the end of the transition sees it, as for any other request */
        set_state(g_state);
        strncpy(g_current_act, g_target_act, sizeof(g_current_act) - 1);
        save_current_activity(g_current_act);
        clear_activity_target();
        set_state((strcmp(g_current_act, ORCH_POWEROFF_ID) == 0) ? ORCH_STATE_IDLE : ORCH_STATE_RUNNING);
        pthread_mutex_unlock(&g_act_lock);

        if (g_progress_cb) {
            g_progress_cb(g_target_act, g_state, 0, 0, "Transition completed", 0, 0, g_cb_user_data);
        }
        return 0;
    }

    pthread_mutex_lock(&g_act_lock);
    set_state(g_state); /* starting or stopping, with sending true */
    pthread_mutex_unlock(&g_act_lock);

    if (g_progress_cb) {
        g_progress_cb(g_target_act, g_state, 0, (int)g_queue_len, "Starting switch...", 0, -1, g_cb_user_data);
    }
    return 0;
}

int orch_power_off(int dry_run, char *preview_json, size_t preview_len) {
    return orch_switch_activity(ORCH_POWEROFF_ID, dry_run, preview_json, preview_len);
}

static int is_power_toggle(const orch_step_t *s) {
    return s->type == STEP_ACT_DEVICE_CMD &&
           (strcasecmp(s->command_name, "PowerToggle") == 0 || strcasecmp(s->command_name, "Power") == 0);
}

static void note_toggle(const orch_step_t *s, orch_toggle_t *toggles, size_t max, size_t *n) {
    for (size_t i = 0; i < *n; i++) {
        if (strcmp(toggles[i].device_id, s->device_id) == 0) return;
    }
    if (*n >= max) return;
    snprintf(toggles[*n].device_id, sizeof(toggles[*n].device_id), "%s", s->device_id);
    snprintf(toggles[*n].command, sizeof(toggles[*n].command), "%s", s->command_name);
    (*n)++;
}

static int repair_activity_locked(const char *act_id, int dry_run, orch_toggle_t *toggles, size_t max_toggles,
                                  size_t *toggle_count, char *preview_json, size_t preview_len);

int orch_repair_activity(const char *act_id, int dry_run, orch_toggle_t *toggles, size_t max_toggles,
                         size_t *toggle_count, char *preview_json, size_t preview_len) {
    queue_lock();
    int rc = repair_activity_locked(act_id, dry_run, toggles, max_toggles, toggle_count, preview_json, preview_len);
    queue_unlock();
    return rc;
}

static int repair_activity_locked(const char *act_id, int dry_run, orch_toggle_t *toggles, size_t max_toggles,
                                  size_t *toggle_count, char *preview_json, size_t preview_len) {
    *toggle_count = 0;
    if (!act_id || !act_id[0] || strcmp(act_id, ORCH_POWEROFF_ID) == 0) return -3;
    if (!dry_run) {
        if (orch_is_busy()) return -2;
        pthread_mutex_lock(&g_act_lock);
        int running = (strcmp(g_current_act, act_id) == 0);
        pthread_mutex_unlock(&g_act_lock);
        if (!running) return -3;
    }

    orch_step_t *main_steps = (orch_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_step_t));
    orch_bg_step_t *bg_steps = (orch_bg_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_bg_step_t));
    size_t main_count = 0, bg_count = 0;
    uint32_t sw_ms = 0, end_ms = 0;
    if (!main_steps || !bg_steps ||
        orch_build_steps(ORCH_POWEROFF_ID, act_id, main_steps, ORCH_MAX_STEPS, &main_count,
                         bg_steps, ORCH_MAX_STEPS, &bg_count, &sw_ms, &end_ms) != 0) {
        free(main_steps);
        free(bg_steps);
        return -1;
    }

    size_t m = 0, b = 0;
    for (size_t i = 0; i < main_count; i++) {
        if (is_power_toggle(&main_steps[i])) note_toggle(&main_steps[i], toggles, max_toggles, toggle_count);
        else main_steps[m++] = main_steps[i];
    }
    for (size_t i = 0; i < bg_count; i++) {
        if (is_power_toggle(&bg_steps[i].step)) note_toggle(&bg_steps[i].step, toggles, max_toggles, toggle_count);
        else bg_steps[b++] = bg_steps[i];
    }

    if (dry_run) {
        orch_format_preview_json(act_id, main_steps, m, bg_steps, b, sw_ms, end_ms, preview_json, preview_len);
    } else if (m > 0 || b > 0) {
        /* the tail of the last transition still waiting goes out first, as for a switch */
        orch_step_t *all = (orch_step_t *)calloc(ORCH_MAX_STEPS, sizeof(orch_step_t));
        size_t tail = all ? orch_take_pending_tail(all, ORCH_MAX_STEPS - m) : 0;
        if (all) memcpy(all + tail, main_steps, m * sizeof(orch_step_t));
        m += tail;
        strncpy(g_target_act, act_id, sizeof(g_target_act) - 1);
        g_failed_steps = 0;
        orch_enqueue_steps(all ? all : main_steps, m);
        orch_enqueue_bg_steps(bg_steps, b);
        free(all);
        /* The queue returns to RUNNING after its last main step; with none, stay RUNNING. Set
         * after the steps are queued, so the event carries sending true. */
        set_state(m > 0 ? ORCH_STATE_STARTING : g_state);
        if (g_progress_cb) {
            g_progress_cb(g_target_act, g_state, 0, (int)m, "Help: re-sending the activity's commands", 0, -1, g_cb_user_data);
        }
    }
    free(main_steps);
    free(bg_steps);
    return 0;
}
