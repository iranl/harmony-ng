#define _GNU_SOURCE
#include "rf_ota.h"
#include "rf_spi.h"
#include "rf_pairing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

bool is_ota_busy(void) {
    pthread_mutex_lock(&g_ota_lock);
    bool busy = g_rf_ota.in_progress;
    pthread_mutex_unlock(&g_ota_lock);
    return busy;
}

void ota_set_progress(uint32_t sent_bytes, uint16_t sent_chunks) {
    pthread_mutex_lock(&g_ota_lock);
    g_rf_ota.sent_bytes = sent_bytes;
    g_rf_ota.sent_chunks = sent_chunks;
    pthread_mutex_unlock(&g_ota_lock);
}

void ota_set_status(int status, bool in_prog, const char *err) {
    pthread_mutex_lock(&g_ota_lock);
    g_rf_ota.status_code = status;
    g_rf_ota.in_progress = in_prog;
    if (err) {
        snprintf(g_rf_ota.error_msg, sizeof(g_rf_ota.error_msg), "%s", err);
    } else {
        g_rf_ota.error_msg[0] = '\0';
    }
    pthread_mutex_unlock(&g_ota_lock);
    set_hub_led(in_prog ? 2 : 1);
    if (!in_prog) {
        if (status == 2) {
            printf("[+] Hub RF OTA finished: SUCCESS\n");
        } else if (status < 0) {
            printf("[-] Hub RF OTA finished: FAILED (%s)\n", err ? err : "unknown error");
        }
    }
}

