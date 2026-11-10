#include "bt_cmd.h"
#include "bt_pairing.h"
#include "bt_hid.h"
#include "bt_remote.h"

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

void poll_bthid_fifo(void) {
    if (g_bthid_fifo_fd < 0) {
        struct stat st;
        if (stat(BTHID_FIFO, &st) == 0 && !S_ISFIFO(st.st_mode)) unlink(BTHID_FIFO);
        if (stat(BTHID_FIFO, &st) != 0) mkfifo(BTHID_FIFO, 0666);
        chmod(BTHID_FIFO, 0666);
        g_bthid_fifo_fd = open(BTHID_FIFO, O_RDWR | O_NONBLOCK);
        if (g_bthid_fifo_fd < 0) return;
    }

    char buf[1024];
    ssize_t n = read(g_bthid_fifo_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;

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
            if (strncmp(line, "TARGET ", 7) == 0) {
                bd_addr_t t_addr;
                if (parse_bd_addr(line + 7, t_addr)) {
                    register_host_target(t_addr, NULL);
                    for (int i = 0; i < MAX_HOST_CONNS; i++) {
                        if (memcmp(g_hosts[i].addr, t_addr, 6) == 0) {
                            g_active_host_idx = i;
                            if (!g_hosts[i].connected || g_hosts[i].hid_cid == 0) {
                                if (!g_host_reconnect_active) {
                                    start_host_reconnect(t_addr, MAX_HOST_RECONNECT_ATTEMPTS);
                                } else {
                                    try_connect_host_addr(t_addr);
                                }
                            }
                            break;
                        }
                    }
                }
                line = strtok_r(NULL, "\r\n", &save);
                continue;
            }
            uint16_t cur_cid = get_target_host_hid_cid();
            if (cur_cid == 0) {
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
                    g_host_mouse_buttons = (uint8_t)btn;
                    send_host_mouse_report(g_host_mouse_buttons, (int8_t)dx, (int8_t)dy, (int8_t)wheel);
                }
            } else if (strncmp(line, "MOUSE_MOVE ", 11) == 0) {
                int dx = 0, dy = 0;
                if (sscanf(line + 11, "%d %d", &dx, &dy) == 2) {
                    send_host_mouse_report(g_host_mouse_buttons, (int8_t)dx, (int8_t)dy, 0);
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
                    send_host_mouse_report(g_host_mouse_buttons, 0, 0, (int8_t)w);
                }
            } else if (strncmp(line, "RAW ", 4) == 0) {
                uint8_t raw[16];
                int rlen = parse_hex_bytes(line + 4, raw, sizeof(raw));
                cur_cid = get_target_host_hid_cid();
                if (rlen > 0 && cur_cid != 0) {
                    hid_device_send_interrupt_message(cur_cid, raw, rlen);
                    g_keys_sent++;
                    update_bthid_status();
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
                    if (!g_pending_wake_btn[0] && press_len >= 5 && (press_buf[0] == 0xa1 || press_buf[0] == 0xA1)) {
                        if (press_buf[1] == 0x01 && press_buf[4] != 0) {
                            snprintf(g_pending_wake_btn, sizeof(g_pending_wake_btn), "KEY %02x %02x", press_buf[2], press_buf[4]);
                            g_pending_wake_time = time(NULL);
                        } else if (press_buf[1] == 0x02 && (press_buf[2] != 0 || press_buf[3] != 0)) {
                            uint16_t usage = press_buf[2] | (press_buf[3] << 8);
                            snprintf(g_pending_wake_btn, sizeof(g_pending_wake_btn), "CONSUMER %04x", usage);
                            g_pending_wake_time = time(NULL);
                        }
                    }
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
                } else if (press_len > 0) {
                    if (g_host_pending_release_type != 0) {
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        send_host_release_all();
                    }
                    hid_device_send_interrupt_message(cur_cid, press_buf, press_len);
                    if (rel_len > 0) {
                        memcpy(g_host_custom_release_buf, rel_buf, rel_len);
                        g_host_custom_release_len = rel_len;
                        g_host_pending_cid = cur_cid;
                        g_host_pending_release_type = 3;
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        btstack_run_loop_set_timer(&g_host_key_release_timer, 30);
                        btstack_run_loop_add_timer(&g_host_key_release_timer);
                    }
                    g_keys_sent++;
                    update_bthid_status();
                }
            } else if ((strncasecmp(line, "A101", 4) == 0 || strncasecmp(line, "A102", 4) == 0 || strncasecmp(line, "A103", 4) == 0) && strlen(line) >= 8) {
                uint8_t rep[16];
                int rlen = parse_hex_bytes(line, rep, sizeof(rep));
                cur_cid = get_target_host_hid_cid();
                if (cur_cid == 0) {
                    if (!g_pending_wake_btn[0] && rlen >= 5 && (rep[0] == 0xa1 || rep[0] == 0xA1)) {
                        if (rep[1] == 0x01 && rep[4] != 0) {
                            snprintf(g_pending_wake_btn, sizeof(g_pending_wake_btn), "KEY %02x %02x", rep[2], rep[4]);
                            g_pending_wake_time = time(NULL);
                        } else if (rep[1] == 0x02 && (rep[2] != 0 || rep[3] != 0)) {
                            uint16_t usage = rep[2] | (rep[3] << 8);
                            snprintf(g_pending_wake_btn, sizeof(g_pending_wake_btn), "CONSUMER %04x", usage);
                            g_pending_wake_time = time(NULL);
                        }
                    }
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
                } else if (rlen > 0) {
                    if (g_host_pending_release_type != 0) {
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        send_host_release_all();
                    }
                    hid_device_send_interrupt_message(cur_cid, rep, rlen);
                    g_host_pending_cid = cur_cid;
                    if (rep[1] == 0x01 && (rlen > 4 && rep[4] != 0)) {
                        g_host_pending_release_type = 1;
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        btstack_run_loop_set_timer(&g_host_key_release_timer, 30);
                        btstack_run_loop_add_timer(&g_host_key_release_timer);
                    } else if (rep[1] == 0x02 && (rep[2] != 0 || rep[3] != 0)) {
                        g_host_pending_release_type = 2;
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        btstack_run_loop_set_timer(&g_host_key_release_timer, 30);
                        btstack_run_loop_add_timer(&g_host_key_release_timer);
                    } else if (rep[1] == 0x03 && rlen > 2 && rep[2] != 0) {
                        g_host_pending_release_type = 4;
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        btstack_run_loop_set_timer(&g_host_key_release_timer, 30);
                        btstack_run_loop_add_timer(&g_host_key_release_timer);
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

    if (strcasecmp(cmd, "stop_scan") == 0 || strcasecmp(cmd, "scan_off") == 0 || strcasecmp(cmd, "scan_stop") == 0) {
        stop_user_scan();
    } else if (strncasecmp(cmd, "scan", 4) == 0) {
        start_user_scan();
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
        if (g_host_connecting && (time(NULL) - g_host_connecting_start) < 5) return;
        char *addr_str = cmd + 12;
        while (*addr_str == ' ') addr_str++;
        bd_addr_t tgt;
        if (parse_bd_addr(addr_str, tgt)) {
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
    } else if (strncasecmp(cmd, "disconnect_host", 15) == 0) {
        /* A disconnect that was asked for must not be undone by the auto-reconnect on the close
         * event; a key for the host, connect_host or the next activity start reconnects it.
         * Per host: disconnecting one host must not stop another from auto-reconnecting. */
        char *addr_str = cmd + 15;
        while (*addr_str == ' ') addr_str++;
        bd_addr_t tgt;
        if (parse_bd_addr(addr_str, tgt)) {
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
    } else if (strncmp(cmd, "key ", 4) == 0) {
        send_host_named_key(cmd + 4);
    } else if (strncmp(cmd, "text ", 5) == 0) {
        send_host_text(cmd + 5);
    } else if (strcasecmp(cmd, "reload_map") == 0) {
        bt_remote_invalidate_map();
        log_msg("BLE remote map invalidated");
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

    if (access(CMD_FILE, F_OK) != 0) return;
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
