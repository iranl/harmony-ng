#define _GNU_SOURCE
#include "hw_action.h"
#include "cmd_range.h"
#include "cmd_ir_alt.h"
#include "resource_cache.h"
#include "ir_encoder.h"
#include "ir_i2s.h"
#include "config_store.h"
#include "bt_common.h"
#include "orchestrator.h"
#include "hub_events.h"
#include "ble_adv_payload.h"
#include "plugin_mgr.h"
#include "net_device.h"
#include "net_action.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <poll.h>

/* The Hub's name for plugins: read on every plugin command, so kept here instead of reading
 * settings.json each time. A short copy under the lock. */
static pthread_mutex_t s_hub_name_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_hub_name[64] = HW_HUB_NAME_DEFAULT;

void hw_set_hub_name(const char *name) {
    /* "Harmony Hub" is the stored default when the user never named the Hub: the stock name */
    const char *n = (name && name[0] && strcmp(name, "Harmony Hub") != 0) ? name : HW_HUB_NAME_DEFAULT;
    pthread_mutex_lock(&s_hub_name_lock);
    snprintf(s_hub_name, sizeof(s_hub_name), "%s", n);
    pthread_mutex_unlock(&s_hub_name_lock);
}

void hw_get_hub_name(char *out, size_t len) {
    if (!out || !len) return;
    pthread_mutex_lock(&s_hub_name_lock);
    snprintf(out, len, "%s", s_hub_name);
    pthread_mutex_unlock(&s_hub_name_lock);
}

/*
 * Held IR key. A press hands the encoded code to one player thread and returns. The thread
 * sends the code's start sequence, then its repeat sequence again and again with the code's
 * own timing, as the device's own remote does while a key is down. It ends when the key is
 * released, when no hold (the remote's heartbeat while the key is down) came for
 * HW_IR_HOLD_DEADLINE_MS, or when another command needs the transmitter; then it sends the
 * finish sequence if the code has one. It always ends after a whole frame and never before the
 * start sequence and the press's minimum repeat parts are out (ir_press_repeat_parts(): the
 * device's pressMinRepeats, as on a stock Hub), so a quick tap still sends what a tap sends.
 * The command lock is not held while it plays, so the release gets in.
 *
 * A tap of a code with repeat parts is played by the same thread with the key already
 * released: the caller waits for it, and every other sender waits for it like for a held key,
 * so nothing gets between the frames of one press.
 *
 * No condition variables: the Hub's kernel has no futex system call, so pthread_cond_wait()
 * returns at once and a thread waiting on one spins at full CPU. The player sleeps in poll()
 * on a pipe instead, and a caller waiting for the player to finish checks every few ms.
 */
#define IR_HOLD_IDLE_POLL_US 2000
static struct {
    pthread_mutex_t lock;
    int wake_rd, wake_wr;   /* a byte: a key was handed over */
    int thread_up;
    int active;             /* a key is handed over and has not ended */
    int stop;               /* it ends after the frame on air */
    uint64_t deadline_ms;   /* it ends when no hold came by then */
    char key[200];          /* "<device id>/<command>" */
    uint8_t *blob;
    size_t blob_len;
    uint8_t ports;
    int min_parts;          /* repeat parts that go out before it may end */
    unsigned job;           /* counts the keys handed over */
    unsigned job_done;      /* the last key that ended */
    int job_rc;             /* its result: 0, or the failed send's */
} s_ir_hold = { PTHREAD_MUTEX_INITIALIZER, -1, -1, 0, 0, 0, 0, {0}, NULL, 0, 0, 0, 0, 0, 0 };

static uint64_t ir_hold_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* s_ir_hold.lock held on entry and on return; returns when a key is handed over */
static void ir_hold_wait_for_key(void) {
    while (!s_ir_hold.active) {
        pthread_mutex_unlock(&s_ir_hold.lock);
        struct pollfd p = { s_ir_hold.wake_rd, POLLIN, 0 };
        if (poll(&p, 1, 1000) > 0) {   /* the timeout only guards against a lost byte */
            char b[16];
            while (read(s_ir_hold.wake_rd, b, sizeof(b)) > 0) {}
        }
        pthread_mutex_lock(&s_ir_hold.lock);
    }
}

/* s_ir_hold.lock held on entry and on return; returns when no key plays (the one playing ends
 * after its frame on air) */
static void ir_hold_wait_idle(void) {
    while (s_ir_hold.active) {
        s_ir_hold.stop = 1;
        pthread_mutex_unlock(&s_ir_hold.lock);
        usleep(IR_HOLD_IDLE_POLL_US);
        pthread_mutex_lock(&s_ir_hold.lock);
    }
}

static void *ir_hold_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&s_ir_hold.lock);
    for (;;) {
        ir_hold_wait_for_key();
        uint8_t *blob = s_ir_hold.blob;
        size_t blob_len = s_ir_hold.blob_len;
        uint8_t ports = s_ir_hold.ports;
        int min_parts = s_ir_hold.min_parts;
        unsigned job = s_ir_hold.job;
        pthread_mutex_unlock(&s_ir_hold.lock);

        /* one send for the capture mode, from the first frame to the last */
        ir_i2s_tx_hold(1);
        int part = IR_BLOB_START, frames = 0, repeats = 0, rc, timed_out = 0;
        for (;;) {
            rc = ir_i2s_blast_blob_part(blob, blob_len, part, ports);
            if (rc != 0) break;
            frames++;
            if (part == IR_BLOB_REPEAT) repeats++;
            pthread_mutex_lock(&s_ir_hold.lock);
            int stop = s_ir_hold.stop;
            int late = ir_hold_now_ms() >= s_ir_hold.deadline_ms;
            pthread_mutex_unlock(&s_ir_hold.lock);
            /* a release or a lost hold ends it only once the press's minimum is out */
            if (repeats >= min_parts) {
                if (stop) break;
                if (late) {
                    timed_out = 1;
                    break;
                }
            }
            part = IR_BLOB_REPEAT;
        }
        if (rc == 0) ir_i2s_blast_blob_part(blob, blob_len, IR_BLOB_FINISH, ports); /* -ENOENT: the code has none */
        ir_i2s_tx_hold(0);

        pthread_mutex_lock(&s_ir_hold.lock);
        if (rc != 0) {
            printf("[!] IR held key %s: send failed (%d) after %d frame(s)\n", s_ir_hold.key, rc, frames);
        } else if (timed_out) {
            printf("[*] IR held key %s: no hold or release for %d ms, stopped after %d frame(s)\n",
                   s_ir_hold.key, HW_IR_HOLD_DEADLINE_MS, frames);
        }
        ir_free_encoded(blob);
        s_ir_hold.blob = NULL;
        s_ir_hold.job_rc = rc;
        s_ir_hold.job_done = job;
        s_ir_hold.active = 0;
    }
    return NULL;
}

/* Ends the held key, if any, and returns when its frame on air is out */
static void ir_hold_stop_wait(void) {
    pthread_mutex_lock(&s_ir_hold.lock);
    ir_hold_wait_idle();
    pthread_mutex_unlock(&s_ir_hold.lock);
}

/* Starts a held key; the player frees blob. A key still playing ends first (its frame on air
 * goes out whole). min_parts: repeat parts it sends even when released at once; released: a tap,
 * the key is released already. *job_out gets the key's number. -1 without a player thread:
 * blob stays the caller's. */
static int ir_hold_press(const char *key, uint8_t *blob, size_t blob_len, uint8_t ports, int min_parts,
                         int released, unsigned *job_out) {
    pthread_mutex_lock(&s_ir_hold.lock);
    if (!s_ir_hold.thread_up) {
        if (s_ir_hold.wake_rd < 0) {
            int fds[2];
            if (pipe(fds) != 0) {
                pthread_mutex_unlock(&s_ir_hold.lock);
                return -1;
            }
            for (int i = 0; i < 2; i++) {
                fcntl(fds[i], F_SETFD, FD_CLOEXEC);
                fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
            }
            s_ir_hold.wake_rd = fds[0];
            s_ir_hold.wake_wr = fds[1];
        }
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 128 * 1024);
        int err = pthread_create(&th, &attr, ir_hold_thread, NULL);
        pthread_attr_destroy(&attr);
        if (err != 0) {
            pthread_mutex_unlock(&s_ir_hold.lock);
            return -1;
        }
        s_ir_hold.thread_up = 1;
    }
    ir_hold_wait_idle();
    snprintf(s_ir_hold.key, sizeof(s_ir_hold.key), "%s", key);
    s_ir_hold.blob = blob;
    s_ir_hold.blob_len = blob_len;
    s_ir_hold.ports = ports;
    s_ir_hold.min_parts = min_parts;
    s_ir_hold.deadline_ms = ir_hold_now_ms() + HW_IR_HOLD_DEADLINE_MS;
    s_ir_hold.stop = released ? 1 : 0;
    s_ir_hold.job++;
    if (job_out) *job_out = s_ir_hold.job;
    s_ir_hold.active = 1;
    if (write(s_ir_hold.wake_wr, "k", 1) < 0) { /* full pipe: a byte is already waiting */ }
    pthread_mutex_unlock(&s_ir_hold.lock);
    return 0;
}

