#define _GNU_SOURCE
#include "rf_ota.h"
#include "rf_spi.h"
#include "rf_pairing.h"
#include "hub_led.h"
#include "sha256.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>

/* Windowed transfer (remotes that put RF_OTA_WINDOWED in their OTA_START ACK): the Hub puts the
 * CC2544 in RF_PM_TRANSFER as Logitech's firmware does for its HOT transfers (the remote's radio
 * then fetches the queued frames by itself), streams RF_OTA_DATA frames (26 data bytes each)
 * without waiting, asks for an ACK every OTA_WIN_ACK_EVERY chunks, and the ACKs carry the
 * remote's contiguous chunk count. It stays at most OTA_WIN_IN_FLIGHT chunks ahead of that count
 * and resends from it when it stops advancing for OTA_WIN_REWIND_MS (go-back-N). The Hub's
 * CC2544 queues 8 frames: more than about one frame per 2 ms overflows it and frames are lost. */
#define OTA_WIN_ACK_EVERY   16
#define OTA_WIN_GAP_US      2000
#define OTA_WIN_IN_FLIGHT   64
#define OTA_WIN_REWIND_MS   300
#define OTA_WIN_STALL_MS    20000

static volatile uint32_t s_win_contig;
static volatile uint8_t s_win_slot; /* slot of the windowed transfer in progress, 0 = none */

void ota_note_chunk_ack(uint8_t slot, const uint8_t *payload) {
    if (!payload || payload[4] != RF_OTA_WINDOWED) return;
    if (!s_win_slot || (slot & 0x0F) != s_win_slot) return; /* another remote's ACK */
    uint32_t c = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8);
    if (c > s_win_contig) s_win_contig = c; /* ACKs can arrive out of order */
}

/* Returns true when the remote has every chunk; otherwise the status is set (resumable) */
static bool ota_send_windowed(FILE *f, uint8_t target_slot, uint32_t start, uint32_t tot_chunks, uint32_t tot_bytes) {
    s_win_contig = start;
    s_win_slot = target_slot & 0x0F;
    uint32_t next = start, last_contig = start, last_logged = start;
    /* last_progress moves only when the remote's count grows (stall abort); last_rewind paces
     * the resends, which must not count as progress or a remote that is gone is retried forever */
    uint64_t last_progress = get_monotonic_ms(), last_rewind = last_progress, t0 = last_progress;
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
            s_win_slot = 0;
            ota_set_status(-1, false, err);
            return false;
        }
        if (now - last_progress > OTA_WIN_REWIND_MS && now - last_rewind > OTA_WIN_REWIND_MS && next > contig) {
            next = contig; /* go back to the first chunk the remote is missing */
            rewinds++;
            last_rewind = now; /* give the resend a full period */
        }
        if (next < contig) next = contig;
        uint64_t done = (uint64_t)contig * RF_OTA_DATA_BYTES;
        ota_set_progress(done > tot_bytes ? tot_bytes : (uint32_t)done, (uint16_t)contig);
        if (contig / 1000 != last_logged / 1000) {
            last_logged = contig;
            printf("[*] Hub RF OTA to Slot %d: %u%% (%u/%u chunks, windowed)\n", target_slot,
                   (uint32_t)((contig * 100ULL) / tot_chunks), contig, tot_chunks);
        }

        if (next < tot_chunks && next - contig < OTA_WIN_IN_FLIGHT) {
            uint8_t chunk_data[RF_OTA_DATA_BYTES];
            fseek(f, (long)next * RF_OTA_DATA_BYTES, SEEK_SET);
            size_t rd = fread(chunk_data, 1, sizeof(chunk_data), f);
            if (rd == 0) {
                s_win_slot = 0;
                ota_set_status(-1, false, "OTA image read error");
                return false;
            }
            bool want_ack = (next + 1 == tot_chunks) || (next % OTA_WIN_ACK_EVERY == 0);
            rf_send_ota_data(target_slot, next, want_ack, chunk_data, rd);
            next++;
            usleep(OTA_WIN_GAP_US);
        } else {
            usleep(1000); /* window full or everything sent: wait for ACKs */
        }
    }
    s_win_slot = 0;
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
    if (in_prog) hub_led_show(HUB_LED_ALT_SLOW);
    else if (status < 0) hub_led_show_failure();
    else hub_led_restore();
    if (!in_prog) {
        rf_set_power_mode(RF_PM_ACTIVE);
        if (status == 2) {
            printf("[+] Hub RF OTA finished: SUCCESS\n");
        } else if (status < 0) {
            printf("[-] Hub RF OTA finished: FAILED (%s)\n", err ? err : "unknown error");
        }
    }
}

