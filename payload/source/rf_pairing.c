#define _GNU_SOURCE
#include "rf_pairing.h"
#include "rf_spi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>
#include <pthread.h>

void set_hub_led(int state) {
    int fd = open("/sys/class/input/input0/led", O_WRONLY);
    if (fd >= 0) {
        /* On Harmony Hub Atheros input0: 0 is Off, 1 is Solid Red, 2 is Fast Blink, 3 is Slow Blink */
        const char *val = (state == 0) ? "0\n" : "1\n";
        ssize_t w = write(fd, val, strlen(val));
        (void)w;
        close(fd);
    }
}

static void *led_blink_worker(void *arg) {
    (void)arg;
    int state = 0;
    while (g_running && g_pairing_mode) {
        set_hub_led(state ? 1 : 0); /* Blink Red during pairing */
        state = !state;
        usleep(250000);
    }
    set_hub_led(1); /* Solid Red when ready */
    return NULL;
}

static void *pairing_timeout_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 30 && g_running && g_pairing_mode; i++) {
        sleep(1);
    }
    if (g_pairing_mode) {
        printf("[*] Pairing mode timed out after 30s\n");
        stop_pairing_mode();
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

    pthread_t led_th;
    if (pthread_create(&led_th, NULL, led_blink_worker, NULL) == 0) {
        pthread_detach(led_th);
    }

    pthread_t timer_th;
    if (pthread_create(&timer_th, NULL, pairing_timeout_worker, NULL) == 0) {
        pthread_detach(timer_th);
    }
}

void stop_pairing_mode(void) {
    if (g_pairing_mode) {
        g_pairing_mode = false;
        uint8_t close_lock[7] = {0x10, 0xFF, 0x80, 0xB2, 0x02, 0x00, 0x00};
        printf("[*] Closing pairing lock (10 FF 80 B2 02 00 00)...\n");
        rf_write_cmd(close_lock, sizeof(close_lock));
        set_hub_led(1);
    }
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
    }
    if (slot == 2 || slot == 0) {
        g_paired_remotes[1].active = false;
        memset(g_paired_remotes[1].rf_addr, 0, 4);
    }
    if (!g_paired_remotes[0].active && !g_paired_remotes[1].active) {
        g_has_paired_device = false;
        memset(g_paired_addr, 0, sizeof(g_paired_addr));
    }
}
