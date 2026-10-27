#include "bt_hid.h"
#include "bt_pairing.h"

btstack_timer_source_t g_host_key_release_timer;
uint8_t g_host_pending_release_type = 0; // 1 = keyboard, 2 = consumer, 3 = custom
uint8_t g_host_custom_release_buf[16];
int     g_host_custom_release_len = 0;
int     g_host_send_hold_mode = 0;
uint8_t g_host_mouse_buttons = 0;

static btstack_timer_source_t g_host_crelease_timer;

static void host_key_release_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) return;
    if (g_host_pending_release_type == 1) {
        uint8_t release[10] = {0xa1, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
        hid_device_send_interrupt_message(cid, release, sizeof(release));
    } else if (g_host_pending_release_type == 2) {
        uint8_t release[4] = {0xa1, 0x02, 0, 0};
        hid_device_send_interrupt_message(cid, release, sizeof(release));
    } else if (g_host_pending_release_type == 3 && g_host_custom_release_len > 0) {
        hid_device_send_interrupt_message(cid, g_host_custom_release_buf, g_host_custom_release_len);
    } else if (g_host_pending_release_type == 4) {
        uint8_t mrelease[6] = {0xa1, 0x03, 0, 0, 0, 0};
        hid_device_send_interrupt_message(cid, mrelease, sizeof(mrelease));
        g_host_mouse_buttons = 0;
    }
    g_host_pending_release_type = 0;
}

static void host_crelease_timer_handler(btstack_timer_source_t *ts) {
    UNUSED(ts);
    uint16_t cid = get_target_host_hid_cid();
    if (cid != 0) {
        uint8_t crelease[4] = {0xa1, 0x02, 0, 0};
        hid_device_send_interrupt_message(cid, crelease, sizeof(crelease));
    }
}

void send_host_release_all(void) {
    uint8_t prev_type = g_host_pending_release_type;
    if (g_host_pending_release_type != 0) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        g_host_pending_release_type = 0;
    }
    if (!is_any_host_connected()) return;

    if (prev_type == 2) {
        uint8_t crelease[4] = {0xa1, 0x02, 0, 0};
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_send_interrupt_message(g_hosts[i].hid_cid, crelease, sizeof(crelease));
            }
        }
    } else if (prev_type == 1) {
        uint8_t release[10] = {0xa1, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_send_interrupt_message(g_hosts[i].hid_cid, release, sizeof(release));
            }
        }
    } else if (prev_type == 3 && g_host_custom_release_len > 0) {
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_send_interrupt_message(g_hosts[i].hid_cid, g_host_custom_release_buf, g_host_custom_release_len);
            }
        }
    } else if (prev_type == 4 || g_host_mouse_buttons != 0) {
        uint8_t mrelease[6] = {0xa1, 0x03, 0, 0, 0, 0};
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_send_interrupt_message(g_hosts[i].hid_cid, mrelease, sizeof(mrelease));
            }
        }
        g_host_mouse_buttons = 0;
    } else {
        uint8_t release[10] = {0xa1, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_send_interrupt_message(g_hosts[i].hid_cid, release, sizeof(release));
            }
        }
        btstack_run_loop_remove_timer(&g_host_crelease_timer);
        btstack_run_loop_set_timer_handler(&g_host_crelease_timer, host_crelease_timer_handler);
        btstack_run_loop_set_timer(&g_host_crelease_timer, 20);
        btstack_run_loop_add_timer(&g_host_crelease_timer);
    }
    if (g_host_mouse_buttons != 0) {
        uint8_t mrelease[6] = {0xa1, 0x03, 0, 0, 0, 0};
        for (int i = 0; i < MAX_HOST_CONNS; i++) {
            if (g_hosts[i].connected && g_hosts[i].hid_cid) {
                hid_device_send_interrupt_message(g_hosts[i].hid_cid, mrelease, sizeof(mrelease));
            }
        }
        g_host_mouse_buttons = 0;
    }
}

void send_host_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) {
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
        return;
    }
    uint8_t report[6] = {0xa1, 0x03, buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};
    hid_device_send_interrupt_message(cid, report, sizeof(report));
    g_keys_sent++;
    update_bthid_status();
}