void *rf_ota_sender_worker(void *arg) {
    uint8_t target_slot = (uint8_t)(uintptr_t)arg;
    if (target_slot < 1 || target_slot > 2) target_slot = g_remote_slot ? g_remote_slot : 1;

    if (is_companion_slot(target_slot)) {
        printf("[-] Hub RF OTA rejected: slot %d is a Companion remote (no OTA support)\n", target_slot);
        ota_set_status(-1, false, "Remote model does not support OTA");
        return NULL;
    }

    pthread_mutex_lock(&g_ota_lock);
    g_rf_ota.in_progress = true;
    g_rf_ota.target_slot = target_slot;
    g_rf_ota.status_code = 1;
    g_rf_ota.error_msg[0] = '\0';
    g_rf_ota.sent_bytes = 0;
    g_rf_ota.sent_chunks = 0;
    g_rf_ota.total_bytes = 0;
    g_rf_ota.total_chunks = 0;
    pthread_mutex_unlock(&g_ota_lock);

    const char *ota_path = "/cache/elite_ui.ota.gz";
    if (access(ota_path, R_OK) != 0) ota_path = "/data/elite_ui.ota.gz";
    if (access(ota_path, R_OK) != 0) ota_path = "/tmp/elite_ui.ota.gz";
    FILE *f = fopen(ota_path, "rb");
    if (!f) {
        char err[128];
        snprintf(err, sizeof(err), "OTA image not found (%s)", ota_path);
        ota_set_status(-1, false, err);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 10000000) {
        fclose(f);
        char err[128];
        snprintf(err, sizeof(err), "Invalid OTA file size: %ld", file_size);
        ota_set_status(-1, false, err);
        return NULL;
    }

    uint32_t tot_bytes = (uint32_t)file_size;
    uint16_t tot_chunks = (uint16_t)((tot_bytes + 21) / 22);

    pthread_mutex_lock(&g_ota_lock);
    g_rf_ota.total_bytes = tot_bytes;
    g_rf_ota.total_chunks = tot_chunks;
    pthread_mutex_unlock(&g_ota_lock);

    printf("[*] Starting RF OTA to slot %d: %u bytes, %u chunks from %s\n",
           target_slot, tot_bytes, tot_chunks, ota_path);

    /* 1. RF_MSG_OTA_START */
    rf_frame_t frame;
    uint8_t start_payload[25] = {0};
    start_payload[0] = (uint8_t)(tot_bytes & 0xFF);
    start_payload[1] = (uint8_t)((tot_bytes >> 8) & 0xFF);
    start_payload[2] = (uint8_t)((tot_bytes >> 16) & 0xFF);
    start_payload[3] = (uint8_t)((tot_bytes >> 24) & 0xFF);
    start_payload[4] = (uint8_t)(tot_chunks & 0xFF);
    start_payload[5] = (uint8_t)((tot_chunks >> 8) & 0xFF);

    int ack_res = -1;
    for (int attempt = 1; attempt <= 10 && g_running; attempt++) {
        rf_build_frame(&frame, target_slot, RF_MSG_OTA_START, ++g_tx_seq, RF_FLAG_ACK_REQ, start_payload, 6);
        ack_res = rf_send_frame_acked(&frame, 1000);
        if (ack_res == 0) break;
        if (attempt < 10) {
            printf("[*] Waiting for remote in slot %d to respond to OTA_START (attempt %d/10)...\n",
                   target_slot, attempt);
            usleep(800000);
        }
    }

    if (ack_res != 0) {
        fclose(f);
        ota_set_status(-1, false, "Remote failed to ACK OTA_START (timeout - wake remote)");
        return NULL;
    }
    if (g_ack_status != 0x00) {
        fclose(f);
        char err[128];
        if (g_ack_status == 0x02) {
            snprintf(err, sizeof(err), "Remote rejected OTA: Low battery (<40%%)");
        } else {
            snprintf(err, sizeof(err), "Remote rejected OTA: Error code 0x%02X", g_ack_status);
        }
        ota_set_status(-1, false, err);
        return NULL;
    }

    /* 2. Stream RF_MSG_OTA_CHUNK frames */
    uint16_t c_idx = 0;
    uint32_t sent_bytes = 0;
    while (c_idx < tot_chunks && g_running) {
        uint8_t chunk_data[22];
        size_t rd = fread(chunk_data, 1, sizeof(chunk_data), f);
        if (rd == 0) break;

        uint8_t chunk_payload[25] = {0};
        chunk_payload[0] = (uint8_t)(c_idx & 0xFF);
        chunk_payload[1] = (uint8_t)((c_idx >> 8) & 0xFF);
        chunk_payload[2] = (uint8_t)rd;
        memcpy(&chunk_payload[3], chunk_data, rd);

        bool need_ack = ((c_idx + 1 == tot_chunks) || (c_idx % 4 == 0));
        uint8_t flags = need_ack ? RF_FLAG_ACK_REQ : 0;
        rf_build_frame(&frame, target_slot, RF_MSG_OTA_CHUNK, ++g_tx_seq, flags, chunk_payload, (uint8_t)(3 + rd));

        if (need_ack) {
            int retries = 0;
            int ack_ok = -1;
            while (retries < 5) {
                if (rf_send_frame_acked(&frame, 1200) == 0 && g_ack_status == 0x00) {
                    ack_ok = 0;
                    break;
                }
                retries++;
                usleep(35000);
            }
            if (ack_ok != 0) {
                fclose(f);
                char err[128];
                snprintf(err, sizeof(err), "Chunk %u ACK timeout", c_idx);
                ota_set_status(-1, false, err);
                return NULL;
            }
        } else {
            rf_send_frame(&frame);
        }

        sent_bytes += rd;
        ota_set_progress(sent_bytes, c_idx + 1);
        if ((c_idx + 1) % 250 == 0 || c_idx + 1 == tot_chunks) {
            uint32_t pct = tot_chunks > 0 ? (uint32_t)(((c_idx + 1) * 100) / tot_chunks) : 0;
            printf("[*] Hub RF OTA to Slot %d: %u%% (%u/%u chunks, %u/%u bytes)\n",
                   target_slot, pct, c_idx + 1, tot_chunks, sent_bytes, tot_bytes);
        }
        c_idx++;
        usleep(15000);
    }

    fclose(f);

    /* 3. RF_MSG_OTA_END */
    uint8_t end_payload[25] = {0};
    rf_build_frame(&frame, target_slot, RF_MSG_OTA_END, ++g_tx_seq, RF_FLAG_ACK_REQ, end_payload, 1);
    if (rf_send_frame_acked(&frame, 6000) != 0) {
        ota_set_status(-1, false, "OTA_END ACK timeout");
        return NULL;
    }

    if (g_ack_status != 0x00) {
        char err[128];
        snprintf(err, sizeof(err), "Remote verification failed (status 0x%02X)", g_ack_status);
        ota_set_status(-1, false, err);
        return NULL;
    }

    printf("[+] RF OTA completed successfully to slot %d!\n", target_slot);
    ota_set_status(2, false, NULL);
    return NULL;
}
