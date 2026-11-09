#include "remote_rf_ota.h"
#include "remote_fb.h"
#include "remote_rf.h"
#include "remote_power.h"
#include "remote_ui.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <time.h>
#include <errno.h>

#define OTA_MAX_CHUNKS 65536 /* every 16-bit chunk index; OTA_START rejects larger images */
#define OTA_BITMAP_SIZE (OTA_MAX_CHUNKS / 8)
static uint8_t s_ota_bitmap[OTA_BITMAP_SIZE];
static uint32_t s_current_file_crc = 0;

typedef struct {
    uint32_t total_bytes;
    uint16_t total_chunks;
    uint32_t file_crc;
    uint16_t highest_contiguous_chunk;
    uint16_t chunk_bytes; /* RF_OTA_DATA_BYTES (windowed) or RF_OTA_CHUNK_BYTES (stop-and-wait) */
} ota_meta_t;

/* Chunk size of the transfer in progress (0 until the first data frame tells which kind) */
static uint16_t s_chunk_bytes;

static inline uint16_t ota_chunks_for(uint32_t tot_b, uint16_t chunk_bytes) {
    return (uint16_t)((tot_b + chunk_bytes - 1) / chunk_bytes);
}

static inline bool ota_is_chunk_received(uint32_t c_idx) {
    if (c_idx >= OTA_MAX_CHUNKS) return false;
    return (s_ota_bitmap[c_idx / 8] & (1 << (c_idx % 8))) != 0;
}

static inline void ota_mark_chunk_received(uint32_t c_idx) {
    if (c_idx >= OTA_MAX_CHUNKS) return;
    s_ota_bitmap[c_idx / 8] |= (1 << (c_idx % 8));
}

/* Every chunk below s_contig_hint is received (reset at OTA_START): each chunk ACK asks for the
 * contiguous count, so the scan continues from there instead of from chunk 0 */
static uint16_t s_contig_hint;
static uint16_t ota_get_highest_contiguous_chunk(uint16_t total_chunks) {
    for (uint32_t i = s_contig_hint; i < total_chunks && i < OTA_MAX_CHUNKS; i++) {
        if (!ota_is_chunk_received(i)) {
            s_contig_hint = i;
            return i;
        }
    }
    s_contig_hint = total_chunks;
    return total_chunks;
}

static void ota_save_meta(uint32_t tot_b, uint16_t tot_c, uint32_t crc, uint16_t contig_chunk) {
    FILE *fm = fopen("/tmp/remote_ui.ota.meta", "wb");
    if (!fm) return;
    ota_meta_t m = { tot_b, tot_c, crc, contig_chunk, s_chunk_bytes };
    fwrite(&m, sizeof(m), 1, fm);
    size_t bm_len = (tot_c + 7) / 8;
    if (bm_len > OTA_BITMAP_SIZE) bm_len = OTA_BITMAP_SIZE;
    fwrite(s_ota_bitmap, 1, bm_len, fm);
    fclose(fm);
}

/* Loads a checkpoint of the same image; on success s_chunk_bytes is the checkpoint's chunk size
 * and *out_contig the first chunk missing, in those units */
static bool ota_load_meta(uint32_t tot_b, uint32_t crc, uint16_t *out_contig) {
    FILE *fm = fopen("/tmp/remote_ui.ota.meta", "rb");
    if (!fm) return false;
    ota_meta_t m;
    if (fread(&m, sizeof(m), 1, fm) != 1) {
        fclose(fm);
        return false;
    }
    if (m.chunk_bytes != RF_OTA_DATA_BYTES && m.chunk_bytes != RF_OTA_CHUNK_BYTES) {
        fclose(fm);
        return false;
    }
    uint16_t tot_c = ota_chunks_for(tot_b, m.chunk_bytes);
    if (m.total_bytes != tot_b || m.total_chunks != tot_c || (crc != 0 && m.file_crc != crc)) {
        fclose(fm);
        return false;
    }
    size_t bm_len = (tot_c + 7) / 8;
    if (bm_len > OTA_BITMAP_SIZE || fread(s_ota_bitmap, 1, bm_len, fm) != bm_len) {
        fclose(fm);
        return false;
    }
    fclose(fm);
    s_chunk_bytes = m.chunk_bytes;
    if (out_contig) *out_contig = ota_get_highest_contiguous_chunk(tot_c);
    return true;
}

