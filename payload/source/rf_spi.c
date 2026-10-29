#define _GNU_SOURCE
#include "rf_spi.h"
#include "rf_pairing.h"
#include "rf_keymap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>

uint8_t g_tx_seq = 0;
int g_ack_status = 0;
static uint8_t g_pending_ack_seq = 0;
static int g_ack_received = 0;
static pthread_mutex_t g_ack_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ack_cond = PTHREAD_COND_INITIALIZER;

static int g_nak_count = 0;
static int g_backoff_ms = 0;
#define NAK_BACKOFF_MAX_MS 5000

int flash_cc2544(void) {
    int fin = open(CC2544_FW, O_RDONLY);
    if (fin < 0) return -1;
    int fout = open(RFFW_DEV, O_WRONLY);
    if (fout < 0) { close(fin); return -1; }

    char buf[1024];
    ssize_t n, total = 0;
    while ((n = read(fin, buf, sizeof(buf))) > 0) {
        if (write(fout, buf, n) != n) {
            close(fin);
            close(fout);
            return -1;
        }
        total += n;
    }
    close(fin);
    close(fout);
    printf("[+] Programmed %zd bytes into CC2544 firmware\n", total);
    usleep(100000);
    return 0;
}

int rf_write_cmd(const uint8_t *cmd, size_t len) {
    if (g_rf_fd < 0) return -1;
    pthread_mutex_lock(&g_rf_mutex);
    ssize_t w = write(g_rf_fd, cmd, len);
    pthread_mutex_unlock(&g_rf_mutex);
    if (w < 0) {
        perror("write /dev/rfspi");
        return -1;
    }
    if (g_debug_logging) {
        printf("[*] TX (%zu bytes): ", len);
        for (size_t i = 0; i < len; i++) printf("%02X ", cmd[i]);
        printf("\n");
    }
    return 0;
}

int rf_send_frame(rf_frame_t *frame) {
    uint8_t out[30];
    memset(out, 0, sizeof(out));
    out[0] = 0x12; /* Hub Report 0x12 (Host/Dongle mode) */
    uint8_t slot = frame->dev_slot ? (frame->dev_slot & 0x0F) : (g_remote_slot ? (g_remote_slot & 0x0F) : 0x01);
    out[1] = slot; /* Slot 0x01 or 0x02 */
    out[2] = frame->hdr_flag ? frame->hdr_flag : (0x50 | (frame->seq & 0x0F));
    out[3] = frame->msg_type;
    out[4] = frame->flags;
    memcpy(&out[5], frame->payload, 25);
    return rf_write_cmd(out, sizeof(out));
}

int rf_send_frame_acked(rf_frame_t *frame, int timeout_ms) {
    pthread_mutex_lock(&g_ack_mutex);
    g_pending_ack_seq = frame->seq;
    g_ack_received = 0;
    pthread_mutex_unlock(&g_ack_mutex);

    if (rf_send_frame(frame) < 0) return -1;

    pthread_mutex_lock(&g_ack_mutex);
    if (!g_ack_received) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_ack_cond, &g_ack_mutex, &ts);
    }
    int ok = g_ack_received;
    pthread_mutex_unlock(&g_ack_mutex);
    return ok ? 0 : -1;
}

void rf_signal_ack(uint8_t seq, uint8_t status) {
    pthread_mutex_lock(&g_ack_mutex);
    if ((seq & 0x0F) == (g_pending_ack_seq & 0x0F) || seq == g_pending_ack_seq) {
        g_ack_received = 1;
        g_ack_status = status;
        pthread_cond_signal(&g_ack_cond);
    }
    pthread_mutex_unlock(&g_ack_mutex);
}

void query_paired_devices(void) {
    uint8_t q_pipes[] = { 0x20, 0x30, 0x40, 0x21, 0x31, 0x41, 0x03 };
    for (size_t i = 0; i < sizeof(q_pipes); i++) {
        uint8_t q[7] = {0x10, 0xFF, 0x83, 0xB5, q_pipes[i], 0x00, 0x00};
        rf_write_cmd(q, sizeof(q));
        usleep(30000);
    }
}

