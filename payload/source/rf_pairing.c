#define _GNU_SOURCE
#include "rf_pairing.h"
#include "rf_spi.h"
#include "hub_led.h"
#include "config_store.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>
#include <pthread.h>

/* Every light change by hub_rf counts here, so a delayed restore can tell that the
   light changed again in the meantime. A setter bumps the count and writes the light
   under s_led_lock, so a check of the count and the write that follows it cannot
   interleave with another setter. */
static pthread_mutex_t s_led_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned s_led_changes;

#define HUB_LED_FAILURE_S 5

/* Puts the Hub light back to the led_mode setting hub_daemon saved.
   The caller holds s_led_lock. */
static void led_restore_locked(void) {
    ++s_led_changes;
    char mode[32] = {0};
    FILE *f = fopen(HARMONY_SETTINGS_FILE, "r");
    if (f) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        cJSON *root = cJSON_Parse(buf);
        cJSON *item = root ? cJSON_GetObjectItemCaseSensitive(root, "led_mode") : NULL;
        if (cJSON_IsString(item) && item->valuestring) {
            snprintf(mode, sizeof(mode), "%s", item->valuestring);
        }
        cJSON_Delete(root);
    }
    hub_led_write(hub_led_mode_value(mode));
}

void hub_led_show(int value) {
    pthread_mutex_lock(&s_led_lock);
    ++s_led_changes;
    hub_led_write(value);
    pthread_mutex_unlock(&s_led_lock);
}

void hub_led_restore(void) {
    pthread_mutex_lock(&s_led_lock);
    led_restore_locked();
    pthread_mutex_unlock(&s_led_lock);
}

/* Restores the light only if nothing changed it since change number shown */
static void led_restore_if_unchanged(unsigned shown) {
    pthread_mutex_lock(&s_led_lock);
    if (s_led_changes == shown) led_restore_locked();
    pthread_mutex_unlock(&s_led_lock);
}

static void *failure_restore_worker(void *arg) {
    unsigned shown = (unsigned)(uintptr_t)arg;
    sleep(HUB_LED_FAILURE_S);
    led_restore_if_unchanged(shown);
    return NULL;
}

void hub_led_show_failure(void) {
    pthread_mutex_lock(&s_led_lock);
    unsigned shown = ++s_led_changes;
    hub_led_write(HUB_LED_RED_SLOW);
    pthread_mutex_unlock(&s_led_lock);
    pthread_t th;
    if (rf_create_thread(&th, 64 * 1024, failure_restore_worker, (void *)(uintptr_t)shown) == 0) {
        pthread_detach(th);
    } else {
        led_restore_if_unchanged(shown);
    }
}

static void stop_pairing(bool timed_out);

/* Counts pairing windows: the timer of a window that was stopped must not end a later one that
 * opened within its last second */
static volatile unsigned s_pairing_window;

static void *pairing_timeout_worker(void *arg) {
    unsigned window = (unsigned)(uintptr_t)arg;
    for (int i = 0; i < 30 && g_running && g_pairing_mode && s_pairing_window == window; i++) {
        sleep(1);
    }
    if (g_pairing_mode && s_pairing_window == window) {
        printf("[*] Pairing mode timed out after 30s\n");
        stop_pairing(true);
    }
    return NULL;
}

void start_pairing_mode(int slot) {
    if (g_pairing_mode) {
        /* a second request while the window is open: its slot replaces the first one */
        rf_slot_map_set_pair_target((uint8_t)(slot > 0 ? slot : 0), false);
        return;
    }
    rf_slot_map_set_pair_target((uint8_t)(slot > 0 ? slot : 0), true);
    uint8_t duration_sec = 0x1E; /* 30 seconds lock duration (matches stock hal_hub libhal_rf_host_open_lock) */
    uint8_t open_lock[7] = {0x10, 0xFF, 0x80, 0xB2, 0x01, 0x00, duration_sec};
    printf("[*] Opening pairing lock (duration=%ds, 10 FF 80 B2 01 00 %02X)...\n", duration_sec, duration_sec);
    rf_write_cmd(open_lock, sizeof(open_lock));
    unsigned window = ++s_pairing_window;
    g_pairing_mode = true;

    hub_led_show(HUB_LED_GREEN_FAST);

    pthread_t timer_th;
    if (rf_create_thread(&timer_th, 64 * 1024, pairing_timeout_worker, (void *)(uintptr_t)window) == 0) {
        pthread_detach(timer_th);
    } else {
        /* without the timer nothing would end the pairing mode: the CC2544 closes its lock
         * after 30 s by itself, and start_pairing_mode() refused every request until a stop */
        printf("[-] Pairing timer not started, closing the pairing lock\n");
        stop_pairing(true);
    }
}

/* A timeout shows as a failure on the light; a pairing or a cancel does not. A timeout or a
 * cancel drops the chosen slot: a lock that closes without a pairing must not place a remote. */
static void stop_pairing(bool timed_out) {
    if (timed_out) rf_slot_map_set_pair_target(0, false);
    if (g_pairing_mode) {
        g_pairing_mode = false;
        uint8_t close_lock[7] = {0x10, 0xFF, 0x80, 0xB2, 0x02, 0x00, 0x00};
        printf("[*] Closing pairing lock (10 FF 80 B2 02 00 00)...\n");
        rf_write_cmd(close_lock, sizeof(close_lock));
        if (timed_out) hub_led_show_failure();
        else hub_led_restore();
    }
}