void render_ota_overlay(void) {
    /* Header */
    fb_fill_rect(0, 0, FB_WIDTH, 34, RGB_HEADER);
    fb_draw_text_aa(14, 9, tr(STR_OTA_TITLE), RGB_YELLOW, &g_font_title);
    fb_fill_rect(0, 34, FB_WIDTH, 1, RGB_DIVIDER);

    int card_y = 52;

    /* Title */
    fb_draw_text_aa(8, card_y + 18, tr(STR_UPDATING_FIRMWARE), RGB_WHITE, &g_font_title);
    fb_draw_text_aa(8, card_y + 40, tr(STR_RECEIVING_OVER_RF), RGB_TEXT_MUTED, &g_font_main);

    /* Progress Stats */
    char prog_str[48];
    int pct = (g_ota_total_bytes > 0) ? (int)((g_ota_received_bytes * 100ULL) / g_ota_total_bytes) : 0;
    if (pct > 100) pct = 100;
    snprintf(prog_str, sizeof(prog_str), "Progress: %d%% (%u / %u KB)",
             pct, g_ota_received_bytes / 1024, g_ota_total_bytes / 1024);
    fb_draw_text_aa(8, card_y + 72, prog_str, RGB_YELLOW, &g_font_main);

    char chunk_str[48];
    snprintf(chunk_str, sizeof(chunk_str), "Chunks: %u / %u",
             g_ota_received_chunks, g_ota_total_chunks);
    fb_draw_text_aa(8, card_y + 96, chunk_str, RGB_TEXT_MUTED, &g_font_main);

    /* Progress Bar */
    int bar_x = 8;
    int max_w = FB_WIDTH - 16;
    int bar_w = (int)((pct * max_w) / 100);
    fb_fill_rect(bar_x, card_y + 124, max_w, 10, RGB_BG);
    fb_draw_rect(bar_x, card_y + 124, max_w, 10, g_theme.pal.outline);
    if (bar_w > 0) {
        fb_fill_rect(bar_x, card_y + 124, bar_w, 10, RGB_YELLOW);
    }

    fb_draw_text_aa(8, card_y + 148, tr(STR_DO_NOT_POWER_OFF), RGB_RED, &g_font_main);
    fb_draw_text_aa(8, card_y + 172, tr(STR_KEEP_CLOSE_TO_HUB), RGB_TEXT_MUTED, &g_font_main);

    /* Lock banner / stall indication */
    fb_fill_rect(0, FB_HEIGHT - 30, FB_WIDTH, 30, RGB_HEADER);
    uint64_t quiet_ms = (g_ota_last_packet_ms > 0) ? ms_since(get_monotonic_ms(), g_ota_last_packet_ms) : 0;
    if (quiet_ms > 4000) {
        fb_draw_text_aa(18, FB_HEIGHT - 22, "Waiting for Hub...", RGB_YELLOW, &g_font_main);
    } else {
        fb_draw_text_aa(18, FB_HEIGHT - 22, tr(STR_UPDATE_IN_PROGRESS), RGB_YELLOW, &g_font_main);
    }
}

static void ota_write_power_mode(uint8_t mode) {
    if (g_rf_fd < 0) return;
    uint8_t cmd[7] = {0x20, mode, 0x00, 0x00, 0x00, 0x00, 0x00};
    pthread_mutex_lock(&g_rf_write_mutex);
    if (write(g_rf_fd, cmd, sizeof(cmd)) < 0) {}
    pthread_mutex_unlock(&g_rf_write_mutex);
    g_rf_pm = mode;
    printf("[*] CC2544 power mode 0x%02X\n", mode);
}

/* Logitech's firmware keeps the radio in RF_PM_TRANSFER for a whole HOT transfer: in that mode the
 * remote's CC2544 fetches the Hub's queued frames by itself (about 250 a second on a Harmony
 * Ultimate), while in RF_PM_ACTIVE it gets one only in the acknowledgement of a frame it sent. */
void ota_set_updating(bool on) {
    g_is_ota_updating = on;
    ota_write_power_mode(on ? RF_PM_TRANSFER : RF_PM_ACTIVE);
}

/* First data frame of a transfer: settle the chunk size. A checkpoint made with the other kind of
 * frames cannot be continued, so the image is restarted from chunk 0. */
