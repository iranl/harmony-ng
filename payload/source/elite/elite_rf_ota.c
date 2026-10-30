#include "elite_rf_ota.h"
#include "elite_fb.h"
#include "elite_rf.h"
#include "elite_power.h"
#include "elite_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <time.h>
#include <errno.h>

void render_ota_overlay(void) {
    /* Header */
    fb_fill_rect(0, 0, FB_WIDTH, 34, RGB_HEADER);
    fb_draw_text_aa(14, 9, "FIRMWARE OTA UPDATE", RGB_YELLOW, &g_font_title);
    fb_fill_rect(0, 34, FB_WIDTH, 1, RGB_DIVIDER);

    /* Center Card */
    int card_y = 52;
    int card_h = 216;
    fb_fill_rect(10, card_y, FB_WIDTH - 20, card_h, RGB_CARD);
    fb_draw_rect(10, card_y, FB_WIDTH - 20, card_h, RGB_YELLOW);

    /* Title */
    fb_draw_text_aa(22, card_y + 18, "UPDATING FIRMWARE", RGB_WHITE, &g_font_title);
    fb_draw_text_aa(22, card_y + 40, "Receiving firmware over RF", RGB_TEXT_MUTED, &g_font_main);

    /* Progress Stats */
    char prog_str[48];
    int pct = (g_ota_total_bytes > 0) ? (int)((g_ota_received_bytes * 100ULL) / g_ota_total_bytes) : 0;
    if (pct > 100) pct = 100;
    snprintf(prog_str, sizeof(prog_str), "Progress: %d%% (%u / %u KB)",
             pct, g_ota_received_bytes / 1024, g_ota_total_bytes / 1024);
    fb_draw_text_aa(22, card_y + 72, prog_str, RGB_YELLOW, &g_font_main);

    char chunk_str[48];
    snprintf(chunk_str, sizeof(chunk_str), "Chunks: %u / %u",
             g_ota_received_chunks, g_ota_total_chunks);
    fb_draw_text_aa(22, card_y + 96, chunk_str, RGB_TEXT_MUTED, &g_font_main);

    /* Progress Bar */
    int bar_x = 22;
    int max_w = FB_WIDTH - 44;
    int bar_w = (int)((pct * max_w) / 100);
    fb_fill_rect(bar_x, card_y + 124, max_w, 10, RGB_BG);
    fb_draw_rect(bar_x, card_y + 124, max_w, 10, 0x2945);
    if (bar_w > 0) {
        fb_fill_rect(bar_x, card_y + 124, bar_w, 10, RGB_YELLOW);
    }

    fb_draw_text_aa(22, card_y + 148, "Do not power off remote or dock!", RGB_RED, &g_font_main);
    fb_draw_text_aa(22, card_y + 172, "Keep remote close to Hub.", RGB_TEXT_MUTED, &g_font_main);

    /* Lock banner */
    fb_fill_rect(0, FB_HEIGHT - 30, FB_WIDTH, 30, RGB_HEADER);
    fb_draw_text_aa(18, FB_HEIGHT - 22, "Firmware update in progress", RGB_YELLOW, &g_font_main);
}