void stop_pairing_mode(void) {
    rf_slot_map_set_pair_target(0, false);
    stop_pairing(false);
}

void stop_pairing_after_success(void) {
    stop_pairing(false);
}

void *hub_button_worker(void *arg) {
    (void)arg;
    int btn_fd = open("/dev/input/event0", O_RDONLY);
    if (btn_fd < 0) {
        printf("[-] Warning: cannot open /dev/input/event0 for Hub Pair button: %s\n", strerror(errno));
        return NULL;
    }
    printf("[+] Hub Pair button listener started on /dev/input/event0\n");

    struct input_event ev;
    while (g_running) {
        ssize_t n = read(btn_fd, &ev, sizeof(ev));
        if (n == (ssize_t)sizeof(ev)) {
            if (ev.type == 1 && ev.value == 1) { /* EV_KEY press */
                printf("[*] Hub Physical Pair Button Pressed! (code=%d)\n", ev.code);
                if (g_pairing_mode) {
                    printf("[*] Pairing currently active -> Stopping pairing mode\n");
                    stop_pairing_mode();
                } else {
                    printf("[*] Starting pairing mode from Hub Pair Button\n");
                    /* the lowest slot without a remote */
                    int target_slot = 0;
                    for (int s = 1; s <= MAX_PAIRED_REMOTES && !target_slot; s++)
                        if (!g_paired_remotes[s - 1].active) target_slot = s;
                    start_pairing_mode(target_slot);
                }
            }
        } else if (n < 0) {
            if (errno == EINTR) continue;
            usleep(100000);
        }
    }
    close(btn_fd);
    return NULL;
}

void unpair_device(int slot) {
    /* One slot at a time, also for "every remote": the CC2544's slot-0 "unpair all" request is
       NAKed on some Hub firmware. Each remote is told first, so its own CC2544 clears the Hub
       bond before the Hub drops the link. The command names the slot's CC2544 pipe. */
    if (slot < 0 || slot > MAX_PAIRED_REMOTES) {
        RF_LOG(LOG_LEVEL_WARN, "[-] Refusing invalid RF unpair slot %d\n", slot);
        return;
    }
    int first_slot = slot == 0 ? 1 : slot;
    int last_slot = slot == 0 ? MAX_PAIRED_REMOTES : slot;
    /* "every remote": the notice and the cleanup only for slots that hold one (an empty pipe may
     * hold a keyboard or a Companion, and needs no version file); the radio command for all six,
     * as a stale pairing is not always in the Hub's table */
    bool held[MAX_PAIRED_REMOTES + 1] = { false };
    for (int fs = 1; fs <= MAX_PAIRED_REMOTES; fs++)
        held[fs] = slot == fs || (slot == 0 && g_paired_remotes[fs - 1].active);
    for (int target = first_slot; target <= last_slot; target++) {
        /* the notice only for a touchscreen remote: a Companion or Smart Control does not
         * handle it */
        if (held[target] && !is_companion_slot((uint8_t)target)) {
            rf_frame_t unpair_msg;
            memset(&unpair_msg, 0, sizeof(unpair_msg));
            unpair_msg.report_id = 0x20;
            unpair_msg.sub_id = 0x12;
            unpair_msg.dev_slot = (uint8_t)target;
            unpair_msg.msg_type = RF_MSG_UNPAIR_NOTIFY;
            unpair_msg.hdr_flag = 0x50 | (g_tx_seq & 0x0F);
            unpair_msg.seq = g_tx_seq++;
            rf_send_frame(&unpair_msg);
            usleep(30000); /* the frame goes out before the slot is removed */
        }

        uint8_t pipe = rf_slot_to_pipe((uint8_t)target);
        rf_slot_map_note_table(pipe, false); /* a pairing on this pipe is then a new one */
        uint8_t unpair[] = { 0x10, 0xFF, 0x80, 0xB2, 0x03, pipe, 0x00 };
        RF_LOG(LOG_LEVEL_INFO, "[*] Unpairing remote (slot=%d, pipe 10 FF 80 B2 03 %02X 00)...\n", target, pipe);
        rf_write_cmd(unpair, sizeof(unpair));
        usleep(30000);
    }
    /* everything known about the remote goes with the pairing (the same cleanup as for an
     * unpair the remote asked for) */
    for (int fs = 1; fs <= MAX_PAIRED_REMOTES; fs++) {
        if (held[fs]) rf_slot_forget((uint8_t)fs);
    }
    /* a push waiting for an unpaired remote has no remote left to go to; the status only hid it
     * while the slot was still known as a Companion */
    int pending = rf_sync_pending_slot(NULL, 0);
    if (pending >= 1 && pending <= MAX_PAIRED_REMOTES && held[pending]) {
        unlink("/data/harmony/rf_sync_pending.flag");
        unlink("/data/harmony/rf_sync_pending_details.json");
    }
    if (!rf_any_slot_active()) {
        g_has_paired_device = false;
        memset(g_paired_addr, 0, sizeof(g_paired_addr));
    }
}