static bool ota_begin_chunks(uint16_t chunk_bytes) {
    if (s_chunk_bytes == chunk_bytes) return true;
    if (s_chunk_bytes != 0) {
        printf("[*] RF OTA: %u-byte chunks after a checkpoint of %u-byte chunks, starting over\n", chunk_bytes, s_chunk_bytes);
        if (g_ota_file) fclose(g_ota_file);
        g_ota_file = fopen("/tmp/remote_ui.ota.gz", "wb");
        if (!g_ota_file) return false;
        unlink("/tmp/remote_ui.ota.meta");
        memset(s_ota_bitmap, 0, sizeof(s_ota_bitmap));
        g_ota_received_chunks = 0;
        g_ota_received_bytes = 0;
    }
    s_chunk_bytes = chunk_bytes;
    s_contig_hint = 0;
    g_ota_total_chunks = ota_chunks_for(g_ota_total_bytes, chunk_bytes);
    return true;
}

static void ota_send_chunk_ack(uint8_t seq) {
    /* The contiguous count lets a windowed Hub resend from the first missing chunk */
    uint16_t contig = ota_get_highest_contiguous_chunk(g_ota_total_chunks);
    rf_frame_t ack;
    rf_build_ack(&ack, g_remote_slot, seq, 0x00);
    ack.payload[2] = (uint8_t)(contig & 0xFF);
    ack.payload[3] = (uint8_t)((contig >> 8) & 0xFF);
    ack.payload[4] = RF_OTA_WINDOWED;
    rf_send_frame(&ack);

    /* Resume checkpoint, at most twice a second (a windowed Hub asks for an ACK about 15 times
     * a second) and once complete */
    static uint64_t s_last_meta_ms = 0;
    uint64_t t_meta = get_monotonic_ms();
    if (ms_since(t_meta, s_last_meta_ms) >= 500 || contig == g_ota_total_chunks) {
        s_last_meta_ms = t_meta;
        ota_save_meta(g_ota_total_bytes, g_ota_total_chunks, s_current_file_crc, contig);
    }
}

static void ota_mark_progress_render(void) {
    static uint64_t last_ota_render_ms = 0;
    uint64_t now_ms = get_monotonic_ms();
    if (now_ms - last_ota_render_ms >= 500) {
        last_ota_render_ms = now_ms;
        g_screen_dirty = true;
    }
}

/* RF_OTA_DATA frame as read from /dev/rfspi: 20 <pipe> 12 <slot> <0x60|idx[3:0]|ack> <idx[11:4]> <data[26]> */
void handle_ota_data_frame(const uint8_t *pkt) {
    if (!g_is_ota_updating || !g_ota_file) return;
    if (!ota_begin_chunks(RF_OTA_DATA_BYTES)) return;
    g_ota_last_packet_ms = get_monotonic_ms();
    g_last_activity_time = time(NULL);

    /* 12-bit index, completed from the chunk expected next (at most 64 in flight either way) */
    uint32_t idx12 = (uint32_t)(pkt[4] & 0x0F) | ((uint32_t)pkt[5] << 4);
    uint32_t expect = s_contig_hint;
    uint32_t idx = (expect & ~0xFFFu) | idx12;
    if (idx + 2048 < expect) idx += 0x1000;
    else if (idx > expect + 2048 && idx >= 0x1000) idx -= 0x1000;
    if (idx >= g_ota_total_chunks) return;

    uint32_t off = idx * RF_OTA_DATA_BYTES;
    uint32_t d_len = g_ota_total_bytes - off;
    if (d_len > RF_OTA_DATA_BYTES) d_len = RF_OTA_DATA_BYTES;
    fseek(g_ota_file, (long)off, SEEK_SET);
    fwrite(&pkt[6], 1, d_len, g_ota_file);
    if (!ota_is_chunk_received((uint16_t)idx)) {
        ota_mark_chunk_received((uint16_t)idx);
        g_ota_received_bytes += d_len;
        g_ota_received_chunks++;
    }
    if (pkt[4] & RF_OTA_HDR_ACK) ota_send_chunk_ack(pkt[4] & 0x0F);
    ota_mark_progress_render();
}

