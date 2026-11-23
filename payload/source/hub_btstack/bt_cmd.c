#include "bt_cmd.h"
#include "bt_pairing.h"
#include "bt_hid.h"
#include "bt_remote.h"
#include "bt_adv.h"
#include "bt_listen.h"
#include "bt_wake_key.h"

#ifndef TLV_DB_PATH
#define TLV_DB_PATH "/data/harmony/btstack_keys.tlv"
#endif
/* A command left in CMD_FILE longer than this is not run: it was written while hub_btstack was
 * not running (hub_send_btstack_cmd falls back to the file), maybe long ago */
#define CMD_FILE_MAX_AGE_S 10
/* A raw report that puts a key or button down and has no release of its own ("RAW", or
 * "press|" without a release part) is released after this long, like a held key */
#define HOST_RAW_HOLD_MS 10000

/* Mouse deltas and the wheel are one signed byte each: a larger value wrapped (200 became -56) */
static int8_t mouse_delta(int v) {
    if (v > 127) return 127;
    if (v < -127) return -127;
    return (int8_t)v;
}

/* What a report puts down: 1 a keyboard key or modifier, 2 a consumer usage, 4 a mouse button,
 * 0 nothing (a release, a mouse move, or not a report) */
static uint8_t report_down_type(const uint8_t *r, int len) {
    if (len < 3 || r[0] != 0xa1) return 0;
    if (r[1] == 0x01) {
        for (int i = 2; i < len; i++) {
            if (r[i]) return 1;
        }
        return 0;
    }
    if (r[1] == 0x02) return (r[2] || (len > 3 && r[3])) ? 2 : 0;
    if (r[1] == 0x03) return r[2] ? 4 : 0;
    return 0;
}

/* After a raw report on cid: a "down" with nothing to lift it gets the hold release; a release of
 * the pending kind on the same channel ends it */
static void raw_report_sent(const uint8_t *r, int len, uint16_t cid) {
    uint8_t t = report_down_type(r, len);
    if (t) {
        g_host_pending_cid = cid;
        g_host_pending_release_type = t;
        arm_host_key_release_timer(HOST_RAW_HOLD_MS);
        return;
    }
    uint8_t kind = (len >= 2 && r[0] == 0xa1) ? (r[1] == 0x03 ? 4 : r[1]) : 0;
    if (kind && kind == g_host_pending_release_type && cid == g_host_pending_cid) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        g_host_pending_release_type = 0;
    }
}

/* The address argument of connect_host and disconnect_host: 0 none (empty, or for connect_host
 * only the duration in seconds that POST /bt/reconnect adds), 1 an address, -1 something that is
 * not an address. That last case does nothing: it used to mean "every host". */
static int cmd_addr_arg(const char *arg, bd_addr_t out, bool duration_ok) {
    while (*arg == ' ') arg++;
    if (!*arg) return 0;
    if (parse_bd_addr(arg, out)) return 1;
    const char *p = arg;
    while (*p >= '0' && *p <= '9') p++;
    size_t digits = (size_t)(p - arg);
    while (*p == ' ') p++;
    return (duration_ok && *p == '\0' && digits > 0 && digits <= 5) ? 0 : -1;
}

/* The key store as hub_btstack opened it at start. A restore renames another file over it; the
 * running BTstack keeps using the old, deleted one (restored pairings unknown, new ones lost). */
static struct stat s_tlv_at_start;
static bool s_tlv_at_start_ok = false;

static bool key_store_replaced(void) {
    struct stat st;
    if (!s_tlv_at_start_ok || stat(TLV_DB_PATH, &st) != 0) return false;
    return st.st_ino != s_tlv_at_start.st_ino || st.st_dev != s_tlv_at_start.st_dev;
}

btstack_timer_source_t cmd_poll_timer;
static int g_cmd_sock_fd = -1;
static btstack_data_source_t g_cmd_sock_ds;
static btstack_timer_source_t g_pairing_timer;

void pairing_timeout_cancel(void) {
    btstack_run_loop_remove_timer(&g_pairing_timer);
}

/* Leave pairing mode: not discoverable, drop the pending device link, and reconnect the
 * host that pairing mode disconnected if an activity is running. */
void pairing_mode_off(const char *reason) {
    pairing_timeout_cancel();
    g_host_pairing = false;
    gap_discoverable_control(0);
    /* A pairing that never happened must not link its device to the next host that connects */
    unlink("/tmp/bt_pending_pair_device");
    char msg[128];
    snprintf(msg, sizeof(msg), "Classic HID Host pairing disabled (%s)", reason);
    log_msg(msg);
    update_status(app_state == APP_READY ? "ready" : "standby");
    update_bthid_status();
    char cur_act[64] = "-1";
    read_current_activity(cur_act, sizeof(cur_act));
    bd_addr_t tgt;
    if (cur_act[0] && strcmp(cur_act, "-1") != 0 && strcasecmp(cur_act, "poweroff") != 0 &&
        get_reconnect_target(tgt) && !is_host_connected(tgt)) {
        start_host_reconnect(tgt, MAX_HOST_RECONNECT_ATTEMPTS);
    }
}