/* Returns when key number job has ended, with its result */
static int ir_hold_wait_job(unsigned job) {
    pthread_mutex_lock(&s_ir_hold.lock);
    while ((int)(s_ir_hold.job_done - job) < 0) {
        pthread_mutex_unlock(&s_ir_hold.lock);
        usleep(IR_HOLD_IDLE_POLL_US);
        pthread_mutex_lock(&s_ir_hold.lock);
    }
    int rc = s_ir_hold.job_rc;
    pthread_mutex_unlock(&s_ir_hold.lock);
    return rc;
}

/* The hold of the key that is playing: it goes on for another HW_IR_HOLD_DEADLINE_MS. A hold
 * for any other key, or for none, does nothing. */
static void ir_hold_heartbeat(const char *key) {
    pthread_mutex_lock(&s_ir_hold.lock);
    if (s_ir_hold.active && !s_ir_hold.stop && key && strcmp(s_ir_hold.key, key) == 0) {
        s_ir_hold.deadline_ms = ir_hold_now_ms() + HW_IR_HOLD_DEADLINE_MS;
    }
    pthread_mutex_unlock(&s_ir_hold.lock);
}

/* The release of key (NULL: of whatever plays): it ends after the frame on air. Does not wait. */
static void ir_hold_release(const char *key) {
    pthread_mutex_lock(&s_ir_hold.lock);
    if (s_ir_hold.active && (!key || strcmp(s_ir_hold.key, key) == 0)) s_ir_hold.stop = 1;
    pthread_mutex_unlock(&s_ir_hold.lock);
}



static hw_mqtt_button_handler_t g_mqtt_button_handler = NULL;

void hw_set_mqtt_button_handler(hw_mqtt_button_handler_t handler) {
    g_mqtt_button_handler = handler;
}

int g_dry_run = 0;

__thread int g_live_remote_key = 0;
__thread unsigned long long g_key_trace_rx_ms = 0;
__thread unsigned long long g_key_trace_daemon_ms = 0;

int hw_ir_send_pronto(const char *pronto_hex, uint8_t ports, uint8_t repeats) {
    if (!pronto_hex || !pronto_hex[0]) return -1;
    if (ports == 0) return 0; /* 0 = No blasters / IR disabled */

    uint8_t *bin = NULL;
    size_t bin_len = 0;
    int rc = ir_encode_pronto(pronto_hex, repeats ? repeats : 3, 500, &bin, &bin_len);
    if (rc != 0 || !bin || bin_len == 0) {
        return -2;
    }

    uint8_t port_mask = ports;
    if (g_dry_run) {
        printf("[DRY-RUN] IR Pronto simulated: %zu bytes, port_mask 0x%02x\n", bin_len, port_mask);
        ir_free_encoded(bin);
        return 0;
    }
    ir_hold_stop_wait(); /* a held key ends for this send */
    int ret = ir_i2s_blast_blob(bin, bin_len, port_mask);

    ir_free_encoded(bin);
    return ret;
}

#ifdef TEST_HARNESS
/* Last hw_ir_send_raw() call, for tests/test_ir_raw.py (GET /api/test/ir-raw-last) */
static struct {
    uint32_t freq_hz;
    size_t count;
    uint8_t ports;
    uint8_t repeats;
    uint64_t sum_us;
    uint32_t first[8];
    int calls;
} s_test_last_raw;

cJSON *hw_test_last_raw_json(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "calls", s_test_last_raw.calls);
    cJSON_AddNumberToObject(o, "frequency", s_test_last_raw.freq_hz);
    cJSON_AddNumberToObject(o, "count", (double)s_test_last_raw.count);
    cJSON_AddNumberToObject(o, "port", s_test_last_raw.ports);
    cJSON_AddNumberToObject(o, "repeats", s_test_last_raw.repeats);
    cJSON_AddNumberToObject(o, "sum_us", (double)s_test_last_raw.sum_us);
    cJSON *first = cJSON_AddArrayToObject(o, "first");
    for (size_t i = 0; i < 8 && i < s_test_last_raw.count; i++) {
        cJSON_AddItemToArray(first, cJSON_CreateNumber(s_test_last_raw.first[i]));
    }
    return o;
}
#endif

int hw_ir_send_raw(uint32_t freq_hz, const uint32_t *timings_us, size_t count, uint8_t ports, uint8_t repeats) {
    if (!timings_us || count == 0) return -1;
#ifdef TEST_HARNESS
    s_test_last_raw.calls++;
    s_test_last_raw.freq_hz = freq_hz;
    s_test_last_raw.count = count;
    s_test_last_raw.ports = ports;
    s_test_last_raw.repeats = repeats;
    s_test_last_raw.sum_us = 0;
    for (size_t i = 0; i < count; i++) {
        s_test_last_raw.sum_us += timings_us[i];
        if (i < 8) s_test_last_raw.first[i] = timings_us[i];
    }
#endif
    if (ports == 0) return 0; /* 0 = No blasters / IR disabled */

    uint8_t *bin = NULL;
    size_t bin_len = 0;
    int rc = ir_encode_raw(freq_hz, timings_us, count, repeats ? repeats : 3, &bin, &bin_len);
    if (rc != 0 || !bin || bin_len == 0) {
        return -2;
    }

    uint8_t port_mask = ports;
    if (g_dry_run) {
        printf("[DRY-RUN] IR Raw simulated: %zu timings (%u Hz), port_mask 0x%02x\n", count, freq_hz, port_mask);
        ir_free_encoded(bin);
        return 0;
    }
    ir_hold_stop_wait(); /* a held key ends for this send */
    int ret = ir_i2s_blast_blob(bin, bin_len, port_mask);

    ir_free_encoded(bin);
    return ret;
}

int hw_ir_cancel(uint8_t ports) {
    (void)ports;
    ir_hold_release(NULL);
    return 0;
}

/* The parts of one press without the player thread: the start, parts repeat parts, the finish */
static int ir_play_press(const uint8_t *blob, size_t blob_len, uint8_t ports, int parts) {
    ir_i2s_tx_hold(1);
    int rc = ir_i2s_blast_blob_part(blob, blob_len, IR_BLOB_START, ports);
    for (int i = 0; rc == 0 && i < parts; i++) rc = ir_i2s_blast_blob_part(blob, blob_len, IR_BLOB_REPEAT, ports);
    if (rc == 0) ir_i2s_blast_blob_part(blob, blob_len, IR_BLOB_FINISH, ports); /* -ENOENT: the code has none */
    ir_i2s_tx_hold(0);
    return rc;
}

/* The RC5 toggle bit, as a stock Hub keeps it (device.lua toggleByte): one per device and
 * protocol, 0 after a start. A press or tap sends the stored bit and then flips it, so a
 * receiver tells a new press of a key from the key held; a held key's repeats are that
 * press's frames and keep its bit. No device (a keycode sent on its own): 0, as stored. */
#define RC5_TOGGLE_MAX 32
static struct { char key[128]; int bit; } s_rc5_toggles[RC5_TOGGLE_MAX];
static pthread_mutex_t s_rc5_toggle_lock = PTHREAD_MUTEX_INITIALIZER;

static int rc5_toggle_take(const char *device_id, const char *keycode) {
    if (!device_id || !device_id[0] || !ir_keycode_is_rc5_toggle(keycode)) return 0;
    char key[sizeof(s_rc5_toggles[0].key)];
    snprintf(key, sizeof(key), "%s/%.*s", device_id, (int)strcspn(keycode + 2, ":"), keycode + 2);
    int bit = 0;
    pthread_mutex_lock(&s_rc5_toggle_lock);
    for (int i = 0; i < RC5_TOGGLE_MAX; i++) {
        if (!s_rc5_toggles[i].key[0]) snprintf(s_rc5_toggles[i].key, sizeof(s_rc5_toggles[i].key), "%s", key);
        if (strcmp(s_rc5_toggles[i].key, key) == 0) {
            bit = s_rc5_toggles[i].bit;
            s_rc5_toggles[i].bit = !bit;
            break;
        }
    }
    pthread_mutex_unlock(&s_rc5_toggle_lock);
    return bit; /* more than RC5_TOGGLE_MAX devices with RC5: the others send the code as stored */
}

/* press 0: one blocking send of the code's start sequence, as given (a fallback for a step that
 * names a raw or Pronto code). press 1: a press as a stock Hub sends it, the start part then
 * ir_press_repeat_parts() repeat parts for the device's press_min_repeats (negative: not set);
 * held_key NULL: a tap, it returns when the press is out; else the press of that key: the send
 * starts and goes on in the player until the release. device_id: the device the code belongs
 * to, for its RC5 toggle bit; NULL: the code goes out as stored. */