static void handle_rf_packet(const uint8_t *pkt, size_t len) {
    if (len < 7) return;

    if (pkt[0] == 0x20 && (pkt[1] & 0x0F) > 0 && (pkt[1] & 0x0F) <= 2) {
        mark_remote_slot_active(pkt[1] & 0x0F);
    } else if (pkt[0] == 0x12 && pkt[1] >= 1 && pkt[1] <= 2) {
        mark_remote_slot_active(pkt[1]);
    }

    if (g_debug_logging) {
        printf("[*] RX (len=%zd): ", len);
        for (size_t i = 0; i < len; i++) printf("%02X ", pkt[i]);
        printf("\n");
    }

    /* Route Companion / eQuad HID keyboard & consumer reports (Report 0x20, Sub-ID 0x01 or 0x03) */
    if (len >= 5 && pkt[0] == 0x20 && (pkt[2] == 0x01 || pkt[2] == 0x03)) {
        handle_companion_packet(pkt, len);
        return;
    }

    /* Route RF protocol frames */
    if (rf_is_valid_frame(pkt, len)) {
        rf_frame_t f;
        memset(&f, 0, sizeof(f));
        if (pkt[0] == 0x20 && len >= 32) {
            if ((pkt[4] & 0xE0) == 0x40 || (pkt[4] & 0xE0) == 0x60) {
                f.report_id = 0x20;
                f.dev_slot = pkt[1] & 0x0F;
                f.sub_id = 0x12;
                f.seq = pkt[4] & 0x0F;
                f.hdr_flag = pkt[4];
                f.msg_type = pkt[5];
                f.flags = pkt[6];
                memcpy(f.payload, &pkt[7], sizeof(f.payload));
            } else {
                memcpy(&f, pkt, sizeof(f));
            }
        } else if (pkt[0] == 0x12 && len >= 30) {
            if (pkt[1] >= 1 && pkt[1] <= 15) {
                g_remote_slot = pkt[1];
            }
            if ((pkt[2] & 0xE0) == 0x40 || (pkt[2] & 0xE0) == 0x60) {
                f.report_id = 0x20;
                f.dev_slot = pkt[1] ? pkt[1] : (g_remote_slot ? g_remote_slot : 0x01);
                f.sub_id = 0x12;
                f.seq = pkt[2] & 0x0F;
                f.hdr_flag = pkt[2];
                f.msg_type = pkt[3];
                f.flags = pkt[4];
                memcpy(f.payload, &pkt[5], sizeof(f.payload));
            } else {
                f.report_id = 0x20;
                f.dev_slot = pkt[1] ? pkt[1] : (g_remote_slot ? g_remote_slot : 0x01);
                f.sub_id = 0x12;
                f.seq = pkt[2] & 0x0F;
                f.hdr_flag = 0x40;
                f.msg_type = pkt[2];
                f.flags = pkt[3];
                memcpy(f.payload, &pkt[4], sizeof(f.payload));
            }
            g_nak_count = 0;
            g_backoff_ms = 0;
        }
        handle_rf_frame(&f);
        return;
    }

    /* 0x42: CC2544 hardware TX completion report: 20 <slot> 42 <status> ...
       Status 0 only means the remote's radio took the frame; elite_ui can still drop it,
       so a frame counts as delivered only when the remote sends RF_MSG_ACK. */
    if (len >= 4 && pkt[0] == 0x20 && pkt[2] == 0x42) {
        uint8_t status = pkt[3];
        if (status == 0x00) {
            if (g_debug_logging)
                printf("[+] CC2544 hardware TX OK (0x42) for seq=%d\n", g_pending_ack_seq);
        } else if (status != 0x01) {
            if (g_debug_logging)
                printf("[-] CC2544 Hardware TX status=0x%02X\n", status);
        }
        return;
    }

    /* 0x41: Link status notification: 10 <slot> 41 04 <status> ... */
    if (len == 7 && pkt[0] == 0x10 && pkt[2] == 0x41) {
        if (pkt[1] >= 1 && pkt[1] <= 15) {
            g_remote_slot = pkt[1];
        }
        bool link_lost = (pkt[4] & 0x40) != 0;
        if (pkt[1] == 1 || pkt[1] == 2) {
            g_paired_remotes[pkt[1] - 1].link_lost = link_lost;
        }
        if (g_debug_logging)
            printf("[*] Hub RF Link Status: %s (slot=%d byte4=0x%02X)\n",
                   link_lost ? "LOST" : "ESTABLISHED", pkt[1], pkt[4]);
        return;
    }

    /* Reset NAK backoff on any valid non-error response from CC2544 */
    if (!(pkt[0] == 0x10 && len >= 3 && pkt[2] == 0x8F)) {
        if (g_nak_count > 0 && g_debug_logging)
            printf("[+] NAK backoff cleared (was %d NAKs, %dms)\n", g_nak_count, g_backoff_ms);
        g_nak_count = 0;
        g_backoff_ms = 0;
    }

    uint8_t report_id = pkt[0];

    /* Short Report: 7 bytes */
    if (report_id == 0x10) {
        uint8_t sub_id = pkt[2];

        /* Firmware Version Reply */
        if (sub_id == 0x81 && pkt[3] == 0xF1) {
            g_fw_version = pkt[5];
            printf("[+] CC2544 FW Version: v%d.00 (0x%02X)\n", g_fw_version, g_fw_version);
            return;
        }

        /* 0x4A: Pairing Notification / Status */
        if (sub_id == 0x4A) {
            uint8_t pair_code = pkt[3];
            uint8_t pair_status = pkt[4];
            printf("[*] CC2544 Pairing event: code=0x%02X status=0x%02X (pkt: %02X %02X %02X %02X %02X %02X %02X)\n",
                   pair_code, pair_status, pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5], pkt[6]);
            if (pair_code == 0x00 && pair_status == 0x00) {
                printf("[+] CC2544 Pairing SUCCESS! Remote paired to Hub!\n");
                query_paired_devices();
                stop_pairing_mode();
            } else if (pair_code >= 0x01 && pair_status == 0x00) {
                printf("[+] CC2544 Pairing lock is OPEN (listening for remotes on slot=%d)...\n", pair_code);
            } else {
                printf("[*] CC2544 Pairing in-progress / event: code=0x%02X status=0x%02X\n", pair_code, pair_status);
            }
            return;
        }

        /* 0x8F: Error / NAK */
        if (sub_id == 0x8F) {
            g_nak_count++;
            if (g_nak_count > 3) {
                if (g_nak_count - 3 >= 20) {
                    g_backoff_ms = NAK_BACKOFF_MAX_MS;
                } else {
                    g_backoff_ms = 100 * (1 << (g_nak_count - 3));
                    if (g_backoff_ms > NAK_BACKOFF_MAX_MS) g_backoff_ms = NAK_BACKOFF_MAX_MS;
                }
            }
            if (g_debug_logging || g_nak_count <= 5)
                printf("[-] CC2544 NAK: SubID=0x%02X err=0x%02X (count=%d, backoff=%dms)\n",
                       pkt[3], pkt[5], g_nak_count, g_backoff_ms);
            return;
        }
    }

    /* Long Report: 20 bytes */
    if (report_id == 0x11) {
        uint8_t sub_id = pkt[2];

        if (sub_id == 0x83 && pkt[3] == 0xB5) {
            uint8_t pipe = pkt[4];
            if (pipe == 0x03) {
                memcpy(g_hub_rf_addr, &pkt[5], 4);
                if (g_debug_logging)
                    printf("[+] Hub CC2544 Own RF Address: %02X %02X %02X %02X\n",
                           pkt[5], pkt[6], pkt[7], pkt[8]);
                return;
            }
            if (pipe >= 0x20 && pipe <= 0x25) {
                int sidx = pipe - 0x20;
                uint16_t equad_id = ((uint16_t)pkt[7] << 8) | pkt[8];
                if (sidx < MAX_PAIRED_REMOTES && equad_id != 0) {
                    g_paired_remotes[sidx].active = true;
                    g_has_paired_device = true;
                    g_paired_remotes[sidx].equad_id = equad_id;
                    if (equad_id == 0x401E) {
                        g_paired_remotes[sidx].model = REMOTE_MODEL_SMART_CONTROL;
                    } else if (equad_id == 0x4026 || equad_id == 0x4027) {
                        g_paired_remotes[sidx].model = REMOTE_MODEL_COMPANION;
                    } else if (equad_id == 0x4021) {
                        g_paired_remotes[sidx].model = REMOTE_MODEL_ELITE;
                    }
                    printf("[+] Slot %d identified: EquadID=0x%04X (model=%s)\n",
                           sidx + 1, equad_id, get_remote_model_name(g_paired_remotes[sidx].model));
                }
                return;
            }
            if (pipe >= 0x30 && pipe <= 0x35) {
                int sidx = pipe - 0x30;
                if (sidx < MAX_PAIRED_REMOTES) {
                    memcpy(g_paired_remotes[sidx].rf_addr, &pkt[5], 4);
                    bool has_addr = (pkt[5] != 0 || pkt[6] != 0 || pkt[7] != 0 || pkt[8] != 0);
                    g_paired_remotes[sidx].active = has_addr;
                    if (has_addr) g_has_paired_device = true;
                    if (sidx == 0 && has_addr) memcpy(g_paired_addr, &pkt[5], 4);
                    printf("[+] Slot %d RF Address: %02X %02X %02X %02X (active=%d)\n",
                           sidx + 1, pkt[5], pkt[6], pkt[7], pkt[8], g_paired_remotes[sidx].active);
                }
                return;
            }
            if (pipe >= 0x40 && pipe <= 0x45) {
                int sidx = pipe - 0x40;
                uint8_t nlen = pkt[5];
                if (sidx < MAX_PAIRED_REMOTES && nlen > 0) {
                    if (nlen > 15) nlen = 15;
                    memcpy(g_paired_remotes[sidx].name, &pkt[6], nlen);
                    g_paired_remotes[sidx].name[nlen] = '\0';
                    if (strstr(g_paired_remotes[sidx].name, "20+") != NULL ||
                        strstr(g_paired_remotes[sidx].name, "Smart") != NULL) {
                        g_paired_remotes[sidx].model = REMOTE_MODEL_SMART_CONTROL;
                    } else if (strstr(g_paired_remotes[sidx].name, "Companion") != NULL ||
                               strstr(g_paired_remotes[sidx].name, "Home") != NULL) {
                        g_paired_remotes[sidx].model = REMOTE_MODEL_COMPANION;
                    }
                    printf("[+] Slot %d Device Name: '%s' (model=%s)\n",
                           sidx + 1, g_paired_remotes[sidx].name, get_remote_model_name(g_paired_remotes[sidx].model));
                }
                return;
            }
        }
    }

    if (g_debug_logging) {
        printf("[*] RX (%zd bytes): ", len);
        for (size_t i = 0; i < len; i++) printf("%02X ", pkt[i]);
        printf("\n");
    }
}

void *rx_worker(void *arg) {
    (void)arg;
    uint8_t buf[64];

    while (g_running) {
        memset(buf, 0, sizeof(buf));
        ssize_t n = read(g_rf_fd, buf, sizeof(buf));
        if (n > 0) {
            handle_rf_packet(buf, (size_t)n);
        } else if (n < 0) {
            if (errno == EINTR) continue;
            perror("read /dev/rfspi");
            usleep(100000);
        } else {
            usleep(50000);
        }
    }
    return NULL;
}

int rf_spi_open(void) {
    g_rf_fd = open(RFSPI_DEV, O_RDWR);
    if (g_rf_fd < 0) {
        perror("open /dev/rfspi");
        return -1;
    }
    return 0;
}

void rf_spi_close(void) {
    if (g_rf_fd >= 0) {
        close(g_rf_fd);
        g_rf_fd = -1;
    }
}
