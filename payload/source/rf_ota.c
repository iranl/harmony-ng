#define _GNU_SOURCE
#include "rf_ota.h"
#include "rf_spi.h"
#include "rf_pairing.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* Windowed transfer (remotes that put RF_OTA_WINDOWED in their OTA_START ACK): the Hub streams
 * chunks without waiting, asks for an ACK every OTA_WIN_ACK_EVERY chunks, and the ACKs carry the
 * remote's contiguous chunk count. It stays at most OTA_WIN_IN_FLIGHT chunks ahead of that count
 * and resends from it when it stops advancing for OTA_WIN_REWIND_MS (go-back-N). On a Harmony
 * Ultimate this moved 248 chunks/s (5.4 KB/s) against 20 for the stop-and-wait transfer; a gap
 * below about 1 ms overflows the Hub's CC2544 and frames are lost. */
#define OTA_WIN_ACK_EVERY   16
#define OTA_WIN_GAP_US      2000
#define OTA_WIN_IN_FLIGHT   64
#define OTA_WIN_REWIND_MS   300
#define OTA_WIN_STALL_MS    20000

static volatile uint32_t s_win_contig;

void ota_note_chunk_ack(const uint8_t *payload) {
    if (!payload || payload[4] != RF_OTA_WINDOWED) return;
    uint32_t c = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8);
    if (c > s_win_contig) s_win_contig = c; /* ACKs can arrive out of order */
}