static int ir_send_keycode(const char *keycode, uint8_t ports, uint8_t repeats, int press, int press_min_repeats,
                           const char *held_key, const char *device_id) {
    if (!keycode || !keycode[0]) return -1;
    if (ports == 0) return 0; /* 0 = No blasters / IR disabled */

    uint8_t *bin = NULL;
    size_t bin_len = 0;
    int rc = ir_encode_harmony_keycode_toggle(keycode, repeats ? repeats : 3, rc5_toggle_take(device_id, keycode),
                                              &bin, &bin_len);
    if (rc != 0 || !bin || bin_len == 0) {
        return -2;
    }
    int parts = press ? ir_press_repeat_parts(keycode, bin, bin_len, press_min_repeats) : 0;

    uint8_t port_mask = ports;
    if (g_dry_run) {
        printf("[DRY-RUN] IR Keycode simulated: %s (%zu bytes), port_mask 0x%02x, %d repeat part(s) per press\n",
               keycode, bin_len, port_mask, parts);
        ir_free_encoded(bin);
        return 0;
    }
    unsigned job = 0;
    if (held_key && ir_hold_press(held_key, bin, bin_len, port_mask, parts, 0, NULL) == 0) return 0;
    if (parts > 0) {
        /* the player sends the whole press, so no other send gets between its frames */
        if (ir_hold_press(held_key ? held_key : keycode, bin, bin_len, port_mask, parts, 1, &job) == 0) {
            return ir_hold_wait_job(job);
        }
        ir_hold_stop_wait(); /* a held key ends for this send */
        int ret = ir_play_press(bin, bin_len, port_mask, parts);
        ir_free_encoded(bin);
        return ret;
    }
    ir_hold_stop_wait(); /* a held key ends for this send */
    int ret = ir_i2s_blast_blob(bin, bin_len, port_mask);

    ir_free_encoded(bin);
    return ret;
}

int hw_ir_send_harmony_keycode(const char *keycode, uint8_t ports, uint8_t repeats) {
    return ir_send_keycode(keycode, ports, repeats, 0, -1, NULL, NULL);
}

static int is_bt_device_connected(const char *target_bdaddr) {
    FILE *f = fopen("/tmp/bthid_status", "r");
    if (!f) return 0;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (!strstr(buf, "\"state\":\"listening\"")) return 0;
    if (target_bdaddr && target_bdaddr[0]) {
        return (strcasestr(buf, target_bdaddr) != NULL);
    }
    return 1;
}

/* A device's id as text ("" when it has none) */
static void device_id_text(const cJSON *dev, char *out, size_t len) {
    const cJSON *j_id = cJSON_GetObjectItemCaseSensitive(dev, "id");
    out[0] = '\0';
    if (j_id && cJSON_IsNumber(j_id)) snprintf(out, len, "%ld", (long)j_id->valuedouble);
    else if (j_id && cJSON_IsString(j_id) && j_id->valuestring) snprintf(out, len, "%s", j_id->valuestring);
}

/*
 * The device a command is for. match_mode 1 = id, 2 = name (any case), 0 = id or name. For 0
 * every id is tried before any name: in one pass the first device whose id OR name matched won,
 * so a device named after another device's id (a switch called "2") took that device's commands.
 * id_out (may be NULL) receives the device's id.
 */
static cJSON *find_device(const cJSON *devs, const char *target, int match_mode, char *id_out, size_t id_len) {
    if (!devs || !target || !target[0]) return NULL;
    cJSON *dev = NULL;
    char id_str[64];
    if (match_mode != 2) {
        cJSON_ArrayForEach(dev, devs) {
            device_id_text(dev, id_str, sizeof(id_str));
            if (id_str[0] && strcmp(id_str, target) == 0) {
                if (id_out) snprintf(id_out, id_len, "%s", id_str);
                return dev;
            }
        }
    }
    if (match_mode != 1) {
        cJSON_ArrayForEach(dev, devs) {
            const cJSON *j_name = cJSON_GetObjectItemCaseSensitive(dev, "name");
            if (j_name && cJSON_IsString(j_name) && j_name->valuestring && j_name->valuestring[0] &&
                strcasecmp(j_name->valuestring, target) == 0) {
                if (id_out) device_id_text(dev, id_out, id_len);
                return dev;
            }
        }
    }
    return NULL;
}

static void check_and_trigger_bt_reconnect_for_command(const cJSON *devs, const char *executed_dev_id, const char *executed_dev_name, const char *executed_cmd) {
    if (!devs || !executed_cmd || !executed_cmd[0]) return;

    /* Do not trigger BT reconnect while stopping or in PowerOff */
    if (orch_get_state() == ORCH_STATE_STOPPING || strcmp(orch_get_current_activity(), "-1") == 0) {
        return;
    }

    /* Do not trigger BT reconnect for power-down or standby commands */
    if (strcasecmp(executed_cmd, "PowerOff") == 0 ||
        strcasecmp(executed_cmd, "Sleep") == 0 ||
        strcasecmp(executed_cmd, "PowerToggle") == 0 ||
        strcasecmp(executed_cmd, "Power") == 0 ||
        strcasecmp(executed_cmd, "Standby") == 0) {
        return;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, devs) {
        const cJSON *dev = item;

        int is_bt = 0;
        cJSON *j_type = cJSON_GetObjectItemCaseSensitive(dev, "type");
        if (j_type && cJSON_IsString(j_type) && j_type->valuestring) {
            if (strcasecmp(j_type->valuestring, "BT") == 0 ||
                strcasecmp(j_type->valuestring, "Bluetooth") == 0 ||
                strcasecmp(j_type->valuestring, "BTHID") == 0 ||
                strcasestr(j_type->valuestring, "bluetooth") != NULL) {
                is_bt = 1;
            }
        }

        cJSON *j_bta = cJSON_GetObjectItemCaseSensitive(dev, "bdaddr");
        if (!j_bta) j_bta = cJSON_GetObjectItemCaseSensitive(dev, "btHost");
        if (j_bta && cJSON_IsString(j_bta) && j_bta->valuestring && hub_valid_bdaddr(j_bta->valuestring)) {
            is_bt = 1;
        }

        if (cJSON_GetObjectItemCaseSensitive(dev, "btAutoReconnect") ||
            cJSON_GetObjectItemCaseSensitive(dev, "btTriggerCommandName")) {
            is_bt = 1;
        }

        if (!is_bt) continue;

        cJSON *j_rec = cJSON_GetObjectItemCaseSensitive(dev, "btAutoReconnect");
        if (j_rec) {
            int rec_val = cJSON_IsNumber(j_rec) ? (int)j_rec->valuedouble : (cJSON_IsBool(j_rec) ? (cJSON_IsTrue(j_rec) ? 1 : 0) : (j_rec->valuestring ? atoi(j_rec->valuestring) : 1));
            if (rec_val == 0) continue;
        }

        char cur_id[64] = {0};
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(dev, "id");
        if (jid) {
            if (cJSON_IsNumber(jid)) snprintf(cur_id, sizeof(cur_id), "%ld", (long)jid->valuedouble);
            else if (cJSON_IsString(jid) && jid->valuestring) strncpy(cur_id, jid->valuestring, sizeof(cur_id) - 1);
        }

        char cur_name[64] = {0};
        cJSON *jnm = cJSON_GetObjectItemCaseSensitive(dev, "name");
        if (jnm && cJSON_IsString(jnm) && jnm->valuestring) {
            strncpy(cur_name, jnm->valuestring, sizeof(cur_name) - 1);
        }

        int target_dev_delay = -1;
        cJSON *jd_dly = cJSON_GetObjectItemCaseSensitive(dev, "btReconnectDelaySec");
        if (jd_dly) {
            if (cJSON_IsNumber(jd_dly) && jd_dly->valuedouble >= 0) target_dev_delay = (int)jd_dly->valuedouble;
            else if (cJSON_IsString(jd_dly) && jd_dly->valuestring && jd_dly->valuestring[0]) target_dev_delay = atoi(jd_dly->valuestring);
        }

        int dur = 0;
        cJSON *jd_dur = cJSON_GetObjectItemCaseSensitive(dev, "btReconnectDurationSec");
        if (jd_dur) {
            if (cJSON_IsNumber(jd_dur) && jd_dur->valuedouble > 0) dur = (int)jd_dur->valuedouble;
            else if (cJSON_IsString(jd_dur) && jd_dur->valuestring) dur = atoi(jd_dur->valuestring);
        }
        if (dur <= 0) dur = 60;

        char trig_did_buf[64] = {0};
        cJSON *j_trig_dev = cJSON_GetObjectItemCaseSensitive(dev, "btTriggerDeviceId");
        if (j_trig_dev) {
            if (cJSON_IsString(j_trig_dev) && j_trig_dev->valuestring) {
                strncpy(trig_did_buf, j_trig_dev->valuestring, sizeof(trig_did_buf) - 1);
            } else if (cJSON_IsNumber(j_trig_dev)) {
                snprintf(trig_did_buf, sizeof(trig_did_buf), "%ld", (long)j_trig_dev->valuedouble);
            }
        }
        const char *trig_did = trig_did_buf;

        char trig_cmd_buf[64] = {0};
        cJSON *j_trig_cmd = cJSON_GetObjectItemCaseSensitive(dev, "btTriggerCommandName");
        if (j_trig_cmd) {
            if (cJSON_IsString(j_trig_cmd) && j_trig_cmd->valuestring) {
                strncpy(trig_cmd_buf, j_trig_cmd->valuestring, sizeof(trig_cmd_buf) - 1);
            } else if (cJSON_IsNumber(j_trig_cmd)) {
                snprintf(trig_cmd_buf, sizeof(trig_cmd_buf), "%ld", (long)j_trig_cmd->valuedouble);
            }
        }
        const char *trig_cmd = trig_cmd_buf;

        /* If colon is in trig_cmd (e.g. "deviceId:PowerOn" or "MQTT:PowerOn"), separate them */
        const char *colon = strchr(trig_cmd, ':');
        if (colon) {
            if (!trig_did[0]) {
                size_t did_len = (size_t)(colon - trig_cmd);
                if (did_len >= sizeof(trig_did_buf)) did_len = sizeof(trig_did_buf) - 1;
                strncpy(trig_did_buf, trig_cmd, did_len);
                trig_did_buf[did_len] = '\0';
                trig_did = trig_did_buf;
            }
            trig_cmd = colon + 1;
        }

        int should_reconnect = 0;
        int dly = 0;

        if (trig_cmd[0]) {
            if (strcasecmp(executed_cmd, trig_cmd) == 0) {
                int match_dev = 1;
                if (trig_did && trig_did[0]) {
                    match_dev = 0;
                    if (executed_dev_id && (strcmp(executed_dev_id, trig_did) == 0 || strcasecmp(executed_dev_id, trig_did) == 0)) match_dev = 1;
                    if (!match_dev && executed_dev_name && (strcmp(executed_dev_name, trig_did) == 0 || strcasecmp(executed_dev_name, trig_did) == 0)) match_dev = 1;
                }
                if (match_dev) {
                    should_reconnect = 1;
                    dly = (target_dev_delay >= 0) ? target_dev_delay : 2;
                }
            }
        }

        /* Fallback: any button press sent directly to this BT device */
        if (!should_reconnect) {
            int is_target_dev = 0;
            if (executed_dev_id && cur_id[0] && (strcmp(executed_dev_id, cur_id) == 0 || strcasecmp(executed_dev_id, cur_id) == 0)) is_target_dev = 1;
            if (!is_target_dev && executed_dev_name && cur_name[0] && (strcmp(executed_dev_name, cur_name) == 0 || strcasecmp(executed_dev_name, cur_name) == 0)) is_target_dev = 1;
            if (is_target_dev) {
                should_reconnect = 1;
                dly = 0; /* Button press: 0 delay for immediate reconnect */
            }
        }

        if (!should_reconnect) continue;

        char target_bdaddr[32] = {0};
        if (j_bta && cJSON_IsString(j_bta) && j_bta->valuestring && hub_valid_bdaddr(j_bta->valuestring)) {
            strncpy(target_bdaddr, j_bta->valuestring, sizeof(target_bdaddr) - 1);
        }

        /* config/devices.json is the only device-to-host mapping: without an address there,
         * bt_reconnect.sh and hub_btstack use the default host (bthid_host_target) */

        printf("[*] BT reconnect trigger: device='%s' (id=%s, target=%s) delay=%ds dur=%ds (triggered by cmd='%s' on dev='%s' id='%s')\n",
               cur_name[0] ? cur_name : "BT-Device", cur_id,
               target_bdaddr[0] ? target_bdaddr : "auto",
               dly, dur, executed_cmd,
               executed_dev_name ? executed_dev_name : "",
               executed_dev_id ? executed_dev_id : "");

        if (dly > 0 && g_dry_run) {
            /* dry run: the delayed reconnect script is not started (the immediate one below goes
             * through hub_send_btstack_cmd(), which prints it) */
            printf("[DRY-RUN] BT reconnect simulated: %s in %ds\n", target_bdaddr[0] ? target_bdaddr : "default host", dly);
        } else if (dly > 0) {
            char sh_cmd[256];
            snprintf(sh_cmd, sizeof(sh_cmd),
                     "/data/harmony/bt_reconnect.sh %s %d %d >/dev/null 2>&1 &",
                     target_bdaddr[0] ? target_bdaddr : "\"\"", dur, dly);
            system(sh_cmd);
        } else {
            if (target_bdaddr[0] && is_bt_device_connected(target_bdaddr)) {
                printf("[*] BT device '%s' (%s) already connected, skipping immediate reconnect\n",
                       cur_name[0] ? cur_name : "BT-Device", target_bdaddr);
            } else {
                char cmd[128];
                if (target_bdaddr[0]) snprintf(cmd, sizeof(cmd), "connect_host %s %d\n", target_bdaddr, dur);
                else snprintf(cmd, sizeof(cmd), "connect_host %d\n", dur);
                hub_send_btstack_cmd(cmd);
            }
        }
    }
}