static void pairing_timeout_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (g_host_pairing) pairing_mode_off("timeout");
}

/* ---- Generic Classic BT page (fire-and-forget connection request) --------
 * IPC: "bt_page <target_bdaddr> [spoof_bdaddr]"
 *
 * Sends a raw HCI Create Connection to <target>.  If spoof_bdaddr is given,
 * the Hub's BD_ADDR is temporarily changed to it via CSR8811 BCCMD before
 * paging and restored after the Connection Complete event.
 *
 * Uses hci_send_cmd_packet() (raw HCI) instead of gap_connect() to avoid
 * creating an hci_connection_t entry in BTstack's connection list, which
 * would block all subsequent hid_device_connect() calls for ~15 seconds
 * until the HCI page timeout fires. */
static bd_addr_t s_page_target;
static bd_addr_t s_page_orig_addr;      /* saved BEFORE spoof */
static bool      s_page_spoofed = false;
static bool      s_page_active  = false;
/* A page with no Connection Complete for the target (lost event, a Create Connection the
 * controller refused) ends here: without it every later bt_page was dropped as busy and a
 * spoofed address stayed. Longer than the controller's page timeout (5.12 s by default). */
#define BT_PAGE_TIMEOUT_MS 10000
static btstack_timer_source_t s_page_timer;

static void bt_page_restore_addr(void) {
    if (!s_page_spoofed) return;
    uint8_t pkt[28] = {
        0x00, 0xFC, 25, 0xC2,
        0x02, 0x00, 0x0c, 0x00, 0x11, 0x47, 0x03, 0x70,
        0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x08, 0x00,
        s_page_orig_addr[3], 0x00, s_page_orig_addr[5], s_page_orig_addr[4],
        s_page_orig_addr[2], 0x00, s_page_orig_addr[1], s_page_orig_addr[0]
    };
    if (hci_send_cmd_packet(pkt, sizeof(pkt)) != 0) log_msg("bt_page: BD_ADDR restore command not sent");
    else log_msg("bt_page: BD_ADDR restored");
    s_page_spoofed = false;
}

static void bt_page_end(void) {
    btstack_run_loop_remove_timer(&s_page_timer);
    bt_page_restore_addr();
    s_page_active = false;
}

static void bt_page_timeout(btstack_timer_source_t *ts) {
    (void)ts;
    if (!s_page_active) return;
    log_msg("bt_page: no Connection Complete for the target, page ended");
    bt_page_end();
}

void bt_page_on_complete(const bd_addr_t addr, uint8_t status, hci_con_handle_t handle) {
    if (!s_page_active) return;
    if (memcmp(addr, s_page_target, 6) != 0) return;

    char msg[128];
    snprintf(msg, sizeof(msg), "bt_page: Complete for %s status=0x%02x handle=0x%04x",
             bd_addr_to_str(addr), status, handle);
    log_msg(msg);

    if (status == 0 && handle != 0 && handle != HCI_CON_HANDLE_INVALID) {
        /* Page succeeded — send raw HCI Disconnect (we only needed the page) */
        uint8_t disc[7];
        disc[0] = 0x06; disc[1] = 0x04; /* HCI Disconnect opcode */
        disc[2] = 3;                     /* parameter length */
        disc[3] = handle & 0xFF; disc[4] = (handle >> 8) & 0xFF;
        disc[5] = 0x13;                  /* reason: Remote User Terminated */
        if (hci_send_cmd_packet(disc, 6) != 0) log_msg("bt_page: Disconnect command not sent");
    }

    bt_page_end();
}

