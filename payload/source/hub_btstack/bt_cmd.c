#include "bt_cmd.h"
#include "bt_pairing.h"
#include "bt_hid.h"

btstack_timer_source_t cmd_poll_timer;
static int g_cmd_sock_fd = -1;
static btstack_data_source_t g_cmd_sock_ds;

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
            if (strncmp(line, "pair_host_on", 12) == 0 ||
                strncmp(line, "pair_host_off", 13) == 0 ||
                strncmp(line, "connect_host", 12) == 0 ||
                strncmp(line, "disconnect_host", 15) == 0 ||
                strncmp(line, "scan", 4) == 0 ||
                strncmp(line, "pair ", 5) == 0) {
                execute_btstack_cmd(line);
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
                if (press_len > 0 && cur_cid != 0) {
                    if (g_host_pending_release_type != 0) {
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        send_host_release_all();
                    }
                    hid_device_send_interrupt_message(cur_cid, press_buf, press_len);
                    if (rel_len > 0) {
                        memcpy(g_host_custom_release_buf, rel_buf, rel_len);
                        g_host_custom_release_len = rel_len;
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
                if (rlen > 0 && cur_cid != 0) {
                    if (g_host_pending_release_type != 0) {
                        btstack_run_loop_remove_timer(&g_host_key_release_timer);
                        send_host_release_all();
                    }
                    hid_device_send_interrupt_message(cur_cid, rep, rlen);
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

    if (strncmp(cmd, "scan", 4) == 0) {
        start_user_scan();
    } else if (strncmp(cmd, "pair ", 5) == 0) {
        char *addr_str = cmd + 5;
        while (*addr_str == ' ') addr_str++;
        start_user_pair(addr_str);
    } else if (strncmp(cmd, "pair_host_on", 12) == 0) {
        stop_host_reconnect();
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].hid_cid) {
                hid_device_disconnect(g_hosts[i].hid_cid);
                g_hosts[i].hid_cid = 0;
            }
            if (g_hosts[i].acl_handle != HCI_CON_HANDLE_INVALID) {
                gap_disconnect(g_hosts[i].acl_handle);
                g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
            }
            if (!is_null_bd_addr(g_hosts[i].addr)) {
                gap_drop_link_key_for_bd_addr(g_hosts[i].addr);
            }
            g_hosts[i].connected = false;
        }
        if (g_host_hid_cid) {
            hid_device_disconnect(g_host_hid_cid);
            g_host_hid_cid = 0;
        }
        if (!is_null_bd_addr(g_host_addr)) {
            gap_drop_link_key_for_bd_addr(g_host_addr);
        }
        g_host_connected = false;
        char *name = cmd + 12;
        while (*name == ' ') name++;
        if (name[0]) {
            strncpy(g_host_name, name, sizeof(g_host_name) - 1);
            gap_set_local_name(g_host_name);
        }
        g_host_pairing = true;
        gap_set_class_of_device(0x0025C0); // Combo: Keyboard + Pointing Device
        gap_connectable_control(1);
        gap_discoverable_control(1);
        char msg[128];
        snprintf(msg, sizeof(msg), "Classic HID Host pairing enabled as '%s'", g_host_name);
        log_msg(msg);
        update_status(app_state == APP_READY ? "ready" : "standby");
        update_bthid_status();
    } else if (strncmp(cmd, "pair_host_off", 13) == 0) {
        g_host_pairing = false;
        gap_discoverable_control(0);
        log_msg("Classic HID Host pairing disabled");
        update_status(app_state == APP_READY ? "ready" : "standby");
        update_bthid_status();
    } else if (strncmp(cmd, "connect_host", 12) == 0) {
        char *addr_str = cmd + 12;
        while (*addr_str == ' ') addr_str++;
        bd_addr_t tgt;
        stop_host_reconnect();
        g_host_connecting = false;
        if (parse_bd_addr(addr_str, tgt)) {
            register_host_target(tgt, NULL);
            start_host_reconnect(tgt, MAX_HOST_RECONNECT_ATTEMPTS);
        } else {
            bd_addr_t def_tgt;
            if (get_pinned_host_target(def_tgt)) {
                start_host_reconnect(def_tgt, MAX_HOST_RECONNECT_ATTEMPTS);
            } else {
                start_host_reconnect(g_host_addr, MAX_HOST_RECONNECT_ATTEMPTS);
            }
        }
    } else if (strncmp(cmd, "disconnect_host", 15) == 0) {
        stop_host_reconnect();
        char *addr_str = cmd + 15;
        while (*addr_str == ' ') addr_str++;
        bd_addr_t tgt;
        if (parse_bd_addr(addr_str, tgt)) {
            for (int i = 0; i < MAX_HOST_CONNS; i++) {
                if (memcmp(g_hosts[i].addr, tgt, 6) == 0 && g_hosts[i].hid_cid) {
                    hid_device_disconnect(g_hosts[i].hid_cid);
                    char dmsg[64];
                    snprintf(dmsg, sizeof(dmsg), "Host %s disconnect requested", addr_str);
                    log_msg(dmsg);
                }
            }
        } else {
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
    chmod(CMD_SOCK, 0666);

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