/* One command as infrared: the code of an IR device's command, or the infrared alternative of a
 * plugin device's command (cmd_ir_alt.h). A press starts the code and returns; the player
 * repeats it until the release. A hold is only the heartbeat of the key that plays and sends
 * nothing itself: each one used to be another blocking send, so a held key reached the device as
 * separate bursts. default_repeats: when the command has no "repeats" (1..20). */
static int send_ir_command(const cJSON *target_dev, const cJSON *target_cmd, const char *keycode, uint8_t ir_ports,
                           const char *dev_id, const char *command, int action, uint8_t default_repeats) {
    char held_key[200];
    snprintf(held_key, sizeof(held_key), "%s/%s", dev_id, command);
    if (action == HW_BTN_ACTION_RELEASE) {
        ir_hold_release(held_key);
        return 0;
    }
    if (action == HW_BTN_ACTION_REPEAT) {
        ir_hold_heartbeat(held_key);
        return 0;
    }
    uint8_t repeats = default_repeats;
    cJSON *j_rep = cJSON_GetObjectItemCaseSensitive(target_cmd, "repeats");
    if (j_rep && cJSON_IsNumber(j_rep) && j_rep->valuedouble > 0 && j_rep->valuedouble <= 20) {
        repeats = (uint8_t)j_rep->valuedouble;
    }
    /* The command's "repeats" is kept in the code's header only: how many frames a press
     * sends comes from the device's pressMinRepeats, as on a stock Hub */
    int press_min_repeats = -1; /* not set: IR_PRESS_MIN_REPEATS_DEFAULT */
    cJSON *j_pmr = cJSON_GetObjectItemCaseSensitive(target_dev, "pressMinRepeats");
    if (cJSON_IsNumber(j_pmr) && j_pmr->valuedouble >= 0 && j_pmr->valuedouble <= IR_PRESS_MIN_REPEATS_MAX) {
        press_min_repeats = (int)j_pmr->valuedouble;
    }
    /* a command's own port (Emitter Port of the saved command) before the device's */
    uint8_t cmd_ports = ir_ports;
    cJSON *j_cport = cJSON_GetObjectItemCaseSensitive(target_cmd, "port");
    if (cJSON_IsNumber(j_cport) && j_cport->valuedouble >= 1 && j_cport->valuedouble <= 7) {
        cmd_ports = (uint8_t)j_cport->valuedouble;
    }
    return ir_send_keycode(keycode, cmd_ports, repeats, 1, press_min_repeats,
                           action == HW_BTN_ACTION_PRESS ? held_key : NULL, dev_id);
}