void send_host_mouse_click(uint8_t button) {
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) return;
    if (g_host_pending_release_type != 0) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        host_key_release_timer_handler(NULL);
    }
    g_host_mouse_buttons |= button;
    send_host_mouse_report(g_host_mouse_buttons, 0, 0, 0);
    g_host_pending_release_type = 4;
    btstack_run_loop_remove_timer(&g_host_key_release_timer);
    btstack_run_loop_set_timer_handler(&g_host_key_release_timer, host_key_release_timer_handler);
    btstack_run_loop_set_timer(&g_host_key_release_timer, 40);
    btstack_run_loop_add_timer(&g_host_key_release_timer);
}

void send_host_keyboard_report(uint8_t modifier, uint8_t keycode) {
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) {
        if (!g_pending_wake_btn[0]) {
            snprintf(g_pending_wake_btn, sizeof(g_pending_wake_btn), "KEY %02x %02x", modifier, keycode);
            g_pending_wake_time = time(NULL);
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
        return;
    }
    if (g_host_pending_release_type != 0) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        host_key_release_timer_handler(NULL);
    }
    uint8_t press[10] = {0xa1, 0x01, modifier, 0, keycode, 0, 0, 0, 0, 0};
    hid_device_send_interrupt_message(cid, press, sizeof(press));
    g_host_pending_release_type = 1;
    btstack_run_loop_remove_timer(&g_host_key_release_timer);
    btstack_run_loop_set_timer_handler(&g_host_key_release_timer, host_key_release_timer_handler);
    btstack_run_loop_set_timer(&g_host_key_release_timer, g_host_send_hold_mode ? 10000 : 40);
    btstack_run_loop_add_timer(&g_host_key_release_timer);
    g_keys_sent++;
    update_bthid_status();
}