static void get_ota_repo_source(char *out_repo, size_t out_sz) {
    out_repo[0] = '\0';
    FILE *f = fopen("/data/harmony/update_state.conf", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "source=", 7) == 0) {
                char *src = line + 7;
                char *nl = strchr(src, '\r'); if (nl) *nl = '\0';
                nl = strchr(src, '\n'); if (nl) *nl = '\0';
                if (*src) strncpy(out_repo, src, out_sz - 1);
                break;
            }
        }
        fclose(f);
    }
    if (!out_repo[0]) {
        strncpy(out_repo, "https://raw.githubusercontent.com/iranl/harmony-ng/ota", out_sz - 1);
    }
}

static void get_hub_version(char *out_ver, size_t sz) {
    out_ver[0] = '\0';
    const char *vdir = g_rf_version_dir ? g_rf_version_dir : "/data/harmony";
    char p[256];
    snprintf(p, sizeof(p), "%s/remote_ui.version", vdir);
    FILE *f = fopen(p, "r");
    if (!f) {
        snprintf(p, sizeof(p), "%s/VERSION", vdir);
        f = fopen(p, "r");
    }
    if (f) {
        if (fgets(out_ver, sz, f)) {
            char *nl = strchr(out_ver, '\r'); if (nl) *nl = '\0';
            nl = strchr(out_ver, '\n'); if (nl) *nl = '\0';
            while (out_ver[0] == 'v' || out_ver[0] == 'V') memmove(out_ver, out_ver + 1, strlen(out_ver));
        }
        fclose(f);
    }
    if (!out_ver[0]) {
        strncpy(out_ver, "2.0.0", sz - 1);
    }
}