static int send_match_in(const cJSON *root, const char *target, int match_mode, const char *command, int action, int value) {
    int ret = -1;

    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs || !cJSON_IsArray(devs)) {
        return -5;
    }

    char target_id_str[64] = {0};
    cJSON *target_dev = find_device(devs, target, match_mode, target_id_str, sizeof(target_id_str));
    if (!target_dev) {
        return -5;
    }

    int is_bt = 0;
    char bdaddr[32] = {0};

    cJSON *j_type = cJSON_GetObjectItemCaseSensitive(target_dev, "type");
    if (j_type && cJSON_IsString(j_type) && (strcasecmp(j_type->valuestring, "BT") == 0 || strcasestr(j_type->valuestring, "bluetooth"))) {
        is_bt = 1;
    }

    char dev_name[64] = {0};
    cJSON *j_dname = cJSON_GetObjectItemCaseSensitive(target_dev, "name");
    if (j_dname && cJSON_IsString(j_dname) && j_dname->valuestring) {
        strncpy(dev_name, j_dname->valuestring, sizeof(dev_name) - 1);
    }

    cJSON *j_bta = cJSON_GetObjectItemCaseSensitive(target_dev, "bdaddr");
    if (!j_bta) j_bta = cJSON_GetObjectItemCaseSensitive(target_dev, "btHost");
    if (j_bta && cJSON_IsString(j_bta) && j_bta->valuestring) {
        strncpy(bdaddr, j_bta->valuestring, sizeof(bdaddr) - 1);
    }

    /* config/devices.json is the only device-to-host mapping; without an address there the
     * device goes to the default host, the one hub_btstack pinned at the last pairing */
    if (!bdaddr[0]) {
        FILE *tf = fopen("/data/harmony/bthid_host_target", "r");
        if (tf) {
            char line[128];
            while (fgets(line, sizeof(line), tf)) {
                if (strncmp(line, "bdaddr=", 7) == 0) {
                    sscanf(line + 7, "%31s", bdaddr);
                }
            }
            fclose(tf);
        }
    }

    int is_mqtt = 0;
    cJSON *j_men2 = cJSON_GetObjectItemCaseSensitive(target_dev, "mqttEnabled");
    if (j_men2) {
        if (cJSON_IsTrue(j_men2)) is_mqtt = 1;
        else if (cJSON_IsNumber(j_men2) && j_men2->valueint > 0) is_mqtt = 1;
        else if (cJSON_IsString(j_men2) && (strcmp(j_men2->valuestring, "1") == 0 || strcasecmp(j_men2->valuestring, "true") == 0)) is_mqtt = 1;
    }
    cJSON *j_dtype = cJSON_GetObjectItemCaseSensitive(target_dev, "type");
    if (j_dtype && cJSON_IsString(j_dtype) && strcasestr(j_dtype->valuestring, "mqtt")) is_mqtt = 1;

    cJSON *j_dmfg = cJSON_GetObjectItemCaseSensitive(target_dev, "manufacturer");
    if (j_dmfg && cJSON_IsString(j_dmfg) && strcasestr(j_dmfg->valuestring, "mqtt")) is_mqtt = 1;

    cJSON *j_mtop = cJSON_GetObjectItemCaseSensitive(target_dev, "mqttTopic");
    if (j_mtop && cJSON_IsString(j_mtop) && j_mtop->valuestring && j_mtop->valuestring[0]) is_mqtt = 1;

    /* Only the device's "plugin" field or "plugin:<id>" type: no manifest is read in the key
     * path, and an IR device whose type happens to match a plugin name stays IR */
    char plugin_id[64] = {0};
    int is_plugin = plugin_mgr_device_plugin_id(target_dev, plugin_id, sizeof(plugin_id));
    const int dev_is_plugin = is_plugin; /* the device itself, not a PLUGIN: keyCode below */
    /* HTTP and TCP devices by their type only (net_device.h): a name with "mqtt" in it does
     * not make one an MQTT device */
    int net_kind = is_plugin ? NET_KIND_NONE : net_device_kind(target_dev);
    if (net_kind) is_mqtt = 0;

    /* Find Command */
    cJSON *cmds = cJSON_GetObjectItemCaseSensitive(target_dev, "commands");
    cJSON *target_cmd = NULL;

    const char *alt = NULL;
    if (strcasecmp(command, "OK") == 0) alt = "Select";
    else if (strcasecmp(command, "Select") == 0) alt = "OK";
    else if (strcasecmp(command, "Enter") == 0) alt = "Select";
    else if (strcasecmp(command, "Back") == 0) alt = "Return";
    else if (strcasecmp(command, "Return") == 0) alt = "Back";
    else if (strcasecmp(command, "Stop") == 0) alt = "Pause";
    else if (strcasecmp(command, "ChannelUp") == 0) alt = "PageUp";
    else if (strcasecmp(command, "ChannelDown") == 0) alt = "PageDown";

    if (cmds && cJSON_IsArray(cmds)) {
        cJSON *c_elem = NULL;
        cJSON_ArrayForEach(c_elem, cmds) {
            cJSON *c_name = cJSON_GetObjectItemCaseSensitive(c_elem, "name");
            if (c_name && cJSON_IsString(c_name) && c_name->valuestring) {
                if (strcmp(c_name->valuestring, command) == 0 ||
                    strcasecmp(c_name->valuestring, command) == 0) {
                    target_cmd = c_elem;
                    break;
                }
                if (!target_cmd && alt && (strcmp(c_name->valuestring, alt) == 0 ||
                                          strcasecmp(c_name->valuestring, alt) == 0)) {
                    target_cmd = c_elem;
                }
            }
        }
    }

    if (!target_cmd && !is_mqtt && !is_plugin) {
        return -6;
    }

    check_and_trigger_bt_reconnect_for_command(devs, target_id_str[0] ? target_id_str : target, dev_name, command);

    /* The code as saved (the device list stays held until the send is done): a copy of 4095
     * characters cut longer raw and Pronto codes and the cut code was sent as if complete */
    const char *keycode_str = "";
    if (target_cmd) {
        cJSON *j_kc = cJSON_GetObjectItemCaseSensitive(target_cmd, "keyCode");
        if (j_kc && cJSON_IsString(j_kc) && j_kc->valuestring && j_kc->valuestring[0]) {
            keycode_str = j_kc->valuestring;
        } else {
            cJSON *j_raw = cJSON_GetObjectItemCaseSensitive(target_cmd, "raw");
            if (j_raw && cJSON_IsString(j_raw) && j_raw->valuestring && j_raw->valuestring[0]) {
                keycode_str = j_raw->valuestring;
            }
        }
    }

    /* Points into the saved keyCode (held until the send is done): a copy cut long app links */
    const char *plug_cmd = command;
    if (strncmp(keycode_str, "PLUGIN:", 7) == 0 || strncmp(keycode_str, "plugin:", 7) == 0) {
        is_plugin = 1;
        const char *p = keycode_str + 7;
        const char *colon = strchr(p, ':');
        if (colon) {
            size_t plen = (size_t)(colon - p);
            if (plen < sizeof(plugin_id)) {
                memcpy(plugin_id, p, plen);
                plugin_id[plen] = '\0';
            }
            if (colon[1]) plug_cmd = colon + 1;
        } else if (!plugin_id[0]) {
            strncpy(plugin_id, p, sizeof(plugin_id) - 1);
        }
    }

    if (strncmp(keycode_str, "G:MQTT", 6) == 0) is_mqtt = 1;

    /* The infrared alternative of a plugin device's command: while the device's config field
     * named by the command's irWhen is on, the command goes out as infrared with irKeyCode, the
     * way an IR device's command does (a TV that ignores some keys over its network API). Only
     * the device as held in memory is read: no file in the key path. */
    const char *ir_alt_code = (dev_is_plugin && target_cmd) ? cmd_ir_alt_active(target_dev, target_cmd) : NULL;
    if (ir_alt_code) is_plugin = 0;

    /* A plugin command with "range": [min, max] takes only a value in that range, and needs one */
    if (is_plugin && target_cmd && cmd_range_valid(cJSON_GetObjectItemCaseSensitive(target_cmd, "range"))) {
        if (value == HW_VALUE_NONE) {
            printf("[!] Command '%s' of device '%s' refused: it needs a value (it has a range)\n",
                   command, dev_name[0] ? dev_name : target);
            return HW_ERR_VALUE_MISSING;
        }
        if (!hw_command_value_in_range(target_cmd, value)) {
            printf("[!] Command '%s' of device '%s' refused: value %d outside its range\n",
                   command, dev_name[0] ? dev_name : target, value);
            return HW_ERR_VALUE_RANGE;
        }
    }
    if (value == HW_VALUE_NONE) value = 0;

    uint8_t ir_ports = IR_PORT_ALL;
    cJSON *j_cp = cJSON_GetObjectItemCaseSensitive(target_dev, "controlPort");
    if (j_cp && cJSON_IsNumber(j_cp)) {
        int cp_val = (int)j_cp->valuedouble;
        if (cp_val >= 0 && cp_val <= 7) {
            ir_ports = (uint8_t)cp_val;
        }
    }

    if (strncmp(keycode_str, "HID:", 4) == 0 || strncmp(keycode_str, "hid:", 4) == 0 ||
        strncasecmp(keycode_str, "G:HID", 5) == 0 ||
        strcasecmp(command, "Connect") == 0 || strcasecmp(command, "Disconnect") == 0 ||
        strcasecmp(command, "BT_CONNECT") == 0 || strcasecmp(command, "BT_DISCONNECT") == 0) {
        is_bt = 1;
    }

    int inter_device_delay = 500;
    int inter_key_delay = 100;
    int hold_inter_key_delay = 200;

    cJSON *j_idd = cJSON_GetObjectItemCaseSensitive(target_dev, "interDeviceDelay");
    if (j_idd && cJSON_IsNumber(j_idd) && j_idd->valueint >= 0) inter_device_delay = j_idd->valueint;

    cJSON *j_ikd = cJSON_GetObjectItemCaseSensitive(target_dev, "interKeyDelay");
    if (j_ikd && cJSON_IsNumber(j_ikd) && j_ikd->valueint >= 0) inter_key_delay = j_ikd->valueint;

    cJSON *j_hikd = cJSON_GetObjectItemCaseSensitive(target_dev, "holdInterKeyDelay");
    if (j_hikd && cJSON_IsNumber(j_hikd) && j_hikd->valueint >= 0) hold_inter_key_delay = j_hikd->valueint;

    static pthread_mutex_t s_hw_cmd_lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t s_last_cmd_time_ms = 0;
    static char s_last_device_id[64] = {0};
    static char s_last_cmd_name[64] = {0};
    /* the last accepted press or tap, for the duplicate filter below */
    static uint64_t s_last_press_ms = 0;
    static char s_last_press_dev[64] = {0};
    static char s_last_press_cmd[64] = {0};

    struct timespec ts;
    /* when the request came in, before it waits for a send in progress */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t arrival_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    pthread_mutex_lock(&s_hw_cmd_lock);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    const char *actual_dev_id = target_id_str[0] ? target_id_str : target;

    /* 90ms debounce rejection: drop rapid duplicate press/tap pulses on the same button. The
     * window runs from the arrival of the previous accepted press or tap: it used to run from
     * the end of the last send or release, which dropped a real quick second press. */
    if (action == HW_BTN_ACTION_TAP || action == HW_BTN_ACTION_PRESS) {
        if (s_last_press_ms > 0 &&
            strcmp(s_last_press_dev, actual_dev_id) == 0 &&
            strcmp(s_last_press_cmd, command) == 0) {
            uint64_t elapsed = (arrival_ms > s_last_press_ms) ? (arrival_ms - s_last_press_ms) : 0;
            if (elapsed < 90) {
                pthread_mutex_unlock(&s_hw_cmd_lock);
                return 0;
            }
        }
        s_last_press_ms = arrival_ms;
        snprintf(s_last_press_dev, sizeof(s_last_press_dev), "%s", actual_dev_id);
        snprintf(s_last_press_cmd, sizeof(s_last_press_cmd), "%s", command);
    }

    /* The gaps between two commands are for sequences (activity steps, command lists, API
     * calls): a device may need time between two commands sent back to back by a program. A key
     * the user just pressed on a remote goes out at once: the user sets its pace. */
    if (!g_live_remote_key && action != HW_BTN_ACTION_RELEASE && action != HW_BTN_ACTION_REPEAT && s_last_cmd_time_ms > 0) {
        uint64_t elapsed = (now_ms > s_last_cmd_time_ms) ? (now_ms - s_last_cmd_time_ms) : 0;
        int req_delay = 0;
        if (strcmp(s_last_device_id, actual_dev_id) != 0) {
            req_delay = inter_device_delay;
        } else {
            if (strcmp(s_last_cmd_name, command) == 0) {
                req_delay = hold_inter_key_delay;
            } else {
                req_delay = inter_key_delay;
            }
        }
        if (req_delay > 0 && elapsed < (uint64_t)req_delay) {
            uint64_t wait_ms = (uint64_t)req_delay - elapsed;
            usleep((useconds_t)(wait_ms * 1000));
        }
    }

    int is_ble_adv = (strncmp(keycode_str, BLE_ADV_KEYCODE_PREFIX, BLE_ADV_KEYCODE_PREFIX_LEN) == 0);

    if (ir_alt_code) {
        /* repeats as a command of an IR device gets when it has none (config_sanitize.c) */
        ret = send_ir_command(target_dev, target_cmd, ir_alt_code, ir_ports, actual_dev_id, command, action, 3);
    } else if (is_plugin && plugin_id[0]) {
        cJSON *j_cfg = cJSON_GetObjectItemCaseSensitive(target_dev, "config");
        ret = hw_plugin_command_send(plugin_id, actual_dev_id, dev_name, plug_cmd, action, value, j_cfg ? j_cfg : target_dev);
    } else if (net_kind) {
        /* One request per press, like a BLE broadcast: the remote sends a hold every 300 ms, so
         * a repeat would send a toggle (Power) twice. Queued for the net_action worker, so a slow
         * device never holds this lock. */
        ret = (action == HW_BTN_ACTION_RELEASE || action == HW_BTN_ACTION_REPEAT) ? 0 :
              net_action_send(target_dev, target_cmd, actual_dev_id, dev_name, command, g_live_remote_key);
        if (ret == -3) ret = HW_ERR_QUEUE_FULL; /* -3 is also an infrared sender's code */
    } else if (is_ble_adv) {
        /* One broadcast per press: the receiver acts on the packet, not on how long it is held */
        if (action == HW_BTN_ACTION_RELEASE || action == HW_BTN_ACTION_REPEAT) {
            ret = 0;
        } else {
            const char *hex = keycode_str + BLE_ADV_KEYCODE_PREFIX_LEN;
            uint8_t adv[BLE_ADV_MAX_LEN];
            int adv_len = ble_adv_parse_hex(hex, adv, NULL);
            if (adv_len < 0) {
                printf("[!] BLE broadcast '%s' skipped: Device '%s' (%s) has an invalid payload\n",
                       command, dev_name[0] ? dev_name : "BLE-Device", actual_dev_id);
                ret = -2;
            } else {
                char adv_cmd[8 + 2 * BLE_ADV_MAX_LEN];
                snprintf(adv_cmd, sizeof(adv_cmd), "adv %.*s\n", 2 * adv_len, hex);
                ret = hub_send_btstack_cmd(adv_cmd);
            }
        }
    } else if (is_bt) {
        if (!bdaddr[0]) {
            pthread_mutex_unlock(&s_hw_cmd_lock);
            printf("[!] BT command '%s' skipped: Device '%s' (%s) has no host paired\n",
                   command, dev_name[0] ? dev_name : "BT-Device", actual_dev_id);
            return 0; /* Skip gracefully without failing activity switch */
        }

        char pipe_cmd[128] = {0};
        unsigned int page = 0, usage = 0;

        if (strcasecmp(command, "Connect") == 0 || strcasecmp(command, "BT_CONNECT") == 0 ||
            strcasecmp(command, "Disconnect") == 0 || strcasecmp(command, "BT_DISCONNECT") == 0) {
            /* A command to hub_btstack, not a key: on the key pipe "CONNECT" / "DISCONNECT" were
               taken for key names and sent as consumer usages 0x0C / 0x0D */
            int is_connect = strcasecmp(command, "Connect") == 0 || strcasecmp(command, "BT_CONNECT") == 0;
            if (action == HW_BTN_ACTION_RELEASE || action == HW_BTN_ACTION_REPEAT) {
                ret = 0; /* once per tap or press: a held key's repeats and its release send nothing */
            } else {
                char bt_cmd[64];
                snprintf(bt_cmd, sizeof(bt_cmd), "%s %s\n", is_connect ? "connect_host" : "disconnect_host", bdaddr);
                ret = hub_send_btstack_cmd(bt_cmd) == 0 ? 0 : -1;
            }
        } else {
            int hid_parsed = 0;
            if (sscanf(keycode_str, "HID:%x:%x", &page, &usage) == 2 ||
                sscanf(keycode_str, "hid:%x:%x", &page, &usage) == 2) {
                hid_parsed = 1;
            } else if (strncasecmp(keycode_str, "G:HID", 5) == 0) {
                const char *open_p = strchr(keycode_str, '(');
                while (open_p) {
                    const char *close_p = strchr(open_p, ')');
                    if (close_p && close_p > open_p + 1) {
                        char hex_buf[32] = {0};
                        size_t hex_len = close_p - (open_p + 1);
                        if (hex_len < sizeof(hex_buf)) {
                            memcpy(hex_buf, open_p + 1, hex_len);
                            unsigned long raw_val = 0;
                            if (sscanf(hex_buf, "%lx", &raw_val) == 1 && hex_len >= 6) {
                                page = (unsigned int)((raw_val >> 24) & 0xFF ? ((raw_val >> 24) & 0xFF) : ((raw_val >> 16) & 0xFF));
                                usage = (unsigned int)(raw_val & 0xFFFF);
                                hid_parsed = 1;
                                break;
                            }
                        }
                    }
                    open_p = close_p ? strchr(close_p + 1, '(') : NULL;
                }
            }
            /* hub_btstack's descriptor has a keyboard (0x07) and a consumer (0x0C)
             * page only; send anything else by name instead of dropping it. */
            if (hid_parsed && !((page == 0x07 && usage > 0 && usage <= 0xFF) ||
                                (page == 0x0C && usage > 0 && usage <= 0x3FF))) {
                hid_parsed = 0;
            }
            if (hid_parsed) {
                if (page == 0x07) {
                    /* KBD_DOWN keeps the key pressed until RELEASE; a raw report
                     * is released by hub_btstack after 30 ms, so a held key never
                     * repeated on the host. */
                    if (action == HW_BTN_ACTION_PRESS) {
                        snprintf(pipe_cmd, sizeof(pipe_cmd), "KBD_DOWN %X\n", usage);
                    } else if (action == HW_BTN_ACTION_RELEASE) {
                        snprintf(pipe_cmd, sizeof(pipe_cmd), "RELEASE\n");
                    } else if (action == HW_BTN_ACTION_REPEAT) {
                        pipe_cmd[0] = '\0';
                        ret = 0;
                    } else {
                        snprintf(pipe_cmd, sizeof(pipe_cmd), "KBD %X\n", usage);
                    }
                } else if (page == 0x0C) {
                    if (action == HW_BTN_ACTION_PRESS) {
                        snprintf(pipe_cmd, sizeof(pipe_cmd), "CONSUMER_DOWN %X\n", usage);
                    } else if (action == HW_BTN_ACTION_RELEASE) {
                        snprintf(pipe_cmd, sizeof(pipe_cmd), "CONSUMER_UP\n");
                    } else if (action == HW_BTN_ACTION_REPEAT) {
                        pipe_cmd[0] = '\0';
                        ret = 0;
                    } else {
                        snprintf(pipe_cmd, sizeof(pipe_cmd), "CONSUMER %X\n", usage);
                    }
                }
            } else {
                if (action == HW_BTN_ACTION_PRESS) {
                    snprintf(pipe_cmd, sizeof(pipe_cmd), "KEYDOWN %s\n", command);
                } else if (action == HW_BTN_ACTION_RELEASE) {
                    snprintf(pipe_cmd, sizeof(pipe_cmd), "KEYUP %s\n", command);
                } else if (action == HW_BTN_ACTION_REPEAT) {
                    pipe_cmd[0] = '\0';
                    ret = 0;
                } else {
                    snprintf(pipe_cmd, sizeof(pipe_cmd), "KEY %s\n", command);
                }
            }
        }

        if (pipe_cmd[0]) {
            int bfd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
            if (bfd >= 0) {
                /* Target and command in one write: whole or not at all, and never with another
                 * writer's command between the two. They were two unchecked writes: a full pipe
                 * could take the target and lose the command, and the key counted as sent.
                 * With debug logging on, a key from a remote also carries its times (key trace). */
                char msg[320];
                int mlen;
                if (g_key_trace_rx_ms && is_debug_log_enabled()) {
                    struct timespec tts;
                    clock_gettime(CLOCK_MONOTONIC, &tts);
                    unsigned long long write_ms = (unsigned long long)tts.tv_sec * 1000ULL + (unsigned long long)tts.tv_nsec / 1000000ULL;
                    mlen = snprintf(msg, sizeof(msg), "TARGET %s\nTS %llu %llu %llu\n%s", bdaddr,
                                    g_key_trace_rx_ms, g_key_trace_daemon_ms, write_ms, pipe_cmd);
                } else {
                    mlen = snprintf(msg, sizeof(msg), "TARGET %s\n%s", bdaddr, pipe_cmd);
                }
                ssize_t w = (mlen > 0 && mlen < (int)sizeof(msg)) ? write(bfd, msg, (size_t)mlen) : -1;
                int werr = errno;
                close(bfd);
                ret = (w == mlen) ? 0 : -1;
                if (ret != 0) {
                    printf("[!] Bluetooth key not handed to hub_btstack (%s): %.*s\n", w < 0 ? strerror(werr) : "short write",
                           (int)strcspn(pipe_cmd, "\n"), pipe_cmd);
                }
            } else {
                ret = -1;
            }

            if (is_debug_log_enabled()) {
                struct stat st;
                if (stat("/tmp/bt_sent.log", &st) == 0 && st.st_size > 65536) {
                    unlink("/tmp/bt_sent.log.1");
                    rename("/tmp/bt_sent.log", "/tmp/bt_sent.log.1");
                }
                FILE *bf = fopen("/tmp/bt_sent.log", "a");
                if (bf) {
                    time_t now = time(NULL);
                    char tbuf[32];
                    ctime_r(&now, tbuf);
                    char *nl = strchr(tbuf, '\n');
                    if (nl) *nl = '\0';
                    fprintf(bf, "[%s] Dev: %s (%s) | Cmd: %s | Action: %d | Page: 0x%02X Usage: 0x%04X | KeyCode: %s | ret=%d | Pipe: %s",
                            tbuf, dev_name[0] ? dev_name : "BT-Device", actual_dev_id, command, action, page, usage, keycode_str, ret, pipe_cmd);
                    fclose(bf);
                }
            }
        }
    } else if (is_mqtt || strncmp(keycode_str, "G:MQTT", 6) == 0) {
        const char *actual_dev_id_str = target_id_str[0] ? target_id_str : target;
        /* MQTT-type devices: the pulse on the device's own topic (handler registered by
         * hub_mqtt), plus the button event on the daemon's bus */
        if (g_mqtt_button_handler && action != HW_BTN_ACTION_RELEASE) {
            g_mqtt_button_handler(actual_dev_id_str, dev_name, command);
        }
        /* The event as POST /api/button-press emits it for this request (press and release only:
         * a held key's repeats are not events). It used to be an HTTP request to the daemon's own
         * port: called from the main loop (WebSocket and MQTT commands, activity steps) nobody
         * accepted it, so the main loop waited 2 s for every command and reported it as failed
         * when no broker was connected. */
        if (action != HW_BTN_ACTION_REPEAT) {
            cJSON *ev = cJSON_CreateObject();
            if (ev) {
                cJSON_AddStringToObject(ev, "button", command);
                cJSON_AddStringToObject(ev, "action", action == HW_BTN_ACTION_RELEASE ? "release" : "press");
                cJSON_AddStringToObject(ev, "source", "api");
                cJSON_AddStringToObject(ev, "deviceId", actual_dev_id_str);
                cJSON_AddStringToObject(ev, "command", command);
                hub_events_emit(HUB_EV_BUTTON, ev);
            }
        }
        ret = 0;
    } else if (keycode_str[0]) {
        ret = send_ir_command(target_dev, target_cmd, keycode_str, ir_ports, actual_dev_id, command, action, 1);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    s_last_cmd_time_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    strncpy(s_last_device_id, target_id_str[0] ? target_id_str : target, sizeof(s_last_device_id) - 1);
    s_last_device_id[sizeof(s_last_device_id) - 1] = '\0';
    strncpy(s_last_cmd_name, command, sizeof(s_last_cmd_name) - 1);
    s_last_cmd_name[sizeof(s_last_cmd_name) - 1] = '\0';
    pthread_mutex_unlock(&s_hw_cmd_lock);

    return ret;
}