bool handle_ota_frame(const rf_frame_t *f) {
    switch (f->msg_type) {
        case RF_MSG_OTA_START: {
            read_remote_battery();
            uint32_t tot_b = (uint32_t)f->payload[0] | ((uint32_t)f->payload[1] << 8) |
                             ((uint32_t)f->payload[2] << 16) | ((uint32_t)f->payload[3] << 24);
            uint16_t tot_c = (uint16_t)f->payload[4] | ((uint16_t)f->payload[5] << 8);
            uint32_t file_crc = (uint32_t)f->payload[6] | ((uint32_t)f->payload[7] << 8) |
                                ((uint32_t)f->payload[8] << 16) | ((uint32_t)f->payload[9] << 24);
            s_current_file_crc = file_crc;
            s_contig_hint = 0;
            s_chunk_bytes = 0;

            printf("[*] RF OTA START: %u bytes, %u chunks, CRC32=0x%08X, batt=%d%% chg=%d\n",
                   tot_b, tot_c, file_crc, g_battery_info.percent, g_battery_info.charging);

            if (tot_b == 0 || tot_b > RF_OTA_MAX_BYTES) {
                printf("[-] RF OTA REJECTED: image size %u outside 1..%u bytes\n", tot_b, (unsigned)RF_OTA_MAX_BYTES);
                rf_frame_t ack;
                rf_build_ack(&ack, g_remote_slot, f->seq, 0x03);
                rf_send_frame(&ack);
                return true;
            }

            if (g_battery_info.percent < 40 && !g_battery_info.charging) {
                printf("[-] RF OTA REJECTED: Low battery (%d%%) and not charging!\n", g_battery_info.percent);
                rf_frame_t ack;
                rf_build_ack(&ack, g_remote_slot, f->seq, 0x02);
                rf_send_frame(&ack);
                return true;
            }

            if (g_ota_file) {
                fclose(g_ota_file);
                g_ota_file = NULL;
            }

            uint16_t resume_chunk = 0;
            bool can_resume = false;
            struct stat st;
            if (stat("/tmp/remote_ui.ota.gz", &st) == 0 && st.st_size > 0) {
                if (ota_load_meta(tot_b, file_crc, &resume_chunk)) {
                    uint16_t have_c = ota_chunks_for(tot_b, s_chunk_bytes);
                    if (resume_chunk > 0 && resume_chunk < have_c) {
                        g_ota_file = fopen("/tmp/remote_ui.ota.gz", "r+b");
                        if (g_ota_file) {
                            can_resume = true;
                            tot_c = have_c;
                            printf("[+] RF OTA resuming from chunk %u / %u (%u bytes, %u-byte chunks)\n",
                                   resume_chunk, tot_c, (uint32_t)resume_chunk * s_chunk_bytes, s_chunk_bytes);
                        }
                    }
                }
            }

            if (!can_resume) {
                resume_chunk = 0;
                s_chunk_bytes = 0;
                memset(s_ota_bitmap, 0, sizeof(s_ota_bitmap));
                unlink("/tmp/remote_ui.ota.meta");
                g_ota_file = fopen("/tmp/remote_ui.ota.gz", "wb");
                if (!g_ota_file) {
                    printf("[-] RF OTA REJECTED: Could not open /tmp/remote_ui.ota.gz\n");
                    rf_frame_t ack;
                    rf_build_ack(&ack, g_remote_slot, f->seq, 0x03);
                    rf_send_frame(&ack);
                    return true;
                }
            }

            ota_set_updating(true);
            g_last_activity_time = time(NULL);
            if (!g_screen_on) {
                fb_set_blank(false);
            }
            g_ota_start_ms = get_monotonic_ms();
            g_ota_last_packet_ms = g_ota_start_ms;
            g_ota_total_bytes = tot_b;
            g_ota_total_chunks = tot_c;
            g_ota_received_chunks = resume_chunk;
            g_ota_received_bytes = (uint32_t)resume_chunk * (can_resume ? s_chunk_bytes : 0);
            g_screen_dirty = true;

            /* The resume chunk goes in the field of its kind: payload[2..3] for a stop-and-wait
             * Hub (22-byte chunks), payload[6..7] for a windowed one (26-byte chunks) */
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            if (can_resume && s_chunk_bytes == RF_OTA_CHUNK_BYTES) {
                ack.payload[2] = (uint8_t)(resume_chunk & 0xFF);
                ack.payload[3] = (uint8_t)((resume_chunk >> 8) & 0xFF);
            } else if (can_resume && s_chunk_bytes == RF_OTA_DATA_BYTES) {
                ack.payload[6] = (uint8_t)(resume_chunk & 0xFF);
                ack.payload[7] = (uint8_t)((resume_chunk >> 8) & 0xFF);
            }
            ack.payload[4] = RF_OTA_WINDOWED;
            rf_send_frame(&ack);
            return true;
        }

        case RF_MSG_OTA_CHUNK: {
            if (!g_is_ota_updating || !g_ota_file) return true;
            if (!ota_begin_chunks(RF_OTA_CHUNK_BYTES)) return true;
            g_ota_last_packet_ms = get_monotonic_ms();
            g_last_activity_time = time(NULL);

            uint16_t c_idx = (uint16_t)f->payload[0] | ((uint16_t)f->payload[1] << 8);
            uint8_t d_len = f->payload[2];
            if (d_len > RF_OTA_CHUNK_BYTES) d_len = RF_OTA_CHUNK_BYTES;

            if (d_len > 0) {
                /* Seek to precise chunk offset for idempotency against retransmissions */
                fseek(g_ota_file, (long)c_idx * RF_OTA_CHUNK_BYTES, SEEK_SET);
                fwrite(&f->payload[3], 1, d_len, g_ota_file);

                bool already_had = ota_is_chunk_received(c_idx);
                if (!already_had) {
                    ota_mark_chunk_received(c_idx);
                    g_ota_received_bytes += d_len;
                    g_ota_received_chunks++;
                }
            }

            if (f->flags & RF_FLAG_ACK_REQ) ota_send_chunk_ack(f->seq);
            ota_mark_progress_render();
            return true;
        }

        case RF_MSG_OTA_END: {
            if (!g_is_ota_updating || !g_ota_file) return true;
            fflush(g_ota_file);
            fclose(g_ota_file);
            g_ota_file = NULL;

            /* Check that all chunks were actually received */
            uint16_t contig = ota_get_highest_contiguous_chunk(g_ota_total_chunks);
            if (contig < g_ota_total_chunks) {
                printf("[-] OTA END rejected: missing chunks (have up to %u of %u)\n", contig, g_ota_total_chunks);
                rf_frame_t ack;
                rf_build_ack(&ack, g_remote_slot, f->seq, 0x05);
                rf_send_frame(&ack);
                /* The file is closed: leave OTA mode (and the transfer power mode) now instead of
                 * after the 60 s timeout; the checkpoint stays, so the Hub can resume */
                ota_set_updating(false);
                g_screen_dirty = true;
                return true;
            }

            printf("[+] RF OTA END received! Received %u bytes (%u chunks). Verifying file from disk...\n",
                   g_ota_received_bytes, g_ota_received_chunks);

            /* Compute SHA-256 of the complete file on disk (tmpfs in RAM) */
            uint8_t actual_digest[32] = {0};
            FILE *vfile = fopen("/tmp/remote_ui.ota.gz", "rb");
            if (vfile) {
                sha256_ctx_t file_sha;
                sha256_init(&file_sha);
                uint8_t vbuf[4096];
                size_t vrd;
                while ((vrd = fread(vbuf, 1, sizeof(vbuf), vfile)) > 0) {
                    sha256_update(&file_sha, vbuf, vrd);
                }
                sha256_final(actual_digest, &file_sha);
                fclose(vfile);
            }

            /* Check integrity if Hub provided SHA-256 digest (type 0x01) */
            if (f->payload[0] == 0x01) {
                if (memcmp(&f->payload[1], actual_digest, 20) != 0) {
                    printf("[-] OTA SHA-256 digest mismatch! Corrupted transfer or untrusted payload.\n");
                    system("rm -rf /tmp/remote_ota_pkg /data/remote_ui.new /tmp/remote_ui.ota.gz /tmp/remote_ui.ota.meta 2>/dev/null || true");
                    rf_frame_t ack;
                    rf_build_ack(&ack, g_remote_slot, f->seq, 0x06);
                    rf_send_frame(&ack);
                    ota_set_updating(false);
                    g_screen_dirty = true;
                    return true;
                }
                printf("[+] OTA SHA-256 digest verified successfully!\n");
            }

            unlink("/tmp/remote_ui.ota.meta");

            system("rm -rf /tmp/remote_ota_pkg && mkdir -p /tmp/remote_ota_pkg");
            int tar_rc = system("tar -xzf /tmp/remote_ui.ota.gz -C /tmp/remote_ota_pkg 2>/dev/null || "
                                "/data/busybox tar -xzf /tmp/remote_ui.ota.gz -C /tmp/remote_ota_pkg 2>/dev/null || "
                                "/bin/busybox tar -xzf /tmp/remote_ui.ota.gz -C /tmp/remote_ota_pkg 2>/dev/null");

            bool is_pkg = false;
            char bin_path[128] = {0};

            if (tar_rc == 0) {
                /* remote_ui; elite_ui in packages built before the elite -> remote rename */
                static const char *const k_pkg_bins[] = {
                    "/tmp/remote_ota_pkg/remote_ui", "/tmp/remote_ota_pkg/bin/remote_ui",
                    "/tmp/remote_ota_pkg/elite_ui", "/tmp/remote_ota_pkg/bin/elite_ui", NULL };
                for (int bi = 0; k_pkg_bins[bi]; bi++) {
                    if (access(k_pkg_bins[bi], F_OK) == 0) {
                        snprintf(bin_path, sizeof(bin_path), "%s", k_pkg_bins[bi]);
                        is_pkg = true;
                        break;
                    }
                }
            }

            if (!is_pkg) {
                system("gzip -d -c /tmp/remote_ui.ota.gz > /data/remote_ui.new 2>/dev/null || cat /tmp/remote_ui.ota.gz > /data/remote_ui.new");
                strcpy(bin_path, "/data/remote_ui.new");
            }
            system("chmod +x /tmp/remote_ota_pkg/* /data/remote_ui.new 2>/dev/null || true");

            bool valid_elf = false;
            FILE *felf = fopen(bin_path, "rb");
            if (felf) {
                uint8_t hdr[4] = {0};
                if (fread(hdr, 1, 4, felf) == 4) {
                    if (hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
                        valid_elf = true;
                    }
                }
                fclose(felf);
            }

            rf_frame_t ack;
            if (valid_elf) {
                printf("[+] ELF header verified! Replacing binary and installing scripts...\n");
                system("cp -f /data/remote_ui /data/remote_ui.bak 2>/dev/null");

                if (is_pkg) {
                    if (access("/tmp/remote_ota_pkg/rcS.local", F_OK) == 0) {
                        system("cp -f /tmp/remote_ota_pkg/rcS.local /etc/init.d/rcS.local && chmod 755 /etc/init.d/rcS.local");
                        system("cp -f /tmp/remote_ota_pkg/rcS.local /data/rcS.local 2>/dev/null && chmod 755 /data/rcS.local 2>/dev/null || true");
                    }
                    if (access("/tmp/remote_ota_pkg/remote_watchdog.sh", F_OK) == 0) {
                        system("cp -f /tmp/remote_ota_pkg/remote_watchdog.sh /data/remote_watchdog.sh && chmod 755 /data/remote_watchdog.sh");
                    }
                    if (access("/tmp/remote_ota_pkg/factory_reset.sh", F_OK) == 0) {
                        system("cp -f /tmp/remote_ota_pkg/factory_reset.sh /data/factory_reset.sh && chmod 755 /data/factory_reset.sh");
                    }
                    system("for s in /tmp/remote_ota_pkg/*.sh; do [ -f \"$s\" ] && cp -f \"$s\" /data/ && chmod 755 \"/data/${s##*/}\"; done 2>/dev/null || true");
                    /* Built-in themes replace their older versions; other themes stay.
                       No basename: the stock BusyBox has none, and an empty name made
                       older builds wipe /data/themes and leave the last theme's files
                       at its top level; those stray files are removed here. */
                    system("mkdir -p /data/themes && rm -f /data/themes/* 2>/dev/null; for t in /tmp/remote_ota_pkg/themes/*; do [ -d \"$t\" ] && rm -rf \"/data/themes/${t##*/}\" && cp -r \"$t\" /data/themes/; done 2>/dev/null || true");
                    if (access("/tmp/remote_ota_pkg/remote_sync", F_OK) == 0) {
                        system("cp -f /tmp/remote_ota_pkg/remote_sync /data/remote_sync && chmod 755 /data/remote_sync");
                    }
                    char cp_cmd[384];
                    snprintf(cp_cmd, sizeof(cp_cmd), "mv -f %s /data/remote_ui && chmod 755 /data/remote_ui", bin_path);
                    system(cp_cmd);
                } else {
                    system("mv -f /data/remote_ui.new /data/remote_ui && chmod 755 /data/remote_ui");
                }

                system("rm -rf /tmp/remote_ota_pkg /tmp/remote_ui.ota.gz /data/remote_ui.new 2>/dev/null || true");
                /* /data/elite_ui stays a link to the new binary for supervisors of older builds */
                system("[ -L /data/elite_ui ] || rm -f /data/elite_ui; ln -sf remote_ui /data/elite_ui 2>/dev/null || true");

                rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
                rf_send_frame(&ack);

                ota_set_updating(false);
                render_screen();
                if (g_rf_fd >= 0) {
                    close(g_rf_fd);
                    g_rf_fd = -1;
                }
                read_remote_battery();
                if (!is_remote_docked()) {
                    /* On battery a reboot switches the remote off (the SoC reset makes the PMIC
                     * cut power; only the Off key turns it back on), as Settings > Reboot already
                     * knows: start the new binary in place instead. Scripts and themes are
                     * installed already; a changed rcS.local applies at the next boot. */
                    printf("[*] RF OTA installed on battery: restarting remote_ui instead of rebooting\n");
                    fflush(stdout);
                    system("/data/remote_sync 2>/dev/null || /data/sync 2>/dev/null || sync 2>/dev/null || true");
                    char *const args[] = { "/data/remote_ui", NULL };
                    execv("/data/remote_ui", args);
                    exit(143); /* execv failed: the supervisor in init.sh starts it again */
                }
                system("touch /tmp/remote_stop 2>/dev/null || true; "
                       "if [ -x /data/safe_reboot.sh ]; then (/data/safe_reboot.sh >/dev/null 2>&1 &); "
                       "else killall -9 init.sh remote_watchdog.sh 2>/dev/null || true; "
                       "sleep 1; "
                       "mount -o remount,ro /cache 2>/dev/null || true; "
                       "mount -o remount,ro /mnt/data 2>/dev/null || true; "
                       "mount -o remount,ro / 2>/dev/null || true; "
                       "/data/remote_sync 2>/dev/null || sync 2>/dev/null || true; "
                       "sleep 1; "
                       "(/sbin/reboot -f 2>/dev/null || reboot -f 2>/dev/null || (sleep 1 && echo b > /proc/sysrq-trigger)) & fi");
                exit(0);
            } else {
                printf("[-] OTA ELF verification failed! Corrupted transfer.\n");
                system("rm -rf /tmp/remote_ota_pkg /data/remote_ui.new /tmp/remote_ui.ota.gz 2>/dev/null || true");
                rf_build_ack(&ack, g_remote_slot, f->seq, 0x05);
                rf_send_frame(&ack);
                ota_set_updating(false);
                g_screen_dirty = true;
            }
            return true;
        }

        default:
            return false;
    }
}