static void bt_page(const bd_addr_t target, const char *spoof_str) {
    if (s_page_active) {
        log_msg("bt_page: busy, request dropped");
        return;
    }

    memcpy(s_page_target, target, 6);
    s_page_active = true;
    s_page_spoofed = false;

    char msg[128];
    snprintf(msg, sizeof(msg), "bt_page: Paging %s", bd_addr_to_str(target));
    log_msg(msg);

    /* Save original BD_ADDR BEFORE any spoof */
    gap_local_bd_addr(s_page_orig_addr);

    if (spoof_str && spoof_str[0]) {
        bd_addr_t spoof;
        if (parse_bd_addr(spoof_str, spoof)) {
            uint8_t pkt[28] = {
                0x00, 0xFC, 25, 0xC2,
                0x02, 0x00, 0x0c, 0x00, 0x11, 0x47, 0x03, 0x70,
                0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x08, 0x00,
                spoof[3], 0x00, spoof[5], spoof[4],
                spoof[2], 0x00, spoof[1], spoof[0]
            };
            if (hci_send_cmd_packet(pkt, sizeof(pkt)) != 0) {
                log_msg("bt_page: spoof command not sent, page dropped");
                s_page_active = false;
                return;
            }
            s_page_spoofed = true;

            char smsg[128];
            snprintf(smsg, sizeof(smsg), "bt_page: Spoofed BD_ADDR to %s", spoof_str);
            log_msg(smsg);
        }
    }

    /* Raw HCI Create Connection — bypasses BTstack's connection state machine */
    uint8_t cc[16];
    cc[0] = 0x05; cc[1] = 0x04;  /* opcode: HCI Create Connection (OGF=1, OCF=5) */
    cc[2] = 13;                   /* parameter length */
    /* BD_ADDR in HCI wire order (little-endian) */
    cc[3] = target[5]; cc[4] = target[4]; cc[5] = target[3];
    cc[6] = target[2]; cc[7] = target[1]; cc[8] = target[0];
    cc[9] = 0x18; cc[10] = 0xCC; /* packet type: DM1|DH1|DM3|DH3|DM5|DH5 */
    cc[11] = 0x01;               /* page scan repetition mode R1 */
    cc[12] = 0x00;               /* reserved */
    cc[13] = 0x00; cc[14] = 0x00; /* clock offset: none */
    cc[15] = 0x00;               /* allow role switch: no */
    if (hci_send_cmd_packet(cc, 16) != 0) {
        log_msg("bt_page: Create Connection not sent");
        bt_page_restore_addr();
        s_page_active = false;
        return;
    }
    btstack_run_loop_set_timer_handler(&s_page_timer, &bt_page_timeout);
    btstack_run_loop_set_timer(&s_page_timer, BT_PAGE_TIMEOUT_MS);
    btstack_run_loop_add_timer(&s_page_timer);
}


/* The key pipe is read as soon as hub_daemon writes to it. It used to be read only from the
 * 100 ms command timer: every key waited up to 100 ms, and a press and its release written
 * within one period reached the host back to back. */
static btstack_data_source_t g_bthid_fifo_ds;
static char s_fifo_carry[256]; /* a line cut by the end of a read */
static size_t s_fifo_carry_len = 0;

static void bthid_fifo_process(btstack_data_source_t *ds, btstack_data_source_callback_type_t callback_type) {
    (void)ds;
    if (callback_type == DATA_SOURCE_CALLBACK_READ) poll_bthid_fifo();
}