int hw_command_value_in_range(const cJSON *cmd, int value) {
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(cmd, "range");
    if (!cmd_range_valid(r)) return 1;
    return value >= cJSON_GetArrayItem(r, 0)->valueint && value <= cJSON_GetArrayItem(r, 1)->valueint;
}

int hw_device_command_needs_value(const char *target, int match_mode, const char *command) {
    if (!target || !command) return 0;
    struct cfg_snap *snap = NULL;
    const cJSON *root = get_device_list_acquire(&snap);
    if (!root) return 0;
    int needs = 0;
    const cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    const cJSON *dev = cJSON_IsArray(devs) ? find_device(devs, target, match_mode, NULL, 0) : NULL;
    const cJSON *c = NULL;
    cJSON_ArrayForEach(c, (dev ? cJSON_GetObjectItemCaseSensitive(dev, "commands") : NULL)) {
        const cJSON *cn = cJSON_GetObjectItemCaseSensitive(c, "name");
        if (!cJSON_IsString(cn) || strcasecmp(cn->valuestring, command) != 0) continue;
        const cJSON *kc = cJSON_GetObjectItemCaseSensitive(c, "keyCode");
        if (!cJSON_IsString(kc)) kc = cJSON_GetObjectItemCaseSensitive(c, "raw");
        int plugin = plugin_mgr_device_plugin_id(dev, NULL, 0) ||
                     (cJSON_IsString(kc) && strncasecmp(kc->valuestring, "PLUGIN:", 7) == 0);
        /* A command sent as infrared ignores the value: no value needed while its switch is on */
        needs = plugin && !cmd_ir_alt_active(dev, c) &&
                cmd_range_valid(cJSON_GetObjectItemCaseSensitive(c, "range"));
        break;
    }
    release_device_list(&snap);
    return needs;
}