bool handle_icon_frame(const rf_frame_t *f) {
    switch (f->msg_type) {
        case RF_MSG_ICON_START: {
            g_icon_rx_type = f->payload[0];
            g_icon_rx_tot_bytes = (uint32_t)f->payload[1] | ((uint32_t)f->payload[2] << 8);
            uint16_t tot_c = (uint16_t)f->payload[3] | ((uint16_t)f->payload[4] << 8);
            memset(g_icon_rx_id, 0, sizeof(g_icon_rx_id));
            strncpy(g_icon_rx_id, (const char *)&f->payload[5], sizeof(g_icon_rx_id) - 1);
            g_icon_rx_bytes = 0;
            g_icon_rx_chunks = 0;

            printf("[*] Remote RF ICON_START: id='%s' type=%d, %u bytes, %u chunks\n",
                   g_icon_rx_id, g_icon_rx_type, g_icon_rx_tot_bytes, tot_c);

            char exist_path[128];
            snprintf(exist_path, sizeof(exist_path), "/data/assets/custom/%s.png", g_icon_rx_id);
            struct stat st;
            if (stat(exist_path, &st) == 0 && st.st_size > 0 && (uint32_t)st.st_size == g_icon_rx_tot_bytes) {
                printf("[*] Custom icon %s already exists on remote (%u bytes) -> skip stream\n",
                       g_icon_rx_id, (unsigned int)st.st_size);
                rf_frame_t ack;
                rf_build_ack(&ack, g_remote_slot, f->seq, RF_ICON_STATUS_ALREADY_EXISTS);
                rf_send_frame(&ack);
                return true;
            }

            if (g_icon_rx_file) {
                fclose(g_icon_rx_file);
                g_icon_rx_file = NULL;
            }
            g_icon_rx_file = fopen("/tmp/custom_icon_rx.tmp", "wb");
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, g_icon_rx_file ? RF_ICON_STATUS_OK : 0x03);
            rf_send_frame(&ack);
            return true;
        }

        case RF_MSG_ICON_CHUNK: {
            if (!g_icon_rx_file) return true;
            uint16_t c_idx = (uint16_t)f->payload[0] | ((uint16_t)f->payload[1] << 8);
            uint8_t d_len = f->payload[2];
            if (d_len > 22) d_len = 22;

            if (d_len > 0) {
                fwrite(&f->payload[3], 1, d_len, g_icon_rx_file);
                g_icon_rx_bytes += d_len;
                g_icon_rx_chunks++;
            }

            if ((f->flags & RF_FLAG_ACK_REQ) || (c_idx % 4 == 0)) {
                rf_frame_t ack;
                rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
                rf_send_frame(&ack);
            }
            return true;
        }

        case RF_MSG_ICON_END: {
            if (!g_icon_rx_file) return true;
            fclose(g_icon_rx_file);
            g_icon_rx_file = NULL;

            uint16_t expected_crc = (uint16_t)f->payload[0] | ((uint16_t)f->payload[1] << 8);

            /* Verify file integrity via CRC16 */
            FILE *v = fopen("/tmp/custom_icon_rx.tmp", "rb");
            uint8_t ack_status = 0x01;
            if (v) {
                fseek(v, 0, SEEK_END);
                long sz = ftell(v);
                fseek(v, 0, SEEK_SET);
                if (sz > 0 && sz <= 16384) {
                    uint8_t *tmp_buf = (uint8_t *)malloc((size_t)sz);
                    if (tmp_buf) {
                        if (fread(tmp_buf, 1, (size_t)sz, v) == (size_t)sz) {
                            uint16_t actual_crc = rf_crc16(tmp_buf, (size_t)sz);
                            if (actual_crc == expected_crc) {
                                ack_status = 0x00;
                            } else {
                                printf("[-] Icon CRC mismatch: expected 0x%04X, got 0x%04X\n",
                                       expected_crc, actual_crc);
                            }
                        }
                        free(tmp_buf);
                    }
                }
                fclose(v);
            }

            if (ack_status == 0x00) {
                mkdir("/data/assets", 0755);
                mkdir("/data/assets/custom", 0755);
                const char *t_prefix = (g_icon_rx_type == 1) ? "device" : (g_icon_rx_type == 2 ? "button" : "activity");
                char dst_path[128];
                snprintf(dst_path, sizeof(dst_path), "/data/assets/custom/%s_%s.png", t_prefix, g_icon_rx_id);

                char cmd[300];
                snprintf(cmd, sizeof(cmd), "cp -f /tmp/custom_icon_rx.tmp %s && cp -f /tmp/custom_icon_rx.tmp /data/assets/custom/%s.png",
                         dst_path, g_icon_rx_id);
                system(cmd);
                unlink("/tmp/custom_icon_rx.tmp");

                fb_invalidate_png_cache(g_icon_rx_id);

                for (int i = 0; i < g_activity_count; i++) {
                    if (strcmp(g_activities[i].id, g_icon_rx_id) == 0 ||
                        (i < g_rf_activity_count && g_rf_activities[i].icon_id == (uint16_t)atoi(g_icon_rx_id))) {
                        char cpath[128];
                        snprintf(cpath, sizeof(cpath), "/data/assets/custom/%s.png", g_icon_rx_id);
                        copy_str(g_activities[i].icon_path, sizeof(g_activities[i].icon_path), cpath);
                    }
                }
                g_screen_dirty = true;
                printf("[+] Custom Icon saved to %s and active screen dirtied!\n", dst_path);
            }

            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, ack_status);
            rf_send_frame(&ack);
            return true;
        }

        default:
            return false;
    }
}