void poll_bthid_fifo(void) {
    if (g_bthid_fifo_fd < 0) {
        struct stat st;
        if (stat(BTHID_FIFO, &st) == 0 && !S_ISFIFO(st.st_mode)) unlink(BTHID_FIFO);
        if (stat(BTHID_FIFO, &st) != 0) mkfifo(BTHID_FIFO, 0666);
        chmod(BTHID_FIFO, 0666);
        g_bthid_fifo_fd = open(BTHID_FIFO, O_RDWR | O_NONBLOCK);
        if (g_bthid_fifo_fd < 0) return;
        btstack_run_loop_set_data_source_fd(&g_bthid_fifo_ds, g_bthid_fifo_fd);
        btstack_run_loop_set_data_source_handler(&g_bthid_fifo_ds, &bthid_fifo_process);
        btstack_run_loop_add_data_source(&g_bthid_fifo_ds);
        btstack_run_loop_enable_data_source_callbacks(&g_bthid_fifo_ds, DATA_SOURCE_CALLBACK_READ);
    }

    char buf[1024 + sizeof(s_fifo_carry)];
    memcpy(buf, s_fifo_carry, s_fifo_carry_len);
    ssize_t n = read(g_bthid_fifo_fd, buf + s_fifo_carry_len, 1023);
    if (n <= 0) return;
    n += (ssize_t)s_fifo_carry_len;
    s_fifo_carry_len = 0;
    buf[n] = 0;
    /* A read that ends in the middle of a line (more than 1023 bytes were waiting): keep the cut
     * line for the next read. Its two halves used to run as two commands, each typed to the host
     * as text. */
    if (buf[n - 1] != '\n' && buf[n - 1] != '\r') {
        char *last_nl = strrchr(buf, '\n');
        if (last_nl && strlen(last_nl + 1) < sizeof(s_fifo_carry)) {
            s_fifo_carry_len = strlen(last_nl + 1);
            memcpy(s_fifo_carry, last_nl + 1, s_fifo_carry_len);
            last_nl[1] = 0;
        }
    }

    char *save = NULL;
    char *line = strtok_r(buf, "\r\n", &save);
    while (line) {
        while (*line == ' ') line++;
        if (*line) {
            if (strncasecmp(line, "pair_host_on", 12) == 0 ||
                strncasecmp(line, "pair_host_off", 13) == 0 ||
                strncasecmp(line, "connect_host", 12) == 0 ||
                strncasecmp(line, "disconnect_host", 15) == 0 ||
                strncasecmp(line, "stop_scan", 9) == 0 ||
                strncasecmp(line, "scan", 4) == 0 ||
                strncasecmp(line, "pair ", 5) == 0) {
                execute_btstack_cmd(line);
                line = strtok_r(NULL, "\r\n", &save);
                continue;
            }
            if (strncasecmp(line, "reload_map", 10) == 0) {
                bt_remote_invalidate_map();
                line = strtok_r(NULL, "\r\n", &save);
                continue;
            }
            if (strncmp(line, "TS ", 3) == 0) {
                /* Key trace: the times of the next command, sent by hub_daemon with debug logging on */
                bt_hid_trace_begin(line + 3);
                line = strtok_r(NULL, "\r\n", &save);
                continue;
            }
            if (strncmp(line, "TARGET ", 7) == 0) {
                bd_addr_t t_addr;
                host_conn_t *th = parse_bd_addr(line + 7, t_addr) ? register_host_target(t_addr, NULL) : NULL;
                if (th) {
                    int i = (int)(th - g_hosts);
                    g_active_host_idx = i;
                    if (!g_hosts[i].connected || g_hosts[i].hid_cid == 0) {
                        if (!g_host_reconnect_active) {
                            start_host_reconnect(t_addr, MAX_HOST_RECONNECT_ATTEMPTS);
                        } else {
                            try_connect_host_addr(t_addr);
                        }
                    }
                } else {
                    /* An address that does not parse, or no room in the host table: the keys
                     * that follow are dropped. They used to go to the previous target host. */
                    g_active_host_idx = BT_HOST_IDX_NONE;
                    char tmsg[96];
                    snprintf(tmsg, sizeof(tmsg), "TARGET %.40s cannot be used: its keys are dropped", line + 7);
                    log_msg(tmsg);
                }
                line = strtok_r(NULL, "\r\n", &save);
                continue;
            }
            bt_hid_trace_command(line);
            uint16_t cur_cid = get_target_host_hid_cid();
            if (cur_cid == 0 && g_active_host_idx != BT_HOST_IDX_NONE) {
                if (!g_host_reconnect_active) {
                    bd_addr_t tgt;
                    if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS && !is_null_bd_addr(g_hosts[g_active_host_idx].addr)) {
                        start_host_reconnect(g_hosts[g_active_host_idx].addr, 20);
                    } else if (get_pinned_host_target(tgt)) {
                        start_host_reconnect(tgt, 20);
                    } else {
                        try_connect_host();
                    }
                }
            }
            if (strncmp(line, "TEXT ", 5) == 0) {
                send_host_text(line + 5);
            } else if (strncmp(line, "KBD_DOWN ", 9) == 0) {
                /* Keyboard usage (hex), held until RELEASE */
                unsigned int kcode = 0;
                if (sscanf(line + 9, "%x", &kcode) == 1 && kcode > 0 && kcode <= 0xFF) {
                    g_host_send_hold_mode = 1;
                    send_host_keyboard_report(0, (uint8_t)kcode);
                    g_host_send_hold_mode = 0;
                }
            } else if (strncmp(line, "KBD ", 4) == 0) {
                /* Keyboard usage (hex), press and release */
                unsigned int kcode = 0;
                if (sscanf(line + 4, "%x", &kcode) == 1 && kcode > 0 && kcode <= 0xFF) {
                    send_host_keyboard_report(0, (uint8_t)kcode);
                }
            } else if (strncmp(line, "KEYDOWN ", 8) == 0) {
                send_host_named_key_ex(line + 8, 1);
            } else if (strncmp(line, "KEYUP ", 6) == 0) {
                send_host_release_all();
            } else if (strncmp(line, "KEY ", 4) == 0) {
                send_host_named_key_ex(line + 4, 0);
            } else if (strcasecmp(line, "RELEASE") == 0 || strcasecmp(line, "RELEASE_ALL") == 0) {
                send_host_release_all();
            } else if (strncmp(line, "CONSUMER_DOWN ", 14) == 0) {
                unsigned int ccode = 0;
                if (sscanf(line + 14, "%x", &ccode) == 1) {
                    g_host_send_hold_mode = 1;
                    send_host_consumer_report((uint16_t)ccode);
                    g_host_send_hold_mode = 0;
                }
            } else if (strncmp(line, "CONSUMER_UP", 11) == 0) {
                send_host_release_all();
            } else if (strncmp(line, "CONSUMER ", 9) == 0) {
                unsigned int ccode = 0;
                if (sscanf(line + 9, "%x", &ccode) == 1) {
                    send_host_consumer_report((uint16_t)ccode);
                }
            } else if (strncmp(line, "MOUSE ", 6) == 0) {
                int btn = 0, dx = 0, dy = 0, wheel = 0;
                int count = sscanf(line + 6, "%d %d %d %d", &btn, &dx, &dy, &wheel);
                if (count >= 3) {
                    g_host_mouse_buttons = (uint8_t)(btn & 0x07);
                    send_host_mouse_report(g_host_mouse_buttons, mouse_delta(dx), mouse_delta(dy), mouse_delta(wheel));
                }
            } else if (strncmp(line, "MOUSE_MOVE ", 11) == 0) {
                int dx = 0, dy = 0;
                if (sscanf(line + 11, "%d %d", &dx, &dy) == 2) {
                    send_host_mouse_report(g_host_mouse_buttons, mouse_delta(dx), mouse_delta(dy), 0);
                }
            } else if (strncmp(line, "MOUSE_CLICK ", 12) == 0) {
                const char *p = line + 12;
                while (*p == ' ') p++;
                uint8_t b = 1;
                if (strcasecmp(p, "right") == 0 || strcmp(p, "2") == 0) b = 2;
                else if (strcasecmp(p, "middle") == 0 || strcmp(p, "3") == 0) b = 4;
                send_host_mouse_click(b);
            } else if (strncmp(line, "MOUSE_DOWN ", 11) == 0) {
                const char *p = line + 11;
                while (*p == ' ') p++;
                uint8_t b = 1;
                if (strcasecmp(p, "right") == 0 || strcmp(p, "2") == 0) b = 2;
                else if (strcasecmp(p, "middle") == 0 || strcmp(p, "3") == 0) b = 4;
                else if (atoi(p) > 0) b = (uint8_t)atoi(p);
                g_host_mouse_buttons |= b;
                send_host_mouse_report(g_host_mouse_buttons, 0, 0, 0);
            } else if (strncmp(line, "MOUSE_UP ", 9) == 0) {
                const char *p = line + 9;
                while (*p == ' ') p++;
                uint8_t b = 1;
                if (strcasecmp(p, "right") == 0 || strcmp(p, "2") == 0) b = 2;
                else if (strcasecmp(p, "middle") == 0 || strcmp(p, "3") == 0) b = 4;
                else if (atoi(p) > 0) b = (uint8_t)atoi(p);
                g_host_mouse_buttons &= ~b;
                send_host_mouse_report(g_host_mouse_buttons, 0, 0, 0);
            } else if (strncmp(line, "MOUSE_WHEEL ", 12) == 0) {
                int w = 0;
                if (sscanf(line + 12, "%d", &w) == 1) {
                    send_host_mouse_report(g_host_mouse_buttons, 0, 0, mouse_delta(w));
                }
            } else if (strncmp(line, "RAW ", 4) == 0) {
                uint8_t raw[16];
                int rlen = parse_hex_bytes(line + 4, raw, sizeof(raw));
                cur_cid = get_target_host_hid_cid();
                if (rlen > 0 && cur_cid != 0) {
                    bt_hid_send_report(cur_cid, raw, rlen);
                    g_keys_sent++;
                    update_bthid_status();
                    raw_report_sent(raw, rlen, cur_cid);
                }
            } else if (strchr(line, '|')) {
                // Pipe-separated report sequence: press|release
                char *pipe = strchr(line, '|');
                *pipe = 0;
                char *rel_str = pipe + 1;
                uint8_t press_buf[16];
                int press_len = parse_hex_bytes(line, press_buf, sizeof(press_buf));
                uint8_t rel_buf[16];
                int rel_len = parse_hex_bytes(rel_str, rel_buf, sizeof(rel_buf));
                cur_cid = get_target_host_hid_cid();
                if (cur_cid == 0) {
                    if (press_len >= 5 && press_buf[0] == 0xa1) {
                        char wake[24] = "";
                        if (press_buf[1] == 0x01 && press_buf[4] != 0) {
                            snprintf(wake, sizeof(wake), "KEY %02x %02x", press_buf[2], press_buf[4]);
                        } else if (press_buf[1] == 0x02 && (press_buf[2] != 0 || press_buf[3] != 0)) {
                            snprintf(wake, sizeof(wake), "CONSUMER %04x", (unsigned)(press_buf[2] | (press_buf[3] << 8)));
                        }
                        wake_key_keep(wake);
                    }
                    if (!g_host_reconnect_active && g_active_host_idx != BT_HOST_IDX_NONE) {
                        bd_addr_t tgt;
                        if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS && !is_null_bd_addr(g_hosts[g_active_host_idx].addr)) {
                            start_host_reconnect(g_hosts[g_active_host_idx].addr, 20);
                        } else if (get_pinned_host_target(tgt)) {
                            start_host_reconnect(tgt, 20);
                        } else {
                            try_connect_host();
                        }
                    }
                } else if (press_len > 0) {
                    if (g_host_pending_release_type != 0) {
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        send_host_release_all();
                    }
                    bt_hid_send_report(cur_cid, press_buf, press_len);
                    if (rel_len > 0) {
                        memcpy(g_host_custom_release_buf, rel_buf, rel_len);
                        g_host_custom_release_len = rel_len;
                        g_host_pending_cid = cur_cid;
                        g_host_pending_release_type = 3;
                        arm_host_key_release_timer(30);
                    } else {
                        raw_report_sent(press_buf, press_len, cur_cid);
                    }
                    g_keys_sent++;
                    update_bthid_status();
                }
            } else if ((strncasecmp(line, "A101", 4) == 0 || strncasecmp(line, "A102", 4) == 0 || strncasecmp(line, "A103", 4) == 0) && strlen(line) >= 8) {
                uint8_t rep[16];
                int rlen = parse_hex_bytes(line, rep, sizeof(rep));
                cur_cid = get_target_host_hid_cid();
                if (cur_cid == 0) {
                    if (rlen >= 5 && rep[0] == 0xa1) {
                        char wake[24] = "";
                        if (rep[1] == 0x01 && rep[4] != 0) {
                            snprintf(wake, sizeof(wake), "KEY %02x %02x", rep[2], rep[4]);
                        } else if (rep[1] == 0x02 && (rep[2] != 0 || rep[3] != 0)) {
                            snprintf(wake, sizeof(wake), "CONSUMER %04x", (unsigned)(rep[2] | (rep[3] << 8)));
                        }
                        wake_key_keep(wake);
                    }
                    if (!g_host_reconnect_active && g_active_host_idx != BT_HOST_IDX_NONE) {
                        bd_addr_t tgt;
                        if (g_active_host_idx >= 0 && g_active_host_idx < MAX_HOST_CONNS && !is_null_bd_addr(g_hosts[g_active_host_idx].addr)) {
                            start_host_reconnect(g_hosts[g_active_host_idx].addr, 20);
                        } else if (get_pinned_host_target(tgt)) {
                            start_host_reconnect(tgt, 20);
                        } else {
                            try_connect_host();
                        }
                    }
                } else if (rlen > 0) {
                    if (g_host_pending_release_type != 0) {
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        send_host_release_all();
                    }
                    bt_hid_send_report(cur_cid, rep, rlen);
                    g_host_pending_cid = cur_cid;
                    if (rep[1] == 0x01 && report_down_type(rep, rlen) == 1) {
                        g_host_pending_release_type = 1;
                        arm_host_key_release_timer(30);
                    } else if (rep[1] == 0x02 && (rep[2] != 0 || rep[3] != 0)) {
                        g_host_pending_release_type = 2;
                        arm_host_key_release_timer(30);
                    } else if (rep[1] == 0x03 && rlen > 2 && rep[2] != 0) {
                        g_host_pending_release_type = 4;
                        arm_host_key_release_timer(30);
                    }
                    g_keys_sent++;
                    update_bthid_status();
                }
            } else {
                if (!send_host_named_key(line)) {
                    send_host_text(line);
                }
            }
        }
        line = strtok_r(NULL, "\r\n", &save);
    }
}