static int hw_device_command_send_match(const char *target, int match_mode, const char *command, int action, int value) {
    if (!target || !command) return -1;

    /* The shared device list (no copy per key): a save meanwhile publishes a new one and leaves
     * this one alone until it is released */
    struct cfg_snap *snap = NULL;
    const cJSON *root = get_device_list_acquire(&snap);
    if (!root) return -4;
    int ret = send_match_in(root, target, match_mode, command, action, value);
    release_device_list(&snap);
    return ret;
}

int hw_device_command_send_action(const char *target, int match_mode, const char *command, int action) {
    return hw_device_command_send_match(target, match_mode, command, action, HW_VALUE_NONE);
}

int hw_device_command_send_value(const char *target, int match_mode, const char *command, int action, int value) {
    return hw_device_command_send_match(target, match_mode, command, action, value);
}

int hw_device_command_send(const char *device_id, const char *command) {
    return hw_device_command_send_match(device_id, 0, command, HW_BTN_ACTION_TAP, HW_VALUE_NONE);
}

int hw_device_command_send_by_id(const char *device_id, const char *command) {
    return hw_device_command_send_match(device_id, 1, command, HW_BTN_ACTION_TAP, HW_VALUE_NONE);
}

int hw_device_command_send_by_name(const char *device_name, const char *command) {
    return hw_device_command_send_match(device_name, 2, command, HW_BTN_ACTION_TAP, HW_VALUE_NONE);
}

static int device_exists_in(const cJSON *root, const char *target, int match_mode) {
    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs || !cJSON_IsArray(devs)) return 0;
    return find_device(devs, target, match_mode, NULL, 0) != NULL;
}

int hw_device_exists(const char *target, int match_mode) {
    if (!target || !target[0]) return 0;
    struct cfg_snap *snap = NULL;
    const cJSON *root = get_device_list_acquire(&snap);
    if (!root) return 0;
    int found = device_exists_in(root, target, match_mode);
    release_device_list(&snap);
    return found;
}