bool handle_ota_frame(const rf_frame_t *f) {
    switch (f->msg_type) {
        case RF_MSG_OTA_START: {
            read_elite_battery();
            uint32_t tot_b = (uint32_t)f->payload[0] | ((uint32_t)f->payload[1] << 8) |
                             ((uint32_t)f->payload[2] << 16) | ((uint32_t)f->payload[3] << 24);
            uint16_t tot_c = (uint16_t)f->payload[4] | ((uint16_t)f->payload[5] << 8);

            printf("[*] RF OTA START: %u bytes, %u chunks, batt=%d%% chg=%d\n",
                   tot_b, tot_c, g_battery_info.percent, g_battery_info.charging);

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
            g_ota_file = fopen("/tmp/elite_ui.ota.gz", "wb");
            if (!g_ota_file) {
                printf("[-] RF OTA REJECTED: Could not open /tmp/elite_ui.ota.gz\n");
                rf_frame_t ack;
                rf_build_ack(&ack, g_remote_slot, f->seq, 0x03);
                rf_send_frame(&ack);
                return true;
            }

            g_is_ota_updating = true;
            g_last_activity_time = time(NULL);
            if (!g_screen_on) {
                fb_set_blank(false);
            }
            g_ota_start_ms = get_monotonic_ms();
            g_ota_last_packet_ms = g_ota_start_ms;
            g_ota_total_bytes = tot_b;
            g_ota_received_bytes = 0;
            g_ota_total_chunks = tot_c;
            g_ota_received_chunks = 0;
            g_screen_dirty = true;

            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
            rf_send_frame(&ack);
            return true;
        }

        case RF_MSG_OTA_CHUNK: {
            if (!g_is_ota_updating || !g_ota_file) return true;
            g_ota_last_packet_ms = get_monotonic_ms();
            g_last_activity_time = time(NULL);

            uint16_t c_idx = (uint16_t)f->payload[0] | ((uint16_t)f->payload[1] << 8);
            uint8_t d_len = f->payload[2];
            if (d_len > 22) d_len = 22;

            if (d_len > 0) {
                fwrite(&f->payload[3], 1, d_len, g_ota_file);
                g_ota_received_bytes += d_len;
                g_ota_received_chunks++;
            }

            if ((f->flags & RF_FLAG_ACK_REQ) || (c_idx % 4 == 0)) {
                rf_frame_t ack;
                rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
                rf_send_frame(&ack);
            }
            static uint64_t last_ota_render_ms = 0;
            uint64_t now_ms = get_monotonic_ms();
            if (now_ms - last_ota_render_ms >= 500) {
                last_ota_render_ms = now_ms;
                g_screen_dirty = true;
            }
            return true;
        }

        case RF_MSG_OTA_END: {
            if (!g_is_ota_updating || !g_ota_file) return true;
            fclose(g_ota_file);
            g_ota_file = NULL;

            printf("[+] RF OTA END received! Received %u bytes (%u chunks). Verifying...\n",
                   g_ota_received_bytes, g_ota_received_chunks);

            system("rm -rf /tmp/elite_ota_pkg && mkdir -p /tmp/elite_ota_pkg");
            int tar_rc = system("tar -xzf /tmp/elite_ui.ota.gz -C /tmp/elite_ota_pkg 2>/dev/null || "
                                "/data/busybox tar -xzf /tmp/elite_ui.ota.gz -C /tmp/elite_ota_pkg 2>/dev/null || "
                                "/bin/busybox tar -xzf /tmp/elite_ui.ota.gz -C /tmp/elite_ota_pkg 2>/dev/null");

            bool is_pkg = false;
            char bin_path[128] = {0};

            if (tar_rc == 0) {
                if (access("/tmp/elite_ota_pkg/elite_ui", F_OK) == 0) {
                    strcpy(bin_path, "/tmp/elite_ota_pkg/elite_ui");
                    is_pkg = true;
                } else if (access("/tmp/elite_ota_pkg/bin/elite_ui", F_OK) == 0) {
                    strcpy(bin_path, "/tmp/elite_ota_pkg/bin/elite_ui");
                    is_pkg = true;
                }
            }

            if (!is_pkg) {
                system("gzip -d -c /tmp/elite_ui.ota.gz > /data/elite_ui.new 2>/dev/null || cat /tmp/elite_ui.ota.gz > /data/elite_ui.new");
                strcpy(bin_path, "/data/elite_ui.new");
            }
            system("chmod +x /tmp/elite_ota_pkg/* /data/elite_ui.new 2>/dev/null || true");

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
                system("cp -f /data/elite_ui /data/elite_ui.bak 2>/dev/null");

                if (is_pkg) {
                    if (access("/tmp/elite_ota_pkg/rcS.local", F_OK) == 0) {
                        system("cp -f /tmp/elite_ota_pkg/rcS.local /etc/init.d/rcS.local && chmod 755 /etc/init.d/rcS.local");
                        system("cp -f /tmp/elite_ota_pkg/rcS.local /data/rcS.local 2>/dev/null && chmod 755 /data/rcS.local 2>/dev/null || true");
                    }
                    if (access("/tmp/elite_ota_pkg/elite_watchdog.sh", F_OK) == 0) {
                        system("cp -f /tmp/elite_ota_pkg/elite_watchdog.sh /data/elite_watchdog.sh && chmod 755 /data/elite_watchdog.sh");
                    }
                    if (access("/tmp/elite_ota_pkg/factory_reset.sh", F_OK) == 0) {
                        system("cp -f /tmp/elite_ota_pkg/factory_reset.sh /data/factory_reset.sh && chmod 755 /data/factory_reset.sh");
                    }
                    system("for s in /tmp/elite_ota_pkg/*.sh; do [ -f \"$s\" ] && cp -f \"$s\" /data/ && chmod 755 \"/data/$(basename \"$s\")\"; done 2>/dev/null || true");
                    char cp_cmd[384];
                    snprintf(cp_cmd, sizeof(cp_cmd), "mv -f %s /data/elite_ui && chmod 755 /data/elite_ui", bin_path);
                    system(cp_cmd);
                } else {
                    system("mv -f /data/elite_ui.new /data/elite_ui && chmod 755 /data/elite_ui");
                }

                system("rm -rf /tmp/elite_ota_pkg /tmp/elite_ui.ota.gz /data/elite_ui.new 2>/dev/null || true");

                rf_build_ack(&ack, g_remote_slot, f->seq, 0x00);
                rf_send_frame(&ack);

                g_is_ota_updating = false;
                render_screen();
                system("killall -9 elite_watchdog.sh 2>/dev/null || true; "
                       "/data/elite_sync 2>/dev/null || /data/sync 2>/dev/null || true; "
                       "sleep 1; "
                       "mount -o remount,ro /data 2>/dev/null || true; "
                       "/data/elite_sync 2>/dev/null || true; "
                       "(/sbin/reboot -f 2>/dev/null || reboot -f 2>/dev/null || (sleep 1 && echo b > /proc/sysrq-trigger)) &");
                usleep(500000);
                reboot(RB_AUTOBOOT);
                exit(1);
            } else {
                printf("[-] OTA ELF verification failed! Corrupted transfer.\n");
                system("rm -rf /tmp/elite_ota_pkg /data/elite_ui.new /tmp/elite_ui.ota.gz 2>/dev/null || true");
                rf_build_ack(&ack, g_remote_slot, f->seq, 0x05);
                rf_send_frame(&ack);
                g_is_ota_updating = false;
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

            if (g_icon_rx_file) {
                fclose(g_icon_rx_file);
                g_icon_rx_file = NULL;
            }
            g_icon_rx_file = fopen("/tmp/custom_icon_rx.tmp", "wb");
            rf_frame_t ack;
            rf_build_ack(&ack, g_remote_slot, f->seq, g_icon_rx_file ? 0x00 : 0x03);
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
                        strncpy(g_activities[i].icon_path, cpath, sizeof(g_activities[i].icon_path) - 1);
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