void execute_btstack_cmd(const char *cmd_in) {
    if (!cmd_in) return;
    char cmd[128] = "";
    strncpy(cmd, cmd_in, sizeof(cmd) - 1);
    char *p = cmd + strlen(cmd) - 1;
    while (p >= cmd && (*p == '\r' || *p == '\n' || *p == ' ')) *p-- = 0;
    if (!cmd[0]) return;

    if (strncasecmp(cmd, "adv_listen ", 11) == 0) {
        bt_listen_handle_cmd(cmd + 11);
    } else if (strcasecmp(cmd, "stop_scan") == 0 || strcasecmp(cmd, "scan_off") == 0 || strcasecmp(cmd, "scan_stop") == 0) {
        stop_user_scan();
    } else if (strncasecmp(cmd, "scan", 4) == 0) {
        start_user_scan();
    } else if (strncasecmp(cmd, "host_forget", 11) == 0) {
        /* A classic host (POST /api/bt-forget-host). The word does not start with "forget": an
         * older hub_btstack, which reads any "forget..." as the BLE remote, ignores it. */
        bd_addr_t tgt;
        if (cmd_addr_arg(cmd + 11, tgt, false) != 1) {
            log_msg("host_forget: not a Bluetooth address, ignored");
            return;
        }
        forget_classic_host(tgt);
    } else if (strncasecmp(cmd, "forget", 6) == 0) {
        forget_ble_remote();
    } else if (strncasecmp(cmd, "pair ", 5) == 0) {
        char *addr_str = cmd + 5;
        while (*addr_str == ' ') addr_str++;
        start_user_pair(addr_str);
    } else if (strncasecmp(cmd, "pair_host_on", 12) == 0) {
        stop_host_reconnect();
        /* Disconnect the hosts the way disconnect_host does: close the HID channels and let the
         * close events update g_hosts[]. Tearing the ACL link down at once (gap_disconnect) and
         * clearing hid_cid by hand left BTstack's HID instance with a stale channel, so every
         * later hid_device_connect() to that host returned the old cid and did nothing until
         * hub_btstack restarted. Keep the link keys: pairing a new host must not unpair the
         * others; a host that forgot its key starts Secure Simple Pairing, which pairing mode
         * accepts. */
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_disconnect(g_hosts[i].hid_cid);
            }
        }
        if (g_host_connected && g_host_hid_cid) {
            hid_device_disconnect(g_host_hid_cid);
        }
        char *name = cmd + 12;
        while (*name == ' ') name++;
        if (name[0]) {
            strncpy(g_host_name, name, sizeof(g_host_name) - 1);
            gap_set_local_name(g_host_name);
        }
        g_host_pairing = true;
        g_pairing_new_key_valid = false;
        gap_set_class_of_device(0x0025C0); // Combo: Keyboard + Pointing Device
        gap_connectable_control(1);
        gap_discoverable_control(1);
        /* Pairing mode used to stay on until switched off, discoverable and accepting any
         * pairing request; end it after PAIRING_TIMEOUT_MS */
        btstack_run_loop_remove_timer(&g_pairing_timer);
        btstack_run_loop_set_timer_handler(&g_pairing_timer, pairing_timeout_handler);
        btstack_run_loop_set_timer(&g_pairing_timer, PAIRING_TIMEOUT_MS);
        btstack_run_loop_add_timer(&g_pairing_timer);
        char msg[128];
        snprintf(msg, sizeof(msg), "Classic HID Host pairing enabled as '%s' (%d s)", g_host_name, PAIRING_TIMEOUT_MS / 1000);
        log_msg(msg);
        update_status(app_state == APP_READY ? "ready" : "standby");
        update_bthid_status();
    } else if (strncasecmp(cmd, "pair_host_off", 13) == 0) {
        pairing_mode_off("requested");
    } else if (strncasecmp(cmd, "connect_host", 12) == 0) {
        char *addr_str = cmd + 12;
        while (*addr_str == ' ') addr_str++;
        bd_addr_t tgt;
        int has_addr = cmd_addr_arg(addr_str, tgt, true);
        if (has_addr < 0) {
            log_msg("connect_host: not a Bluetooth address, ignored");
            return;
        }
        if (g_host_connecting && (time(NULL) - g_host_connecting_start) < 5) {
            /* Another connection attempt is running: this host waits for its turn instead of
             * the command being dropped */
            if (has_addr && !is_host_connected(tgt)) {
                register_host_target(tgt, NULL);
                try_connect_host_addr(tgt); /* inside the window it queues the host */
            }
            return;
        }
        if (has_addr) {
            if (is_host_connected(tgt)) return;
            register_host_target(tgt, NULL);
            start_host_reconnect(tgt, MAX_HOST_RECONNECT_ATTEMPTS);
        } else {
            /* No address: the host keys go to (the activity's target, else the pinned host).
             * "Any host connected" used to count as done, which with two hosts left the one
             * that matters disconnected. */
            bd_addr_t def_tgt;
            if (get_reconnect_target(def_tgt)) {
                if (is_host_connected(def_tgt)) return;
                start_host_reconnect(def_tgt, MAX_HOST_RECONNECT_ATTEMPTS);
            } else if (!is_null_bd_addr(g_host_addr)) {
                if (is_host_connected(g_host_addr)) return;
                start_host_reconnect(g_host_addr, MAX_HOST_RECONNECT_ATTEMPTS);
            } else {
                try_connect_host();
            }
        }
    } else if (strncasecmp(cmd, "stop_reconnect", 14) == 0) {
        stop_host_reconnect();
        rq_clear(); /* the hosts waiting for their turn stop too */
        wake_key_drop(NULL); /* and no kept key goes out at a later connection */
    } else if (strncasecmp(cmd, "disconnect_host", 15) == 0) {
        /* A disconnect that was asked for must not be undone by the auto-reconnect on the close
         * event; a key for the host, connect_host or the next activity start reconnects it.
         * Per host: disconnecting one host must not stop another from auto-reconnecting. */
        char *addr_str = cmd + 15;
        while (*addr_str == ' ') addr_str++;
        bd_addr_t tgt;
        int has_addr = cmd_addr_arg(addr_str, tgt, false);
        if (has_addr < 0) {
            log_msg("disconnect_host: not a Bluetooth address, ignored");
            return;
        }
        if (has_addr) {
            set_host_manual_disconnect(tgt, true);
            rq_remove(tgt);
            /* Stop the reconnect loop only when it is for this host (or for every host); a loop
             * for another host goes on. A stopped loop lets the next waiting host start. */
            if (g_host_reconnect_active &&
                (is_null_bd_addr(g_host_reconnect_target) || memcmp(g_host_reconnect_target, tgt, 6) == 0)) {
                stop_host_reconnect();
                host_reconnect_next();
            }
            for (int i = 0; i < MAX_HOST_CONNS; i++) {
                if (memcmp(g_hosts[i].addr, tgt, 6) == 0 && g_hosts[i].hid_cid) {
                    hid_device_disconnect(g_hosts[i].hid_cid);
                    char dmsg[64];
                    snprintf(dmsg, sizeof(dmsg), "Host %s disconnect requested", addr_str);
                    log_msg(dmsg);
                }
            }
        } else {
            bd_addr_t all_hosts;
            memset(all_hosts, 0, sizeof(all_hosts));
            stop_host_reconnect();
            set_host_manual_disconnect(all_hosts, true);
            rq_clear();
            for (int i = 0; i < MAX_HOST_CONNS; i++) {
                if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                    hid_device_disconnect(g_hosts[i].hid_cid);
                }
            }
            if (g_host_connected && g_host_hid_cid) {
                hid_device_disconnect(g_host_hid_cid);
            }
            log_msg("Host disconnect requested for all");
        }
    } else if (strncasecmp(cmd, "adv ", 4) == 0) {
        bt_adv_handle_cmd(cmd + 4);
    } else if (strncmp(cmd, "key ", 4) == 0) {
        send_host_named_key(cmd + 4);
    } else if (strncmp(cmd, "text ", 5) == 0) {
        send_host_text(cmd + 5);
    } else if (strcasecmp(cmd, "reload_map") == 0) {
        bt_remote_invalidate_map();
        log_msg("BLE remote map invalidated");
        if (key_store_replaced()) {
            /* hub_daemon sends reload_map after a restore: start again (the supervisor does)
             * so that the restored key store is the one in use */
            log_msg("Key store replaced on disk (restore): restarting to load it");
            g_running = 0;
            /* leave the run loop now; cmd_poll_timeout() would also do it within 100 ms */
            btstack_run_loop_trigger_exit();
        }
    } else if (strncasecmp(cmd, "bt_page", 7) == 0) {
        char *p = cmd + 7;
        while (*p == ' ') p++;
        char target_str[32] = "";
        char spoof_str[32] = "";
        sscanf(p, "%31s %31s", target_str, spoof_str);
        bd_addr_t tgt;
        if (!parse_bd_addr(target_str, tgt)) {
            log_msg("bt_page: invalid target address");
            return;
        }
        bt_page(tgt, spoof_str[0] ? spoof_str : NULL);
    }
}