void send_host_consumer_report(uint16_t usage_id) {
    uint16_t cid = get_target_host_hid_cid();
    if (cid == 0) {
        if (!g_pending_wake_btn[0]) {
            snprintf(g_pending_wake_btn, sizeof(g_pending_wake_btn), "CONSUMER %04x", usage_id);
            g_pending_wake_time = time(NULL);
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
        return;
    }
    if (g_host_pending_release_type != 0) {
        btstack_run_loop_remove_timer(&g_host_key_release_timer);
        host_key_release_timer_handler(NULL);
    }
    uint8_t press[4] = {0xa1, 0x02, (uint8_t)(usage_id & 0xff), (uint8_t)((usage_id >> 8) & 0xff)};
    hid_device_send_interrupt_message(cid, press, sizeof(press));
    g_host_pending_release_type = 2;
    btstack_run_loop_remove_timer(&g_host_key_release_timer);
    btstack_run_loop_set_timer_handler(&g_host_key_release_timer, host_key_release_timer_handler);
    btstack_run_loop_set_timer(&g_host_key_release_timer, g_host_send_hold_mode ? 10000 : 40);
    btstack_run_loop_add_timer(&g_host_key_release_timer);
    g_keys_sent++;
    update_bthid_status();
}

int parse_hex_bytes(const char *hex, uint8_t *out, int max_len) {
    int len = 0;
    while (*hex && len < max_len) {
        while (*hex == ' ') hex++;
        if (!*hex || *hex == '|' || *hex == '\r' || *hex == '\n') break;
        unsigned int b = 0;
        if (sscanf(hex, "%02x", &b) == 1 || sscanf(hex, "%02X", &b) == 1) {
            out[len++] = (uint8_t)b;
            hex += 2;
        } else {
            break;
        }
    }
    return len;
}

bool send_host_named_key(const char *name) {
    if (!name || !name[0]) return false;
    char msg[128];
    snprintf(msg, sizeof(msg), "Host key send: %s", name);
    hub_bt_log_debug(msg);

    if (strcasecmp(name, "release_all") == 0 || strcasecmp(name, "release") == 0) {
        send_host_release_all();
        return true;
    }

    uint8_t mod = 0;
    const char *p = name;
    while (*p) {
        if (strncasecmp(p, "ctrl+", 5) == 0) { mod |= 0x01; p += 5; }
        else if (strncasecmp(p, "ctrl", 4) == 0 && strlen(p) > 4) { mod |= 0x01; p += 4; }
        else if (strncasecmp(p, "shift+", 6) == 0) { mod |= 0x02; p += 6; }
        else if (strncasecmp(p, "shift", 5) == 0 && strlen(p) > 5) { mod |= 0x02; p += 5; }
        else if (strncasecmp(p, "alt+", 4) == 0) { mod |= 0x04; p += 4; }
        else if (strncasecmp(p, "alt", 3) == 0 && strlen(p) > 3) { mod |= 0x04; p += 3; }
        else if (strncasecmp(p, "win+", 4) == 0 || strncasecmp(p, "gui+", 4) == 0 || strncasecmp(p, "cmd+", 4) == 0) { mod |= 0x08; p += 4; }
        else if ((strncasecmp(p, "win", 3) == 0 || strncasecmp(p, "gui", 3) == 0 || strncasecmp(p, "cmd", 3) == 0) && strlen(p) > 3) { mod |= 0x08; p += 3; }
        else break;
    }
    name = p;

    // D-Pad / Navigation
    if (strcasecmp(name, "up") == 0 || strcasecmp(name, "DirectionUp") == 0 || strcasecmp(name, "ArrowUp") == 0) { send_host_keyboard_report(mod, 0x52); return true; }
    if (strcasecmp(name, "down") == 0 || strcasecmp(name, "DirectionDown") == 0 || strcasecmp(name, "ArrowDown") == 0) { send_host_keyboard_report(mod, 0x51); return true; }
    if (strcasecmp(name, "left") == 0 || strcasecmp(name, "DirectionLeft") == 0 || strcasecmp(name, "ArrowLeft") == 0) { send_host_keyboard_report(mod, 0x50); return true; }
    if (strcasecmp(name, "right") == 0 || strcasecmp(name, "DirectionRight") == 0 || strcasecmp(name, "ArrowRight") == 0) { send_host_keyboard_report(mod, 0x4f); return true; }
    if (strcasecmp(name, "select") == 0 || strcasecmp(name, "enter") == 0 || strcasecmp(name, "OK") == 0 || strcasecmp(name, "Enter") == 0 || strcasecmp(name, "Select") == 0) { send_host_keyboard_report(mod, 0x28); return true; }
    if (strcasecmp(name, "back") == 0 || strcasecmp(name, "Return") == 0 || strcasecmp(name, "Exit") == 0 || strcasecmp(name, "Back") == 0) { send_host_consumer_report(0x0224); return true; } // Android Back
    if (strcasecmp(name, "home") == 0 || strcasecmp(name, "Home") == 0) { send_host_consumer_report(0x0223); return true; } // Android Home
    if (strcasecmp(name, "menu") == 0 || strcasecmp(name, "Menu") == 0 || strcasecmp(name, "settings") == 0 || strcasecmp(name, "Settings") == 0 || strcasecmp(name, "guide") == 0 || strcasecmp(name, "Guide") == 0) { send_host_consumer_report(0x008D); return true; }
    if (strcasecmp(name, "info") == 0 || strcasecmp(name, "Info") == 0) { send_host_consumer_report(0x01BD); return true; }

    // Media Controls
    if (strcasecmp(name, "play") == 0 || strcasecmp(name, "Play") == 0) { send_host_consumer_report(0x00B0); return true; }
    if (strcasecmp(name, "pause") == 0 || strcasecmp(name, "Pause") == 0) { send_host_consumer_report(0x00B1); return true; }
    if (strcasecmp(name, "play_pause") == 0 || strcasecmp(name, "PlayPause") == 0) { send_host_consumer_report(0x00CD); return true; }
    if (strcasecmp(name, "stop") == 0 || strcasecmp(name, "Stop") == 0) { send_host_consumer_report(0x00B7); return true; }
    if (strcasecmp(name, "next") == 0 || strcasecmp(name, "SkipForward") == 0 || strcasecmp(name, "NextTrack") == 0) { send_host_consumer_report(0x00B5); return true; }
    if (strcasecmp(name, "prev") == 0 || strcasecmp(name, "SkipBack") == 0 || strcasecmp(name, "PreviousTrack") == 0) { send_host_consumer_report(0x00B6); return true; }
    if (strcasecmp(name, "rewind") == 0 || strcasecmp(name, "Rewind") == 0) { send_host_consumer_report(0x00B4); return true; }
    if (strcasecmp(name, "fastforward") == 0 || strcasecmp(name, "FastForward") == 0) { send_host_consumer_report(0x00B3); return true; }

    // Audio & Power & Channels
    if (strcasecmp(name, "vol_up") == 0 || strcasecmp(name, "VolumeUp") == 0) { send_host_consumer_report(0x00E9); return true; }
    if (strcasecmp(name, "vol_down") == 0 || strcasecmp(name, "VolumeDown") == 0) { send_host_consumer_report(0x00EA); return true; }
    if (strcasecmp(name, "mute") == 0 || strcasecmp(name, "Mute") == 0) { send_host_consumer_report(0x00E2); return true; }
    if (strcasecmp(name, "power") == 0 || strcasecmp(name, "PowerToggle") == 0) { send_host_consumer_report(0x0030); return true; }
    if (strcasecmp(name, "ch_up") == 0 || strcasecmp(name, "ChannelUp") == 0) { send_host_consumer_report(0x009C); return true; }
    if (strcasecmp(name, "ch_down") == 0 || strcasecmp(name, "ChannelDown") == 0) { send_host_consumer_report(0x009D); return true; }

    // Color Keys (Android TV standard / Red, Green, Yellow, Blue)
    if (strcasecmp(name, "red") == 0) { send_host_consumer_report(0x0069); return true; }
    if (strcasecmp(name, "green") == 0) { send_host_consumer_report(0x006A); return true; }
    if (strcasecmp(name, "yellow") == 0) { send_host_consumer_report(0x006C); return true; }
    if (strcasecmp(name, "blue") == 0) { send_host_consumer_report(0x006B); return true; }

    // Digits
    if (strcmp(name, "0") == 0 || strcasecmp(name, "Number0") == 0) { send_host_keyboard_report(mod, 0x27); return true; }
    if (strcmp(name, "1") == 0 || strcasecmp(name, "Number1") == 0) { send_host_keyboard_report(mod, 0x1E); return true; }
    if (strcmp(name, "2") == 0 || strcasecmp(name, "Number2") == 0) { send_host_keyboard_report(mod, 0x1F); return true; }
    if (strcmp(name, "3") == 0 || strcasecmp(name, "Number3") == 0) { send_host_keyboard_report(mod, 0x20); return true; }
    if (strcmp(name, "4") == 0 || strcasecmp(name, "Number4") == 0) { send_host_keyboard_report(mod, 0x21); return true; }
    if (strcmp(name, "5") == 0 || strcasecmp(name, "Number5") == 0) { send_host_keyboard_report(mod, 0x22); return true; }
    if (strcmp(name, "6") == 0 || strcasecmp(name, "Number6") == 0) { send_host_keyboard_report(mod, 0x23); return true; }
    if (strcmp(name, "7") == 0 || strcasecmp(name, "Number7") == 0) { send_host_keyboard_report(mod, 0x24); return true; }
    if (strcmp(name, "8") == 0 || strcasecmp(name, "Number8") == 0) { send_host_keyboard_report(mod, 0x25); return true; }
    if (strcmp(name, "9") == 0 || strcasecmp(name, "Number9") == 0) { send_host_keyboard_report(mod, 0x26); return true; }

    // Keyboard keys
    if (strcasecmp(name, "escape") == 0 || strcasecmp(name, "esc") == 0) { send_host_keyboard_report(mod, 0x29); return true; }
    if (strcasecmp(name, "backspace") == 0) { send_host_keyboard_report(mod, 0x2a); return true; }
    if (strcasecmp(name, "tab") == 0) { send_host_keyboard_report(mod, 0x2b); return true; }
    if (strcasecmp(name, "space") == 0) { send_host_keyboard_report(mod, 0x2c); return true; }
    if (strcasecmp(name, "insert") == 0) { send_host_keyboard_report(mod, 0x49); return true; }
    if (strcasecmp(name, "delete") == 0) { send_host_keyboard_report(mod, 0x4c); return true; }
    if (strcasecmp(name, "pageup") == 0) { send_host_keyboard_report(mod, 0x4b); return true; }
    if (strcasecmp(name, "pagedown") == 0) { send_host_keyboard_report(mod, 0x4e); return true; }
    if (strcasecmp(name, "end") == 0) { send_host_keyboard_report(mod, 0x4d); return true; }
    if (strcasecmp(name, "capslock") == 0) { send_host_keyboard_report(mod, 0x39); return true; }

    // Symbols & Punctuation
    if (strcasecmp(name, "minus") == 0) { send_host_keyboard_report(mod, 0x2d); return true; }
    if (strcasecmp(name, "equal") == 0) { send_host_keyboard_report(mod, 0x2e); return true; }
    if (strcasecmp(name, "leftbracket") == 0) { send_host_keyboard_report(mod, 0x2f); return true; }
    if (strcasecmp(name, "rightbracket") == 0) { send_host_keyboard_report(mod, 0x30); return true; }
    if (strcasecmp(name, "backslash") == 0) { send_host_keyboard_report(mod, 0x31); return true; }
    if (strcasecmp(name, "semicolon") == 0) { send_host_keyboard_report(mod, 0x33); return true; }
    if (strcasecmp(name, "apostrophe") == 0) { send_host_keyboard_report(mod, 0x34); return true; }
    if (strcasecmp(name, "grave") == 0) { send_host_keyboard_report(mod, 0x35); return true; }
    if (strcasecmp(name, "comma") == 0) { send_host_keyboard_report(mod, 0x36); return true; }
    if (strcasecmp(name, "period") == 0) { send_host_keyboard_report(mod, 0x37); return true; }
    if (strcasecmp(name, "slash") == 0) { send_host_keyboard_report(mod, 0x38); return true; }

    // Function keys F1-F12
    if ((name[0] == 'f' || name[0] == 'F') && strlen(name) <= 3) {
        int fnum = atoi(name + 1);
        if (fnum >= 1 && fnum <= 12) {
            send_host_keyboard_report(mod, (uint8_t)(0x3a + (fnum - 1)));
            return true;
        }
    }

    // Single character
    if (strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'z') { send_host_keyboard_report(mod, 0x04 + (name[0] - 'a')); return true; }
    if (strlen(name) == 1 && name[0] >= 'A' && name[0] <= 'Z') { send_host_keyboard_report(mod | 0x02, 0x04 + (name[0] - 'A')); return true; }

    unsigned int hex = 0;
    if (sscanf(name, "0x%x", &hex) == 1 || sscanf(name, "%x", &hex) == 1) {
        send_host_consumer_report((uint16_t)hex);
        return true;
    }
    return false;
}

bool send_host_named_key_ex(const char *name, int hold_mode) {
    g_host_send_hold_mode = hold_mode;
    bool ret = send_host_named_key(name);
    g_host_send_hold_mode = 0;
    return ret;
}

void send_host_text(const char *text) {
    if (!text || get_target_host_hid_cid() == 0) return;
    for (size_t i = 0; text[i]; i++) {
        uint8_t mod = 0, keycode = 0;
        unsigned char c = (unsigned char)text[i];
        if (c >= 'a' && c <= 'z') { keycode = 0x04 + (c - 'a'); }
        else if (c >= 'A' && c <= 'Z') { mod = 0x02; keycode = 0x04 + (c - 'A'); }
        else if (c >= '1' && c <= '9') { keycode = 0x1E + (c - '1'); }
        else if (c == '0') { keycode = 0x27; }
        else if (c == ' ') { keycode = 0x2c; }
        else if (c == '\n' || c == '\r') { keycode = 0x28; }
        else if (c == '\t') { keycode = 0x2b; }
        else if (c == '-') { keycode = 0x2d; }
        else if (c == '_') { mod = 0x02; keycode = 0x2d; }
        else if (c == '=') { keycode = 0x2e; }
        else if (c == '+') { mod = 0x02; keycode = 0x2e; }
        else if (c == '.') { keycode = 0x37; }
        else if (c == '/') { keycode = 0x38; }
        else if (c == ':') { mod = 0x02; keycode = 0x33; }
        else if (c == ';') { keycode = 0x33; }
        else if (c == ',') { keycode = 0x36; }
        else if (c == '<') { mod = 0x02; keycode = 0x36; }
        else if (c == '>') { mod = 0x02; keycode = 0x37; }
        else if (c == '?') { mod = 0x02; keycode = 0x38; }
        else if (c == '!') { mod = 0x02; keycode = 0x1e; }
        else if (c == '@') { mod = 0x02; keycode = 0x1f; }
        else if (c == '#') { mod = 0x02; keycode = 0x20; }
        else if (c == '$') { mod = 0x02; keycode = 0x21; }
        else if (c == '%') { mod = 0x02; keycode = 0x22; }
        if (keycode) {
            send_host_keyboard_report(mod, keycode);
            usleep(25000);
        }
    }
}