static int device_command_exists_in(const cJSON *root, const char *target, int match_mode, const char *command) {
    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs || !cJSON_IsArray(devs)) return 0;
    const cJSON *target_dev = find_device(devs, target, match_mode, NULL, 0);
    if (!target_dev) return 0;

    int is_mqtt = 0;
    cJSON *j_men2 = cJSON_GetObjectItemCaseSensitive(target_dev, "mqttEnabled");
    if (j_men2) {
        if (cJSON_IsTrue(j_men2)) is_mqtt = 1;
        else if (cJSON_IsNumber(j_men2) && j_men2->valueint > 0) is_mqtt = 1;
        else if (cJSON_IsString(j_men2) && (strcmp(j_men2->valuestring, "1") == 0 || strcasecmp(j_men2->valuestring, "true") == 0)) is_mqtt = 1;
    }
    cJSON *j_dtype = cJSON_GetObjectItemCaseSensitive(target_dev, "type");
    if (j_dtype && cJSON_IsString(j_dtype) && strcasestr(j_dtype->valuestring, "mqtt")) is_mqtt = 1;
    cJSON *j_dmfg = cJSON_GetObjectItemCaseSensitive(target_dev, "manufacturer");
    if (j_dmfg && cJSON_IsString(j_dmfg) && strcasestr(j_dmfg->valuestring, "mqtt")) is_mqtt = 1;
    cJSON *j_mtop = cJSON_GetObjectItemCaseSensitive(target_dev, "mqttTopic");
    if (j_mtop && cJSON_IsString(j_mtop) && j_mtop->valuestring && j_mtop->valuestring[0]) is_mqtt = 1;
    if (is_mqtt && !net_device_kind(target_dev)) return 1;

    /* Connect and Disconnect are found in the list like any command, as the sender looks them
     * up (the sanitizer adds both to every Bluetooth device). Accepted for any device, a job
     * with Connect for an infrared device was acknowledged and that step sent nothing. */

    cJSON *cmds = cJSON_GetObjectItemCaseSensitive(target_dev, "commands");
    if (cmds && cJSON_IsArray(cmds)) {
        const char *alt = NULL;
        if (strcasecmp(command, "OK") == 0) alt = "Select";
        else if (strcasecmp(command, "Select") == 0) alt = "OK";
        else if (strcasecmp(command, "Enter") == 0) alt = "Select";
        else if (strcasecmp(command, "Back") == 0) alt = "Return";
        else if (strcasecmp(command, "Return") == 0) alt = "Back";
        else if (strcasecmp(command, "Stop") == 0) alt = "Pause";
        else if (strcasecmp(command, "ChannelUp") == 0) alt = "PageUp";
        else if (strcasecmp(command, "ChannelDown") == 0) alt = "PageDown";

        const cJSON *c_elem = NULL;
        cJSON_ArrayForEach(c_elem, cmds) {
            cJSON *c_name = cJSON_GetObjectItemCaseSensitive(c_elem, "name");
            if (c_name && cJSON_IsString(c_name) && c_name->valuestring) {
                if (strcmp(c_name->valuestring, command) == 0 ||
                    strcasecmp(c_name->valuestring, command) == 0 ||
                    (alt && (strcmp(c_name->valuestring, alt) == 0 || strcasecmp(c_name->valuestring, alt) == 0))) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

int hw_device_command_exists(const char *target, int match_mode, const char *command) {
    if (!target || !target[0] || !command || !command[0]) return 0;
    struct cfg_snap *snap = NULL;
    const cJSON *root = get_device_list_acquire(&snap);
    if (!root) return 0;
    int found = device_command_exists_in(root, target, match_mode, command);
    release_device_list(&snap);
    return found;
}

int hw_bthid_send_report(const uint8_t *report, size_t len) {
    if (!report || len == 0) return -1;

    char hex[256] = {0};
    size_t hlen = 0;
    for (size_t i = 0; i < len && (hlen + 3) < sizeof(hex); i++) {
        snprintf(hex + hlen, sizeof(hex) - hlen, "%02X", report[i]);
        hlen += 2;
    }
    snprintf(hex + hlen, sizeof(hex) - hlen, "\n");

    int fd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        write(fd, hex, strlen(hex));
        close(fd);
        return 0;
    }
    return -1;
}

static int device_bt_host_in(const cJSON *root, const char *device_id, char *out, size_t outlen) {
    cJSON *devs = root ? cJSON_GetObjectItemCaseSensitive(root, "devices") : NULL;
    if (!devs || !cJSON_IsArray(devs)) return 0;
    cJSON *dev = NULL;
    cJSON_ArrayForEach(dev, devs) {
        cJSON *j_id = cJSON_GetObjectItemCaseSensitive(dev, "id");
        char id_str[64] = {0};
        if (j_id && cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%ld", (long)j_id->valuedouble);
        else if (j_id && cJSON_IsString(j_id) && j_id->valuestring) snprintf(id_str, sizeof(id_str), "%s", j_id->valuestring);
        if (strcmp(id_str, device_id) != 0) continue;

        cJSON *j_bta = cJSON_GetObjectItemCaseSensitive(dev, "bdaddr");
        if (!j_bta) j_bta = cJSON_GetObjectItemCaseSensitive(dev, "btHost");
        if (j_bta && cJSON_IsString(j_bta) && hub_valid_bdaddr(j_bta->valuestring)) {
            snprintf(out, outlen, "%s", j_bta->valuestring);
            return 1;
        }
        /* Same fallback as the send path: a BT device without an address uses the default host */
        cJSON *j_type = cJSON_GetObjectItemCaseSensitive(dev, "type");
        int is_bt = j_type && cJSON_IsString(j_type) && j_type->valuestring &&
                    (strcasecmp(j_type->valuestring, "BT") == 0 || strcasestr(j_type->valuestring, "bluetooth"));
        if (!is_bt) return 0;
        FILE *tf = fopen("/data/harmony/bthid_host_target", "r");
        if (!tf) return 0;
        char line[128], addr[32] = {0};
        while (fgets(line, sizeof(line), tf)) {
            if (strncmp(line, "bdaddr=", 7) == 0) sscanf(line + 7, "%31s", addr);
        }
        fclose(tf);
        if (!hub_valid_bdaddr(addr)) return 0;
        snprintf(out, outlen, "%s", addr);
        return 1;
    }
    return 0;
}

int hw_device_bt_host(const char *device_id, char *out, size_t outlen) {
    if (!device_id || !device_id[0] || !out || outlen < 18) return 0;
    out[0] = '\0';
    struct cfg_snap *snap = NULL;
    const cJSON *root = get_device_list_acquire(&snap);
    int found = device_bt_host_in(root, device_id, out, outlen);
    release_device_list(&snap);
    return found;
}

int hw_device_bt_disconnect(const char *device_id) {
    char addr[32];
    if (!hw_device_bt_host(device_id, addr, sizeof(addr))) return -1;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "disconnect_host %s\n", addr);
    printf("[*] BT: closing the link of device %s (host %s) after its power off\n", device_id, addr);
    fflush(stdout);
    /* Through the key pipe, behind the device's power-off key: over the command socket the
     * close could overtake the key still waiting in the pipe, and the device stayed on */
    int fd = g_dry_run ? -1 : open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        ssize_t w = write(fd, cmd, strlen(cmd));
        close(fd);
        if (w == (ssize_t)strlen(cmd)) return 0;
    }
    return hub_send_btstack_cmd(cmd);
}

int hw_bt_release_all(void) {
    int fd = open("/tmp/bthid_input", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    ssize_t w = write(fd, "RELEASE\n", 8);
    close(fd);
    return (w == 8) ? 0 : -1;
}

int hw_plugin_command_send(const char *plugin_id, const char *dev_id, const char *dev_name,
                          const char *command, int action, int value, const cJSON *config) {
    /* the id goes into the socket path */
    if (!plugin_mgr_is_safe_id(plugin_id) || !command || !command[0]) return -1;

    char sock_path[128];
    snprintf(sock_path, sizeof(sock_path), HARMONY_PLUGINS_RUN_DIR "/%s.sock", plugin_id);

    const char *action_str = "press";
    if (action == HW_BTN_ACTION_RELEASE) action_str = "release";
    else if (action == HW_BTN_ACTION_REPEAT) action_str = "repeat";
    else if (action == HW_BTN_ACTION_TAP) action_str = "tap";

    char hub_name[64];
    hw_get_hub_name(hub_name, sizeof(hub_name));
    if (g_dry_run) {
        printf("[DRY-RUN] Plugin command simulated: plugin=%s dev=%s (%s) cmd=%s action=%s val=%d hub=%s sock=%s\n",
               plugin_id, dev_id ? dev_id : "", dev_name ? dev_name : "", command, action_str, value, hub_name, sock_path);
        return 0;
    }

    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, sock_path, sizeof(sun.sun_path) - 1);

    cJSON *msg = cJSON_CreateObject();
    if (msg) {
        cJSON_AddStringToObject(msg, "plugin_id", plugin_id);
        cJSON_AddStringToObject(msg, "hub_name", hub_name);
        cJSON_AddStringToObject(msg, "device_id", dev_id ? dev_id : "");
        cJSON_AddStringToObject(msg, "device_name", dev_name ? dev_name : "");
        cJSON_AddStringToObject(msg, "command", command);
        cJSON_AddStringToObject(msg, "action", action_str);
        cJSON_AddNumberToObject(msg, "value", value);
        /* When it was queued (CLOCK_MONOTONIC ms): a plugin drops a repeat that waited too long */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        cJSON_AddNumberToObject(msg, "ts", (double)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL));
        if (config) {
            /* A device with no "config" object goes as itself, but not its command list: the
             * datagram outgrew the plugin's buffer and the fields after it were lost */
            cJSON *cfg = cJSON_CreateObject();
            const cJSON *it = NULL;
            cJSON_ArrayForEach(it, config) {
                if (it->string && strcmp(it->string, "commands") != 0) {
                    cJSON_AddItemReferenceToObject(cfg, it->string, (cJSON *)it);
                }
            }
            cJSON_AddItemToObject(msg, "config", cfg);
        }
    }

    char *json = msg ? cJSON_PrintUnformatted(msg) : NULL;
    ssize_t sent = -1;
    if (json) {
        /* Never waits: this runs under the lock of every device send, and a plugin busy with
         * a slow device (its queue full) stalled IR and Bluetooth keys behind it */
        sent = sendto(s, json, strlen(json), MSG_DONTWAIT, (struct sockaddr *)&sun, sizeof(sun));
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            printf("[!] Plugin '%s' busy (queue full): '%s' %s dropped\n", plugin_id, command, action_str);
        }
        free(json);
    }
    if (msg) cJSON_Delete(msg);
    close(s);
    return (sent > 0) ? 0 : -1;
}