static void cmd_sock_process(btstack_data_source_t *ds, btstack_data_source_callback_type_t callback_type) {
    (void)ds;
    if (callback_type == DATA_SOURCE_CALLBACK_READ && g_cmd_sock_fd >= 0) {
        char buf[256];
        ssize_t n = recvfrom(g_cmd_sock_fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n > 0) {
            buf[n] = 0;
            execute_btstack_cmd(buf);
        }
    }
}

void init_cmd_socket(void) {
    unlink(CMD_SOCK);
    g_cmd_sock_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (g_cmd_sock_fd < 0) {
        fprintf(stderr, "[hub_btstack] Failed to create IPC socket: %s\n", strerror(errno));
        return;
    }
    fcntl(g_cmd_sock_fd, F_SETFD, FD_CLOEXEC);
    fcntl(g_cmd_sock_fd, F_SETFL, fcntl(g_cmd_sock_fd, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, CMD_SOCK, sizeof(sun.sun_path) - 1);
    if (bind(g_cmd_sock_fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        fprintf(stderr, "[hub_btstack] Failed to bind IPC socket %s: %s\n", CMD_SOCK, strerror(errno));
        close(g_cmd_sock_fd);
        g_cmd_sock_fd = -1;
        return;
    }
    chmod(CMD_SOCK, 0660);
    s_tlv_at_start_ok = (stat(TLV_DB_PATH, &s_tlv_at_start) == 0);

    btstack_run_loop_set_data_source_fd(&g_cmd_sock_ds, g_cmd_sock_fd);
    btstack_run_loop_set_data_source_handler(&g_cmd_sock_ds, &cmd_sock_process);
    btstack_run_loop_add_data_source(&g_cmd_sock_ds);
    btstack_run_loop_enable_data_source_callbacks(&g_cmd_sock_ds, DATA_SOURCE_CALLBACK_READ);
    log_msg("IPC command socket initialized: " CMD_SOCK);
}

void cleanup_cmd_socket(void) {
    if (g_cmd_sock_fd >= 0) {
        btstack_run_loop_remove_data_source(&g_cmd_sock_ds);
        close(g_cmd_sock_fd);
        g_cmd_sock_fd = -1;
        unlink(CMD_SOCK);
    }
}

void check_command_file(void) {
    poll_bthid_fifo();

    if (g_user_scan_active && time(NULL) >= g_user_scan_end) {
        g_user_scan_active = false;
        log_msg("BLE remote scan window ended");
        write_scan_results(false);
        gap_stop_scan();
        hog_start_connect();
        return;
    }

    struct stat cst;
    if (stat(CMD_FILE, &cst) != 0) return;
    time_t now = time(NULL);
    if (now - cst.st_mtime > CMD_FILE_MAX_AGE_S || cst.st_mtime - now > CMD_FILE_MAX_AGE_S) {
        unlink(CMD_FILE);
        log_msg("Stale command file dropped (written while hub_btstack was not running)");
        return;
    }
    FILE *f = fopen(CMD_FILE, "r");
    if (!f) return;
    char cmd[128] = "";
    if (fgets(cmd, sizeof(cmd), f)) {
        char *p = cmd + strlen(cmd) - 1;
        while (p >= cmd && (*p == '\r' || *p == '\n' || *p == ' ')) *p-- = 0;
    }
    fclose(f);
    unlink(CMD_FILE);

    if (cmd[0]) {
        execute_btstack_cmd(cmd);
    }
}

void cmd_poll_timeout(btstack_timer_source_t *ts) {
    if (!g_running) {
        btstack_run_loop_trigger_exit();
        return;
    }
    check_command_file();
    btstack_run_loop_remove_timer(ts);
    btstack_run_loop_set_timer(ts, 100);
    btstack_run_loop_add_timer(ts);
}
