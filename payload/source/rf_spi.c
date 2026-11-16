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
#include <poll.h>
#include <sys/time.h>
#include <time.h>

uint8_t g_tx_seq = 0;
/* The status and payload of the last ACK this thread waited for (rf_send_frame_acked): one copy
 * per thread, so a caller reads its own ACK even when another thread sent in the meantime */
__thread int g_ack_status = 0;
__thread uint8_t g_ack_payload[25] = {0};
/* One ACKed send at a time: the frame waited for is described by one slot and one sequence
 * number, and several threads send ACKed frames (config push, icon push, settings, sleep, wake,
 * Help, OTA). Set (under g_ack_mutex) from before the pending number is set until the result is
 * copied. Not a mutex held over the wait: on the Hub (no futex) a thread blocked on a held mutex
 * spins, so a second sender sleeps in short steps until the flag is clear. */
static int g_acked_tx_busy = 0;
static uint8_t g_pending_ack_seq = 0;
static uint8_t g_pending_ack_slot = 0;
static int g_ack_received = 0;
static int g_ack_result_status = 0;
static uint8_t g_ack_result_payload[25];
static pthread_mutex_t g_ack_mutex = PTHREAD_MUTEX_INITIALIZER;
/* The Hub's kernel has no futex system call, so pthread_cond_timedwait() returns at once there
 * and a sender waiting for an ACK spun at full CPU for the whole wait. The receive thread
 * writes a byte to this pipe instead, and the sender sleeps in poll() until it or the timeout
 * comes. g_ack_received (under g_ack_mutex) stays the answer; the byte only wakes. */
static int g_ack_pipe[2] = { -1, -1 };

/* CC2544 NAKs in a row (logged; senders retry on their own ACK timeouts) */
static int g_nak_count = 0;

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
    /* Only a negative result is an error: the CC2544 driver (/dev/rfspi) returns 0 for a
     * complete write (measured on the Hub), so a length check would fail every send */
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

int rf_send_ota_data(uint8_t slot, uint32_t idx, bool ack_req, const uint8_t *data, size_t len) {
    uint8_t out[30];
    memset(out, 0, sizeof(out));
    out[0] = 0x12;
    out[1] = slot ? (slot & 0x0F) : (g_remote_slot ? (g_remote_slot & 0x0F) : 0x01);
    out[2] = (uint8_t)(RF_OTA_HDR_DATA | (idx & 0x0F) | (ack_req ? RF_OTA_HDR_ACK : 0));
    out[3] = (uint8_t)((idx >> 4) & 0xFF);
    if (len > RF_OTA_DATA_BYTES) len = RF_OTA_DATA_BYTES;
    if (data && len) memcpy(&out[4], data, len);
    return rf_write_cmd(out, sizeof(out));
}

static uint8_t s_power_mode = RF_PM_ACTIVE;
int rf_set_power_mode(uint8_t mode) {
    if (mode == s_power_mode) return 0;
    uint8_t cmd[7] = {0x20, mode, 0x00, 0x00, 0x00, 0x00, 0x00};
    int r = rf_write_cmd(cmd, sizeof(cmd));
    if (r == 0) {
        s_power_mode = mode;
        printf("[*] CC2544 power mode 0x%02X\n", mode);
    }
    return r;
}

/* Index into g_remote_link for a slot byte, or -1 */
static int link_idx(uint8_t slot) {
    slot &= 0x0F;
    return (slot >= 1 && slot <= MAX_PAIRED_REMOTES) ? slot - 1 : -1;
}