bool rf_ota_verify_manifest(const uint8_t *digest, char *err_buf, size_t err_sz) {
    const char *manifest_path = "/tmp/remote_manifest.json";
    if (err_buf && err_sz > 0) {
        snprintf(err_buf, err_sz, "Remote OTA manifest missing or invalid");
    }

    if (access(manifest_path, R_OK) != 0) {
        return false;
    }

    FILE *mf = fopen(manifest_path, "r");
    if (!mf) {
        unlink(manifest_path);
        return false;
    }

    fseek(mf, 0, SEEK_END);
    long msz = ftell(mf);
    fseek(mf, 0, SEEK_SET);
    if (msz <= 0 || msz >= 200000) {
        fclose(mf);
        unlink(manifest_path);
        return false;
    }

    char *mbuf = malloc(msz + 1);
    if (!mbuf) {
        fclose(mf);
        unlink(manifest_path);
        return false;
    }

    size_t n = fread(mbuf, 1, msz, mf);
    mbuf[n] = '\0';
    fclose(mf);

    cJSON *mroot = cJSON_Parse(mbuf);
    free(mbuf);
    if (!mroot) {
        unlink(manifest_path);
        return false;
    }

    bool ok = false;
    cJSON *j_ver = cJSON_GetObjectItemCaseSensitive(mroot, "version");
    if (j_ver && cJSON_IsString(j_ver) && j_ver->valuestring) {
        const char *mver = j_ver->valuestring;
        while (*mver == 'v' || *mver == 'V') mver++;
        char hub_ver[64];
        get_hub_version(hub_ver, sizeof(hub_ver));
        if (strcmp(mver, hub_ver) != 0) {
            printf("[-] Remote OTA version mismatch: manifest %s vs Hub %s\n", mver, hub_ver);
            if (err_buf && err_sz > 0) {
                snprintf(err_buf, err_sz, "Remote OTA version mismatch against Hub");
            }
            cJSON_Delete(mroot);
            unlink(manifest_path);
            return false;
        }

        cJSON *files = cJSON_GetObjectItemCaseSensitive(mroot, "files");
        if (files && cJSON_IsObject(files)) {
            cJSON *file_obj = cJSON_GetObjectItemCaseSensitive(files, "bin/arm/remote_ui.ota.gz");
            if (!file_obj) file_obj = cJSON_GetObjectItemCaseSensitive(files, "bin/arm/elite_ui.ota.gz");
            if (file_obj && cJSON_IsObject(file_obj)) {
                cJSON *j_sha = cJSON_GetObjectItemCaseSensitive(file_obj, "sha256");
                if (j_sha && cJSON_IsString(j_sha) && j_sha->valuestring) {
                    char computed_sha_hex[65];
                    for (int i = 0; i < 32; i++) {
                        snprintf(computed_sha_hex + (i * 2), 3, "%02x", digest[i]);
                    }
                    if (strcasecmp(j_sha->valuestring, computed_sha_hex) != 0) {
                        printf("[-] Remote OTA SHA-256 mismatch: manifest %s vs file %s\n",
                               j_sha->valuestring, computed_sha_hex);
                        if (err_buf && err_sz > 0) {
                            snprintf(err_buf, err_sz, "Remote OTA SHA-256 checksum mismatch");
                        }
                    } else {
                        ok = true;
                    }
                }
            }
        }
    }

    cJSON_Delete(mroot);
    unlink(manifest_path);
    return ok;
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
    g_rf_ota.in_progress = false;
    g_rf_ota.target_slot = target_slot;
    g_rf_ota.status_code = 3; /* downloading / preparing */
    snprintf(g_rf_ota.error_msg, sizeof(g_rf_ota.error_msg), "Preparing OTA update...");
    g_rf_ota.sent_bytes = 0;
    g_rf_ota.sent_chunks = 0;
    g_rf_ota.total_bytes = 0;
    g_rf_ota.total_chunks = 0;
    pthread_mutex_unlock(&g_ota_lock);

    const char *ota_path = "/tmp/remote_ui.ota.gz";
    const char *manifest_path = "/tmp/remote_manifest.json";

    /* Age limit: purge image if older than 2 hours to avoid stale RAM retention */
    struct stat ota_st;
    if (stat(ota_path, &ota_st) == 0) {
        if (time(NULL) - ota_st.st_mtime > 7200) {
            printf("[*] Remote OTA image in /tmp is older than 2 hours; purging stale cache\n");
            unlink(ota_path);
            unlink(manifest_path);
        }
    }

    bool was_staged = (access(ota_path, R_OK) == 0);

    FILE *f = fopen(ota_path, "rb");
    if (!f) {
        printf("[*] Remote OTA image not found in /tmp. Downloading to %s...\n", ota_path);
        char repo[256];
        get_ota_repo_source(repo, sizeof(repo));
        char fetch_cmd[512];
        snprintf(fetch_cmd, sizeof(fetch_cmd), "/data/harmony/update.sh fetch-remote-ota '%s' >/tmp/ota_fetch.log 2>&1", repo);
        int rc = -1;
        if (access("/data/harmony/update.sh", X_OK) == 0) {
            rc = system(fetch_cmd);
        }
        if (rc != 0 || access(ota_path, R_OK) != 0) {
            char wget_cmd[1024];
            snprintf(wget_cmd, sizeof(wget_cmd),
                     "/cache/bin/busybox wget -q -T 10 -O /tmp/remote_manifest.json '%s/manifest.json' 2>/dev/null; "
                     "/cache/bin/busybox wget -q -T 20 -O /tmp/remote_ui.ota.gz '%s/bin/arm/remote_ui.ota.gz' 2>/dev/null || "
                     "/cache/bin/busybox wget -q -T 20 -O /tmp/remote_ui.ota.gz '%s/bin/arm/elite_ui.ota.gz' 2>/dev/null",
                     repo, repo, repo);
            system(wget_cmd);
        }
        f = fopen(ota_path, "rb");
    }
    if (!f) {
        char err[128];
        snprintf(err, sizeof(err), "Failed to download OTA image to /tmp");
        ota_set_status(-1, false, err);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > (long)RF_OTA_MAX_BYTES) {
        fclose(f);
        unlink(ota_path);
        unlink(manifest_path);
        char err[128];
        snprintf(err, sizeof(err), "Invalid OTA file size: %ld", file_size);
        ota_set_status(-1, false, err);
        return NULL;
    }

    uint32_t tot_bytes = (uint32_t)file_size;
    /* OTA_START carries the stop-and-wait chunk count; a windowed remote derives its own */
    uint16_t tot_chunks = (uint16_t)((tot_bytes + RF_OTA_CHUNK_BYTES - 1) / RF_OTA_CHUNK_BYTES);

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

    /* Verify manifest:
     * - Required if worker downloaded the image itself (!was_staged)
     * - Required if a manifest sits next to the image in /tmp
     * - Skipped solely for staged builds where no manifest is present in /tmp
     */
    bool has_manifest = (access(manifest_path, R_OK) == 0);
    if (!was_staged || has_manifest) {
        char manifest_err[128];
        if (!rf_ota_verify_manifest(ota_digest, manifest_err, sizeof(manifest_err))) {
            fclose(f);
            unlink(ota_path);
            ota_set_status(-1, false, manifest_err);
            return NULL;
        }
    }

    pthread_mutex_lock(&g_ota_lock);
    g_rf_ota.in_progress = true;
    g_rf_ota.status_code = 1;
    g_rf_ota.total_bytes = tot_bytes;
    g_rf_ota.total_chunks = tot_chunks;
    g_rf_ota.error_msg[0] = '\0';
    pthread_mutex_unlock(&g_ota_lock);

    printf("[*] Starting RF OTA to slot %d: %u bytes, %u chunks from %s\n",
           target_slot, tot_bytes, tot_chunks, ota_path);

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

    /* Transfer power mode for the whole update, as Logitech's firmware does (restored by ota_set_status) */
    rf_set_power_mode(RF_PM_TRANSFER);
    /* A remote with its screen off would only notice the update when someone wakes it */
    /* 4 attempts (about 3 s) cover a light-standby remote; a remote without RF_MSG_WAKE never
     * answers, so more attempts only delay its update */
    int woke = rf_wake_remote(target_slot, 4);
    if (woke)
        printf("[*] Remote in slot %d acknowledged the wake request (attempt %d)\n", target_slot, woke);
    else
        printf("[*] Remote in slot %d did not acknowledge a wake request (older remote or asleep)\n", target_slot);

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

    /* 2. Windowed transfer (RF_OTA_DATA frames) if the remote supports it, else stop-and-wait
     * RF_MSG_OTA_CHUNK frames. Each has its own chunk size and resume count in the OTA_START ACK. */
    uint16_t c_idx = 0;
    uint32_t sent_bytes = 0;
    bool windowed = (g_ack_payload[4] == RF_OTA_WINDOWED);
    if (windowed) {
        uint32_t tot_data = (tot_bytes + RF_OTA_DATA_BYTES - 1) / RF_OTA_DATA_BYTES;
        uint32_t resume = (uint32_t)g_ack_payload[6] | ((uint32_t)g_ack_payload[7] << 8);
        if (resume >= tot_data) resume = 0;
        pthread_mutex_lock(&g_ota_lock);
        g_rf_ota.total_chunks = (uint16_t)tot_data;
        pthread_mutex_unlock(&g_ota_lock);
        printf("[*] Remote supports the windowed OTA transfer: %u chunks of %d bytes%s\n", tot_data,
               RF_OTA_DATA_BYTES, resume ? ", resuming" : "");
        if (resume) {
            ota_set_progress(resume * RF_OTA_DATA_BYTES, (uint16_t)resume);
            printf("[+] Remote requested resume from chunk %u (%u / %u bytes, %u%%)\n", resume,
                   resume * RF_OTA_DATA_BYTES, tot_bytes, (uint32_t)((resume * RF_OTA_DATA_BYTES * 100ULL) / tot_bytes));
        }
        if (!ota_send_windowed(f, target_slot, resume, tot_data, tot_bytes)) {
            fclose(f);
            return NULL;
        }
        c_idx = tot_chunks;
    } else {
        uint16_t resume_chunk = (uint16_t)g_ack_payload[2] | ((uint16_t)g_ack_payload[3] << 8);
        if (resume_chunk > 0 && resume_chunk < tot_chunks) {
            c_idx = resume_chunk;
            sent_bytes = (uint32_t)resume_chunk * RF_OTA_CHUNK_BYTES;
            fseek(f, (long)sent_bytes, SEEK_SET);
            ota_set_progress(sent_bytes, c_idx);
            printf("[+] Remote requested resume from chunk %u (%u / %u bytes, %u%%)\n",
                   c_idx, sent_bytes, tot_bytes, (uint32_t)((sent_bytes * 100ULL) / tot_bytes));
        }
    }
    while (c_idx < tot_chunks && g_running) {
        uint8_t chunk_data[RF_OTA_CHUNK_BYTES];
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
            unlink(ota_path);
            snprintf(err, sizeof(err), "Remote rejected OTA: SHA-256 integrity check failed");
        } else {
            snprintf(err, sizeof(err), "Remote verification failed (status 0x%02X)", g_ack_status);
        }
        ota_set_status(-1, false, err);
        return NULL;
    }

    unlink(ota_path);
    printf("[+] RF OTA completed successfully to slot %d!\n", target_slot);
    ota_set_status(2, false, NULL);
    return NULL;
}
