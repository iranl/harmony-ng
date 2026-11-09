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
    if (pthread_create(&th, NULL, failure_restore_worker, (void *)(uintptr_t)shown) == 0) {
        pthread_detach(th);
    } else {
        led_restore_if_unchanged(shown);
    }
}

static void stop_pairing(bool timed_out);

static void *pairing_timeout_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 30 && g_running && g_pairing_mode; i++) {
        sleep(1);
    }
    if (g_pairing_mode) {
        printf("[*] Pairing mode timed out after 30s\n");
        stop_pairing(true);
    }
    return NULL;
}

void start_pairing_mode(int slot) {
    if (g_pairing_mode) return;
    (void)slot;
    uint8_t duration_sec = 0x1E; /* 30 seconds lock duration (matches stock hal_hub libhal_rf_host_open_lock) */
    uint8_t open_lock[7] = {0x10, 0xFF, 0x80, 0xB2, 0x01, 0x00, duration_sec};
    printf("[*] Opening pairing lock (duration=%ds, 10 FF 80 B2 01 00 %02X)...\n", duration_sec, duration_sec);
    rf_write_cmd(open_lock, sizeof(open_lock));
    g_pairing_mode = true;

    hub_led_show(HUB_LED_GREEN_FAST);

    pthread_t timer_th;
    if (pthread_create(&timer_th, NULL, pairing_timeout_worker, NULL) == 0) {
        pthread_detach(timer_th);
    }
}

/* A timeout shows as a failure on the light; a pairing or a cancel does not */
static void stop_pairing(bool timed_out) {
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
                    int target_slot = (g_paired_remotes[0].active && !g_paired_remotes[1].active) ? 2 : 0;
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
    /* Send RF notification to remote before unpairing so remote clears bonding */
    rf_frame_t unpair_msg;
    memset(&unpair_msg, 0, sizeof(unpair_msg));
    unpair_msg.report_id = 0x20;
    unpair_msg.sub_id = 0x12;
    unpair_msg.msg_type = RF_MSG_UNPAIR_NOTIFY;
    unpair_msg.hdr_flag = 0x50 | (g_tx_seq & 0x0F);
    unpair_msg.seq = g_tx_seq++;
    if (slot == 1 || slot == 0) {
        unpair_msg.dev_slot = 0x01;
        rf_send_frame(&unpair_msg);
    }
    if (slot == 2 || slot == 0) {
        unpair_msg.dev_slot = 0x02;
        rf_send_frame(&unpair_msg);
    }
    usleep(30000); /* Allow RF frame transmission before hardware lock reset */

    uint8_t s = (slot == 2) ? 0x02 : ((slot == 1) ? 0x01 : 0x00);
    uint8_t unpair[] = { 0x10, 0xFF, 0x80, 0xB2, 0x03, s, 0x00 };
    RF_LOG(LOG_LEVEL_INFO, "[*] Unpairing remote (slot=%d, 10 FF 80 B2 03 %02X 00)...\n", slot, s);
    rf_write_cmd(unpair, sizeof(unpair));
    if (slot == 1 || slot == 0) {
        g_paired_remotes[0].active = false;
        memset(g_paired_remotes[0].rf_addr, 0, 4);
        /* the identity goes with the pairing: unit id, skin id, EquadID and family */
        g_paired_remotes[0].equad_id = 0;
        g_paired_remotes[0].model = REMOTE_MODEL_UNKNOWN;
        if (g_remote_versions[0].remote_id[0] || g_remote_versions[0].skin) {
            g_remote_versions[0].remote_id[0] = '\0';
            g_remote_versions[0].skin = 0;
            rf_state_save_version_file(1);
        }
    }
    if (slot == 2 || slot == 0) {
        g_paired_remotes[1].active = false;
        memset(g_paired_remotes[1].rf_addr, 0, 4);
        /* the identity goes with the pairing: unit id, skin id, EquadID and family */
        g_paired_remotes[1].equad_id = 0;
        g_paired_remotes[1].model = REMOTE_MODEL_UNKNOWN;
        if (g_remote_versions[1].remote_id[0] || g_remote_versions[1].skin) {
            g_remote_versions[1].remote_id[0] = '\0';
            g_remote_versions[1].skin = 0;
            rf_state_save_version_file(2);
        }
    }
    if (!g_paired_remotes[0].active && !g_paired_remotes[1].active) {
        g_has_paired_device = false;
        memset(g_paired_addr, 0, sizeof(g_paired_addr));
    }
}