/* g_ack_mutex held */
static void ack_pipe_open_locked(void) {
    if (g_ack_pipe[0] >= 0) return;
    int fds[2];
    if (pipe(fds) != 0) return;
    for (int i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    g_ack_pipe[0] = fds[0];
    g_ack_pipe[1] = fds[1];
}

static void ack_pipe_drain(void) {
    char buf[16];
    if (g_ack_pipe[0] < 0) return;
    while (read(g_ack_pipe[0], buf, sizeof(buf)) > 0) {}
}

int rf_send_frame_acked(rf_frame_t *frame, int timeout_ms) {
    /* A send of the same frame as this thread's last one to this remote is a retry. Kept per
     * thread: each sender resends only its own frames, and with one shared record another
     * thread's send in between (even one still waiting for the busy flag) hid the retry. */
    static __thread struct { uint8_t seq, type; bool valid; } s_last[MAX_PAIRED_REMOTES];
    int li = link_idx(frame->dev_slot ? frame->dev_slot : (g_remote_slot ? g_remote_slot : 0x01));
    bool retry = false;
    if (li >= 0) {
        retry = s_last[li].valid && s_last[li].seq == frame->seq && s_last[li].type == frame->msg_type;
        s_last[li].seq = frame->seq;
        s_last[li].type = frame->msg_type;
        s_last[li].valid = true;
    }

    pthread_mutex_lock(&g_ack_mutex);
    while (g_acked_tx_busy) {
        pthread_mutex_unlock(&g_ack_mutex);
        usleep(2000);
        pthread_mutex_lock(&g_ack_mutex);
    }
    g_acked_tx_busy = 1;
    /* the link counters change under g_ack_mutex: several threads send */
    if (li >= 0) {
        g_remote_link[li].acked_sent++;
        if (retry) g_remote_link[li].retries++;
    }
    ack_pipe_open_locked();
    ack_pipe_drain();   /* a byte from an earlier ACK must not end this wait */
    g_pending_ack_seq = frame->seq;
    /* the slot rf_send_frame() sends to */
    g_pending_ack_slot = frame->dev_slot ? (frame->dev_slot & 0x0F) : (g_remote_slot ? (g_remote_slot & 0x0F) : 0x01);
    g_ack_received = 0;
    pthread_mutex_unlock(&g_ack_mutex);

    if (rf_send_frame(frame) < 0) {
        pthread_mutex_lock(&g_ack_mutex);
        g_pending_ack_slot = 0;
        g_acked_tx_busy = 0;
        pthread_mutex_unlock(&g_ack_mutex);
        return -1;
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    pthread_mutex_lock(&g_ack_mutex);
    while (!g_ack_received) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (long)(now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed >= timeout_ms) break;
        pthread_mutex_unlock(&g_ack_mutex);
        if (g_ack_pipe[0] >= 0) {
            struct pollfd pfd = { g_ack_pipe[0], POLLIN, 0 };
            if (poll(&pfd, 1, (int)(timeout_ms - elapsed)) > 0) ack_pipe_drain();
        } else {
            usleep(2000);   /* no pipe: check the flag every 2 ms */
        }
        pthread_mutex_lock(&g_ack_mutex);
    }
    int ok = g_ack_received;
    g_ack_received = 0;
    g_pending_ack_slot = 0; /* nothing waits: a late ACK matches no one */
    if (ok) {
        g_ack_status = g_ack_result_status;
        memcpy(g_ack_payload, g_ack_result_payload, sizeof(g_ack_payload));
        if (li >= 0) g_remote_link[li].acked_ok++;
    }
    g_acked_tx_busy = 0;
    pthread_mutex_unlock(&g_ack_mutex);
    return ok ? 0 : -1;
}

/* Ask a remote to turn its screen on; returns the attempt that was ACKed, or 0 if none was */
int rf_wake_remote(uint8_t slot, int attempts) {
    for (int i = 1; i <= attempts && g_running; i++) {
        rf_frame_t f;
        uint8_t pl[1] = {0};
        rf_build_frame(&f, slot, RF_MSG_WAKE, ++g_tx_seq, RF_FLAG_ACK_REQ, pl, 1);
        if (rf_send_frame_acked(&f, 600) == 0) return i;
        usleep(100000);
    }
    return 0;
}

void rf_signal_ack_from(uint8_t slot, uint8_t seq, uint8_t status, const uint8_t *payload) {
    pthread_mutex_lock(&g_ack_mutex);
    /* The frame carries 4 bits of the sequence number; the ACK must also come from the slot the
     * frame went to (slot 0 = not known, as from older callers): an ACK of the other remote for
     * its own frame with the same low bits is not this one */
    slot &= 0x0F;
    if (g_pending_ack_slot != 0 && !g_ack_received && (slot == 0 || slot == g_pending_ack_slot) &&
        ((seq & 0x0F) == (g_pending_ack_seq & 0x0F) || seq == g_pending_ack_seq)) {
        g_ack_received = 1;
        g_ack_result_status = status;
        if (payload) {
            memcpy(g_ack_result_payload, payload, sizeof(g_ack_result_payload));
        } else {
            memset(g_ack_result_payload, 0, sizeof(g_ack_result_payload));
        }
        if (g_ack_pipe[1] >= 0) {
            ssize_t w = write(g_ack_pipe[1], "a", 1);   /* full pipe: a wake is already pending */
            (void)w;
        }
    }
    pthread_mutex_unlock(&g_ack_mutex);
}

void rf_signal_ack(uint8_t seq, uint8_t status, const uint8_t *payload) {
    rf_signal_ack_from(0, seq, status, payload);
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
        /* hub_rf keeps two remote slots; a key report from another CC2544 slot (a third paired
           device) would be handled as a key of the remote in slot 1 */
        uint8_t kslot = pkt[1] & 0x0F;
        if (kslot < 1 || kslot > MAX_PAIRED_REMOTES) {
            static uint16_t s_logged; /* one line per slot */
            if (!(s_logged & (1u << kslot))) {
                s_logged |= (uint16_t)(1u << kslot);
                printf("[-] RF key report from slot %u ignored: only slots 1 and 2 hold a remote\n", kslot);
            }
            return;
        }
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
            if (pkt[1] >= 1 && pkt[1] <= MAX_PAIRED_REMOTES) {
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
        }
        if (link_idx(f.dev_slot) >= 0) g_remote_link[link_idx(f.dev_slot)].frames_rx++;
        handle_rf_frame(&f);
        return;
    }

    /* 0x42: CC2544 hardware TX completion report: 20 <slot> 42 <status> ...
       Status 0 only means the remote's radio took the frame; remote_ui can still drop it,
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
        /* the default slot for sends without one: only a slot that can hold a remote */
        if (pkt[1] >= 1 && pkt[1] <= MAX_PAIRED_REMOTES) {
            g_remote_slot = pkt[1];
        }
        bool link_lost = (pkt[4] & 0x40) != 0;
        if (pkt[1] == 1 || pkt[1] == 2) {
            g_paired_remotes[pkt[1] - 1].link_lost = link_lost;
            if (!link_lost) rf_key_note_restart_hint(pkt[1]); /* a remote that started links again */
        }
        if (g_debug_logging)
            printf("[*] Hub RF Link Status: %s (slot=%d byte4=0x%02X)\n",
                   link_lost ? "LOST" : "ESTABLISHED", pkt[1], pkt[4]);
        return;
    }

    /* Any valid non-error response from the CC2544 ends a run of NAKs */
    if (!(pkt[0] == 0x10 && len >= 3 && pkt[2] == 0x8F)) {
        if (g_nak_count > 0 && g_debug_logging)
            printf("[+] CC2544 answers again after %d NAKs\n", g_nak_count);
        g_nak_count = 0;
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
            /* 10 <slot> 8F <report of the refused send> ... */
            if (link_idx(pkt[1]) >= 0) g_remote_link[link_idx(pkt[1])].naks++;
            g_nak_count++;
            if (g_debug_logging || g_nak_count <= 5)
                printf("[-] CC2544 NAK: SubID=0x%02X err=0x%02X (count=%d)\n",
                       pkt[3], pkt[5], g_nak_count);
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
                    /* the EquadID names the family; a Juniper product comes from its skin id */
                    if (equad_id == RF_EQUAD_SMART_CONTROL) {
                        g_paired_remotes[sidx].model = REMOTE_MODEL_SMART_CONTROL;
                    } else if (equad_id == RF_EQUAD_SPRITE_A || equad_id == RF_EQUAD_SPRITE_B) {
                        g_paired_remotes[sidx].model = REMOTE_MODEL_COMPANION;
                    } else if (equad_id == RF_EQUAD_JUNIPER) {
                        g_paired_remotes[sidx].model = REMOTE_MODEL_JUNIPER;
                    }
                    printf("[+] Slot %d identified: EquadID=0x%04X (family=%s)\n",
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
                    /* printable ASCII without quote and backslash: the name goes into the RF
                       status object, which is JSON built with "%s" */
                    for (uint8_t i = 0; i < nlen; i++) {
                        uint8_t c = pkt[6 + i];
                        g_paired_remotes[sidx].name[i] = (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? (char)c : '?';
                    }
                    g_paired_remotes[sidx].name[nlen] = '\0';
                    /* the name hints at a Sprite remote only while the EquadID is unknown; it
                     * never replaces an identified family (is_companion_slot applies it) */
                    is_companion_slot((uint8_t)(sidx + 1));
                    printf("[+] Slot %d Device Name: '%s' (family=%s)\n",
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

volatile bool g_rf_read_failed = false;

void *rx_worker(void *arg) {
    (void)arg;
    uint8_t buf[64];
    int read_errors = 0;

    while (g_running) {
        memset(buf, 0, sizeof(buf));
        ssize_t n = read(g_rf_fd, buf, sizeof(buf));
        if (n > 0) {
            read_errors = 0;
            handle_rf_packet(buf, (size_t)n);
        } else if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { usleep(100000); continue; }
            if (++read_errors <= 3) perror("read /dev/rfspi");
            if (read_errors >= RF_READ_ERRORS_MAX) {
                /* The radio does not answer any more: hub_rf ends, its supervisor starts it again
                 * and the start sets the CC2544 up anew. It used to log ten lines a second for as
                 * long as it ran, with the remotes on "No Hub". */
                fprintf(stderr, "[-] /dev/rfspi: %d read errors in a row, hub_rf stops for a restart\n", read_errors);
                g_rf_read_failed = true;
                g_running = false;
                break;
            }
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
    fcntl(g_rf_fd, F_SETFD, FD_CLOEXEC); /* one user of /dev/rfspi: no helper may keep it */
    return 0;
}

void rf_spi_close(void) {
    if (g_rf_fd >= 0) {
        close(g_rf_fd);
        g_rf_fd = -1;
    }
}