/* Returns true when the remote has every chunk; otherwise the status is set (resumable) */
static bool ota_send_windowed(FILE *f, uint8_t target_slot, uint16_t start, uint16_t tot_chunks, uint32_t tot_bytes) {
    rf_frame_t frame;
    s_win_contig = start;
    uint32_t next = start, last_contig = start, last_logged = start;
    uint64_t last_progress = get_monotonic_ms(), t0 = last_progress;
    unsigned rewinds = 0;

    while (g_running) {
        uint32_t contig = s_win_contig;
        uint64_t now = get_monotonic_ms();
        if (contig >= tot_chunks) break;
        if (contig > last_contig) {
            last_contig = contig;
            last_progress = now;
        }
        if (now - last_progress > OTA_WIN_STALL_MS) {
            char err[128];
            snprintf(err, sizeof(err), "Chunk %u: no progress for %d s (can resume)", contig, OTA_WIN_STALL_MS / 1000);
            ota_set_status(-1, false, err);
            return false;
        }
        if (now - last_progress > OTA_WIN_REWIND_MS && next > contig) {
            next = contig; /* go back to the first chunk the remote is missing */
            rewinds++;
            last_progress = now; /* give the resend a full period */
        }
        if (next < contig) next = contig;
        ota_set_progress(contig * 22 > tot_bytes ? tot_bytes : contig * 22, (uint16_t)contig);
        if (contig / 1000 != last_logged / 1000) {
            last_logged = contig;
            printf("[*] Hub RF OTA to Slot %d: %u%% (%u/%u chunks, windowed)\n", target_slot,
                   (uint32_t)((contig * 100ULL) / tot_chunks), contig, tot_chunks);
        }

        if (next < tot_chunks && next - contig < OTA_WIN_IN_FLIGHT) {
            uint8_t chunk_data[22];
            fseek(f, (long)next * 22, SEEK_SET);
            size_t rd = fread(chunk_data, 1, sizeof(chunk_data), f);
            if (rd == 0) {
                ota_set_status(-1, false, "OTA image read error");
                return false;
            }
            uint8_t chunk_payload[25] = {0};
            chunk_payload[0] = (uint8_t)(next & 0xFF);
            chunk_payload[1] = (uint8_t)((next >> 8) & 0xFF);
            chunk_payload[2] = (uint8_t)rd;
            memcpy(&chunk_payload[3], chunk_data, rd);
            bool want_ack = (next + 1 == tot_chunks) || (next % OTA_WIN_ACK_EVERY == 0);
            rf_build_frame(&frame, target_slot, RF_MSG_OTA_CHUNK, ++g_tx_seq, want_ack ? RF_FLAG_ACK_REQ : 0,
                           chunk_payload, (uint8_t)(3 + rd));
            rf_send_frame(&frame);
            next++;
            usleep(OTA_WIN_GAP_US);
        } else {
            usleep(1000); /* window full or everything sent: wait for ACKs */
        }
    }
    if (!g_running) return false;
    uint64_t el = get_monotonic_ms() - t0;
    printf("[+] Hub RF OTA to Slot %d: all %u chunks received by the remote in %llu s (windowed, %u resends from a gap)\n",
           target_slot, tot_chunks, (unsigned long long)(el / 1000), rewinds);
    return true;
}

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

    const char *ota_path = "/data/harmony/elite_ui.ota.gz";
    FILE *f = fopen(ota_path, "rb");
    if (!f) {
        ota_path = "/cache/elite_ui.ota.gz";
        f = fopen(ota_path, "rb");
    }
    if (!f) {
        char err[128];
        snprintf(err, sizeof(err), "OTA image not found (/data/harmony or /cache)");
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

    /* Pre-compute CRC32 and SHA-256 of entire file upfront for resume and verification */
    uint32_t file_crc = 0;
    uint8_t ota_digest[32] = {0};
    {
        uint8_t cbuf[4096];
        size_t crd;
        uint32_t cval = 0xFFFFFFFF;
        sha256_ctx_t full_sha;
        sha256_init(&full_sha);
        while ((crd = fread(cbuf, 1, sizeof(cbuf), f)) > 0) {
            for (size_t i = 0; i < crd; i++) {
                cval ^= cbuf[i];
                for (int j = 0; j < 8; j++) {
                    cval = (cval >> 1) ^ (0xEDB88320 & (-(int)(cval & 1)));
                }
            }
            sha256_update(&full_sha, cbuf, crd);
        }
        file_crc = ~cval;
        sha256_final(ota_digest, &full_sha);
        fseek(f, 0, SEEK_SET);
    }

    /* 1. RF_MSG_OTA_START with CRC32 file ID */
    rf_frame_t frame;
    uint8_t start_payload[25] = {0};
    start_payload[0] = (uint8_t)(tot_bytes & 0xFF);
    start_payload[1] = (uint8_t)((tot_bytes >> 8) & 0xFF);
    start_payload[2] = (uint8_t)((tot_bytes >> 16) & 0xFF);
    start_payload[3] = (uint8_t)((tot_bytes >> 24) & 0xFF);
    start_payload[4] = (uint8_t)(tot_chunks & 0xFF);
    start_payload[5] = (uint8_t)((tot_chunks >> 8) & 0xFF);
    start_payload[6] = (uint8_t)(file_crc & 0xFF);
    start_payload[7] = (uint8_t)((file_crc >> 8) & 0xFF);
    start_payload[8] = (uint8_t)((file_crc >> 16) & 0xFF);
    start_payload[9] = (uint8_t)((file_crc >> 24) & 0xFF);

    int ack_res = -1;
    for (int attempt = 1; attempt <= 10 && g_running; attempt++) {
        rf_build_frame(&frame, target_slot, RF_MSG_OTA_START, ++g_tx_seq, RF_FLAG_ACK_REQ, start_payload, 10);
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

    /* Check if remote requested to resume from existing progress */
    uint16_t c_idx = 0;
    uint32_t sent_bytes = 0;
    uint16_t resume_chunk = (uint16_t)g_ack_payload[2] | ((uint16_t)g_ack_payload[3] << 8);
    if (resume_chunk > 0 && resume_chunk < tot_chunks) {
        c_idx = resume_chunk;
        sent_bytes = (uint32_t)resume_chunk * 22;
        fseek(f, (long)sent_bytes, SEEK_SET);
        ota_set_progress(sent_bytes, c_idx);
        printf("[+] Remote requested resume from chunk %u (%u / %u bytes, %u%%)\n",
               c_idx, sent_bytes, tot_bytes, (uint32_t)((sent_bytes * 100ULL) / tot_bytes));
    }

    /* 2. Stream RF_MSG_OTA_CHUNK frames: windowed if the remote supports it, else stop-and-wait */
    bool windowed = (g_ack_payload[4] == RF_OTA_WINDOWED);
    if (windowed) {
        printf("[*] Remote supports the windowed OTA transfer\n");
        if (!ota_send_windowed(f, target_slot, c_idx, tot_chunks, tot_bytes)) {
            fclose(f);
            return NULL;
        }
        c_idx = tot_chunks;
    }
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
            while (retries < 15 && g_running) {
                if (rf_send_frame_acked(&frame, 1200) == 0 && g_ack_status == 0x00) {
                    ack_ok = 0;
                    break;
                }
                retries++;
                if (retries == 3 || retries == 7 || retries == 11) {
                    uint8_t sync_pulse[10] = {0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
                    rf_write_cmd(sync_pulse, sizeof(sync_pulse));
                }
                int backoff_ms = 50 + (retries * 15);
                if (backoff_ms > 275) backoff_ms = 275;
                usleep(backoff_ms * 1000);
            }
            if (ack_ok != 0) {
                fclose(f);
                char err[128];
                snprintf(err, sizeof(err), "Chunk %u ACK timeout (can resume)", c_idx);
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

    /* 3. RF_MSG_OTA_END with SHA-256 digest */
    uint8_t end_payload[25] = {0};
    end_payload[0] = 0x01; /* Algorithm 1 = SHA-256 (20-byte prefix) */
    memcpy(&end_payload[1], ota_digest, 20);
    rf_build_frame(&frame, target_slot, RF_MSG_OTA_END, ++g_tx_seq, RF_FLAG_ACK_REQ, end_payload, 21);
    if (rf_send_frame_acked(&frame, 6000) != 0) {
        ota_set_status(-1, false, "OTA_END ACK timeout");
        return NULL;
    }

    if (g_ack_status != 0x00) {
        char err[128];
        if (g_ack_status == 0x06) {
            snprintf(err, sizeof(err), "Remote rejected OTA: SHA-256 integrity check failed");
        } else {
            snprintf(err, sizeof(err), "Remote verification failed (status 0x%02X)", g_ack_status);
        }
        ota_set_status(-1, false, err);
        return NULL;
    }

    printf("[+] RF OTA completed successfully to slot %d!\n", target_slot);
    ota_set_status(2, false, NULL);
    return NULL;
}
