#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "bt_state.h"
#include "bt_pairing.h"
#include "bt_hid.h"
#include "bt_cmd.h"
#include "bt_remote.h"

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);
    switch (event) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                log_msg("BTstack HCI state: WORKING");

                // Classic GAP configuration
                gap_set_local_name(g_host_name);
                gap_set_class_of_device(0x0025C0); // Peripheral: Keyboard + Pointing Device
                gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
                gap_set_allow_role_switch(true);
                gap_ssp_set_enable(1);
                gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
                gap_set_security_mode(GAP_SECURITY_MODE_2);
                gap_connectable_control(1);
                gap_discoverable_control(0); // Off until requested via WebUI or command
                hci_disable_l2cap_timeout_check();

                // Fast scan (30ms/30ms), 7.5-15ms conn interval, latency 0, 2.0s supervision timeout
                gap_set_connection_parameters(48, 48, 6, 12, 0, 200, 0, 0);
                init_known_hosts();
                update_status("initializing");
                hog_start_connect();

                char cur_act[64] = "-1";
                read_current_activity(cur_act, sizeof(cur_act));
                if (cur_act[0] && strcmp(cur_act, "-1") != 0 && strcasecmp(cur_act, "poweroff") != 0) {
                    bd_addr_t tgt;
                    if (get_pinned_host_target(tgt)) {
                        char bmsg[128];
                        snprintf(bmsg, sizeof(bmsg), "Boot reconnect: reconnecting to host %s (activity=%s)",
                                 bd_addr_to_str(tgt), cur_act);
                        log_msg(bmsg);
                        start_host_reconnect(tgt, MAX_HOST_RECONNECT_ATTEMPTS);
                    } else {
                        log_msg("Boot reconnect: resuming connect for active activity");
                        try_connect_host();
                    }
                } else {
                    log_msg("Boot: Hub idle/poweroff, skipping Bluetooth host connect");
                }
            }
            break;

        case GAP_EVENT_ADVERTISING_REPORT: {
            if (app_state != APP_SCANNING) break;
            bd_addr_t adv_addr;
            gap_event_advertising_report_get_address(packet, adv_addr);
            uint8_t adv_addr_type = gap_event_advertising_report_get_address_type(packet);
            const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
            uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);
            int8_t rssi = gap_event_advertising_report_get_rssi(packet);

            char dev_name[64] = "";
            uint8_t flags = 0;
            uint16_t appearance = 0;
            bool has_hid = ad_data_contains_uuid16(ad_len, ad_data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);

            // Extract advertising items
            ad_context_t context;
            for (ad_iterator_init(&context, ad_len, ad_data); ad_iterator_has_more(&context); ad_iterator_next(&context)) {
                uint8_t data_type = ad_iterator_get_data_type(&context);
                uint8_t data_len  = ad_iterator_get_data_len(&context);
                const uint8_t *data = ad_iterator_get_data(&context);
                if (data_type == 0x01 && data_len >= 1) {
                    flags = data[0];
                } else if (data_type == 0x08 || data_type == 0x09) {
                    int copy_len = data_len < (uint8_t)(sizeof(dev_name) - 1) ? data_len : (uint8_t)(sizeof(dev_name) - 1);
                    memcpy(dev_name, data, copy_len);
                    dev_name[copy_len] = 0;
                } else if (data_type == 0x19 && data_len >= 2) {
                    appearance = data[0] | (data[1] << 8);
                }
            }

            bool is_hid = has_hid || (appearance >= 0x03C0 && appearance <= 0x03C4);
            if (!is_hid && dev_name[0]) {
                if (strstr(dev_name, "Remote") || strstr(dev_name, "B25") ||
                    strstr(dev_name, "Voice") || strstr(dev_name, "Google") ||
                    strstr(dev_name, "Homatics") || strstr(dev_name, "BleRemote") ||
                    strstr(dev_name, "Keyboard") || strstr(dev_name, "Gamepad") ||
                    strcasestr(dev_name, "harmony") || strcasestr(dev_name, "smart") ||
                    strcasestr(dev_name, "logitech")) {
                    is_hid = true;
                }
            }
            bool in_pairing_mode = ((flags & 0x03) != 0);

            if (g_user_scan_active) {
                // In active user scan: discover all nearby BLE devices/remotes
                int idx = -1;
                for (int i = 0; i < g_discovered_count; i++) {
                    if (memcmp(g_discovered[i].addr, adv_addr, 6) == 0) {
                        idx = i;
                        break;
                    }
                }
                if (idx >= 0) {
                    if (dev_name[0]) strncpy(g_discovered[idx].name, dev_name, sizeof(g_discovered[idx].name) - 1);
                    g_discovered[idx].rssi = rssi;
                } else if (g_discovered_count < MAX_DISCOVERED) {
                    idx = g_discovered_count++;
                    memcpy(g_discovered[idx].addr, adv_addr, 6);
                    g_discovered[idx].addr_type = adv_addr_type;
                    if (dev_name[0]) {
                        strncpy(g_discovered[idx].name, dev_name, sizeof(g_discovered[idx].name) - 1);
                    } else if (is_hid) {
                        strcpy(g_discovered[idx].name, "Bluetooth Remote (HID)");
                    } else {
                        snprintf(g_discovered[idx].name, sizeof(g_discovered[idx].name),
                                 "BLE Device (%s)", bd_addr_to_str(adv_addr));
                    }
                    g_discovered[idx].rssi = rssi;
                    char sbuf[128];
                    snprintf(sbuf, sizeof(sbuf), "Discovered BLE device: %s ('%s') rssi=%d",
                             bd_addr_to_str(adv_addr), g_discovered[idx].name, rssi);
                    log_msg(sbuf);
                    write_scan_results(true);
                }
                break;
            }

            bool match = false;
            bd_addr_t target_mac;
            if (get_pinned_target(target_mac)) {
                if (memcmp(adv_addr, target_mac, 6) == 0) {
                    match = true;
                }
            } else if (is_hid && (in_pairing_mode || dev_name[0])) {
                match = true;
            }

            if (match) {
                memcpy(remote_device.addr, adv_addr, 6);
                remote_device.addr_type = adv_addr_type;
                char log_buf[160];
                snprintf(log_buf, sizeof(log_buf), "Connecting to BLE remote: %s ('%s')",
                         bd_addr_to_str(remote_device.addr), dev_name);
                log_msg(log_buf);
                hog_connect();
            }
            break;
        }

        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
                btstack_run_loop_remove_timer(&reconnect_timer);
                connection_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
                char msg[64];
                snprintf(msg, sizeof(msg), "LE Connection complete (handle=0x%04x)", connection_handle);
                log_msg(msg);
                // Listen for GATT notifications immediately
                gatt_client_listen_for_characteristic_value_updates(&gatt_notification_listener,
                                                                   handle_gatt_notification_event,
                                                                   connection_handle, NULL);
                app_state = APP_ENCRYPTING;
                update_status("connecting");
                gap_request_connection_parameter_update(connection_handle, 6, 12, 0, 200);
                sm_request_pairing(connection_handle);
            }
            break;

        case HCI_EVENT_CONNECTION_REQUEST: {
            bd_addr_t req_addr;
            hci_event_connection_request_get_bd_addr(packet, req_addr);
            uint8_t link_type = hci_event_connection_request_get_link_type(packet);
            char msg[96];
            snprintf(msg, sizeof(msg), "Classic BT: Connection Request from %s (link_type=%u)", bd_addr_to_str(req_addr), link_type);
            log_msg(msg);
            break;
        }

        case HCI_EVENT_CONNECTION_COMPLETE: {
            uint8_t status = hci_event_connection_complete_get_status(packet);
            hci_con_handle_t handle = hci_event_connection_complete_get_connection_handle(packet);
            bd_addr_t conn_addr;
            hci_event_connection_complete_get_bd_addr(packet, conn_addr);
            char msg[128];
            snprintf(msg, sizeof(msg), "Classic BT: Connection Complete: %s handle=0x%04x status=0x%02x", bd_addr_to_str(conn_addr), handle, status);
            log_msg(msg);
            if (status == 0) {
                host_conn_t *h = register_host_target(conn_addr, NULL);
                if (h) {
                    h->acl_handle = handle;
                }
            } else {
                for (int i = 0; i < MAX_HOST_CONNS; i++) {
                    if (memcmp(g_hosts[i].addr, conn_addr, 6) == 0) {
                        g_hosts[i].connected = false;
                        g_hosts[i].hid_cid = 0;
                        g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
                        break;
                    }
                }
                g_host_connected = is_any_host_connected();
                g_host_hid_cid = get_target_host_hid_cid();
                update_status(app_state == APP_READY ? "ready" : "standby");
                update_bthid_status();
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            hci_con_handle_t disconn_handle = hci_event_disconnection_complete_get_connection_handle(packet);
            uint8_t disconn_reason = hci_event_disconnection_complete_get_reason(packet);
            if (disconn_handle == connection_handle && connection_handle != HCI_CON_HANDLE_INVALID) {
                log_msg("LE Remote Disconnection complete");
                bt_remote_on_disconnect();
                gatt_client_stop_listening_for_characteristic_value_updates(&gatt_notification_listener);
                connection_handle = HCI_CON_HANDLE_INVALID;
                app_state = APP_IDLE;
                update_status("disconnected");
                schedule_reconnect(10); // Instant scan resume
            } else {
                g_host_connecting = false;
                char msg[128];
                snprintf(msg, sizeof(msg), "Classic ACL disconnect: handle=0x%04x reason=0x%02x", disconn_handle, disconn_reason);
                log_msg(msg);
                bool host_cleared = false;
                for (int i = 0; i < MAX_HOST_CONNS; i++) {
                    if (g_hosts[i].acl_handle == disconn_handle || (disconn_handle != HCI_CON_HANDLE_INVALID && g_hosts[i].connected)) {
                        g_hosts[i].connected = false;
                        g_hosts[i].hid_cid = 0;
                        g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
                        host_cleared = true;
                    }
                }
                if (host_cleared) {
                    g_host_connected = is_any_host_connected();
                    g_host_hid_cid = get_target_host_hid_cid();
                    update_status(app_state == APP_READY ? "ready" : "standby");
                    update_bthid_status();
                }
            }
            break;
        }

        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t pin_addr;
            hci_event_pin_code_request_get_bd_addr(packet, pin_addr);
            if (g_host_pairing) {
                log_msg("Classic BT: PIN code requested -> sending default '0000'");
                gap_pin_code_response(pin_addr, "0000");
            } else {
                log_msg("Classic BT: PIN code requested but pairing mode is disabled -> rejecting");
                gap_pin_code_negative(pin_addr);
            }
            break;
        }

        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t conf_addr;
            hci_event_user_confirmation_request_get_bd_addr(packet, conf_addr);
            if (g_host_pairing) {
                log_msg("Classic BT: SSP User Confirmation Request -> auto-accepting");
                gap_ssp_confirmation_response(conf_addr);
            } else {
                log_msg("Classic BT: SSP User Confirmation Request but pairing mode is disabled -> rejecting");
                gap_ssp_confirmation_negative(conf_addr);
            }
            break;
        }

        case HCI_EVENT_USER_PASSKEY_REQUEST: {
            bd_addr_t pass_addr;
            hci_event_user_passkey_request_get_bd_addr(packet, pass_addr);
            if (g_host_pairing) {
                log_msg("Classic BT: SSP Passkey Request -> auto-replying 0");
                gap_ssp_passkey_response(pass_addr, 0);
            } else {
                log_msg("Classic BT: SSP Passkey Request but pairing mode is disabled -> rejecting");
                gap_ssp_passkey_negative(pass_addr);
            }
            break;
        }

        case HCI_EVENT_LINK_KEY_NOTIFICATION: {
            bd_addr_t lk_addr;
            reverse_bd_addr(&packet[2], lk_addr);
            memcpy(g_host_addr, lk_addr, sizeof(bd_addr_t));
            if (tlv_impl) {
                tlv_impl->store_tag(&tlv_context, TLV_TAG_HIDR, (const uint8_t *)lk_addr, sizeof(bd_addr_t));
            }
            char msg[96];
            snprintf(msg, sizeof(msg), "Classic BT: Link Key saved for %s", bd_addr_to_str(lk_addr));
            log_msg(msg);
            break;
        }

        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE: {
            uint8_t status = hci_event_simple_pairing_complete_get_status(packet);
            bd_addr_t pair_addr;
            hci_event_simple_pairing_complete_get_bd_addr(packet, pair_addr);
            char msg[96];
            snprintf(msg, sizeof(msg), "Classic BT: Simple Pairing Complete for %s (status=0x%02x)", bd_addr_to_str(pair_addr), status);
            log_msg(msg);
            break;
        }

        case GAP_EVENT_DEDICATED_BONDING_COMPLETED: {
            uint8_t status = packet[2];
            bd_addr_t b_addr;
            reverse_bd_addr(&packet[3], b_addr);
            char msg[96];
            snprintf(msg, sizeof(msg), "Classic BT: Dedicated Bonding Complete for %s (status=0x%02x)", bd_addr_to_str(b_addr), status);
            log_msg(msg);
            break;
        }

        case HCI_EVENT_HID_META: {
            uint8_t subevent = hci_event_hid_meta_get_subevent_code(packet);
            switch (subevent) {
                case HID_SUBEVENT_CONNECTION_OPENED: {
                    uint8_t status = hid_subevent_connection_opened_get_status(packet);
                    uint16_t cid = hid_subevent_connection_opened_get_hid_cid(packet);
                    bd_addr_t conn_addr;
                    hid_subevent_connection_opened_get_bd_addr(packet, conn_addr);

                    if (status != ERROR_CODE_SUCCESS) {
                        g_host_connecting = false;
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Classic HID Host connection to %s failed (0x%02x)",
                                 bd_addr_to_str(conn_addr), status);
                        log_msg(msg);
                        for (int i = 0; i < MAX_HOST_CONNS; i++) {
                            if (memcmp(g_hosts[i].addr, conn_addr, 6) == 0 || g_hosts[i].hid_cid == cid) {
                                g_hosts[i].connected = false;
                                g_hosts[i].hid_cid = 0;
                                if (g_hosts[i].acl_handle != HCI_CON_HANDLE_INVALID) {
                                    gap_disconnect(g_hosts[i].acl_handle);
                                    g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
                                }
                            }
                        }
                        g_host_connected = is_any_host_connected();
                        g_host_hid_cid = get_target_host_hid_cid();
                        update_status(app_state == APP_READY ? "ready" : "standby");
                        update_bthid_status();
                        char cur_act[64] = "-1";
                        read_current_activity(cur_act, sizeof(cur_act));
                        if (cur_act[0] && strcmp(cur_act, "-1") != 0 && strcasecmp(cur_act, "poweroff") != 0) {
                            if (!g_host_reconnect_active && !is_null_bd_addr(conn_addr)) {
                                start_host_reconnect(conn_addr, MAX_HOST_RECONNECT_ATTEMPTS);
                            }
                        } else {
                            log_msg("Host disconnected while Activity is PowerOff, suppressing reconnect");
                        }
                        break;
                    }

                    g_host_connecting = false;
                    stop_host_reconnect();
                    g_host_connected = true;
                    g_host_pairing = false;
                    gap_discoverable_control(0);
                    g_host_hid_cid = cid;
                    memcpy(g_host_addr, conn_addr, 6);

                    host_conn_t *h = register_host_target(conn_addr, NULL);
                    if (h) {
                        h->connected = true;
                        h->hid_cid = cid;
                    }

                    char msg[128];
                    snprintf(msg, sizeof(msg), "Classic HID Host CONNECTED: %s (cid=0x%04x)",
                             bd_addr_to_str(conn_addr), cid);
                    log_msg(msg);

                    FILE *pf = fopen("/tmp/bt_pending_pair_device", "r");
                    if (pf) {
                        char link_dev_id[64] = {0};
                        if (fgets(link_dev_id, sizeof(link_dev_id), pf)) {
                            char *nl = strpbrk(link_dev_id, "\r\n");
                            if (nl) *nl = 0;
                            if (link_dev_id[0]) {
                                link_bt_device_to_addr(link_dev_id, bd_addr_to_str(conn_addr));
                            }
                        }
                        fclose(pf);
                        unlink("/tmp/bt_pending_pair_device");
                    }

                    if (tlv_impl) {
                        tlv_impl->store_tag(&tlv_context, TLV_TAG_HIDR, (const uint8_t *)conn_addr, sizeof(bd_addr_t));
                    }
                    FILE *hf = fopen("/data/harmony/bthid_host_target", "w");
                    if (hf) {
                        fprintf(hf, "bdaddr=%s\n", bd_addr_to_str(conn_addr));
                        fclose(hf);
                    }
                    FILE *btf = fopen("/data/harmony/bthid_target", "w");
                    if (btf) {
                        fprintf(btf, "type=btkeyboard\nbdaddr=%s\n", bd_addr_to_str(conn_addr));
                        fclose(btf);
                    }
                    update_status(app_state == APP_READY ? "ready" : "standby");
                    update_bthid_status();

                    if (g_pending_wake_btn[0] && (time(NULL) - g_pending_wake_time) < 10) {
                        char pbtn[32];
                        strncpy(pbtn, g_pending_wake_btn, sizeof(pbtn) - 1);
                        pbtn[sizeof(pbtn) - 1] = 0;
                        g_pending_wake_btn[0] = 0;
                        uint8_t mod = 0, kc = 0;
                        if (sscanf(pbtn, "KEY %hhx %hhx", &mod, &kc) == 2) {
                            send_host_keyboard_report(mod, kc);
                            log_msg("Dispatched buffered wake keyboard report to host");
                        } else if (strncmp(pbtn, "CONSUMER ", 9) == 0) {
                            unsigned int usage = 0;
                            if (sscanf(pbtn + 9, "%x", &usage) == 1) {
                                send_host_consumer_report((uint16_t)usage);
                                log_msg("Dispatched buffered wake consumer report to host");
                            }
                        }
                    } else {
                        g_pending_wake_btn[0] = 0;
                    }
                    break;
                }
                case HID_SUBEVENT_CONNECTION_CLOSED: {
                    g_host_connecting = false;
                    uint16_t cid = hid_subevent_connection_closed_get_hid_cid(packet);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Classic HID Host DISCONNECTED (cid=0x%04x)", cid);
                    log_msg(msg);
                    bool matched = false;
                    for (int i = 0; i < MAX_HOST_CONNS; i++) {
                        if (cid != 0 && g_hosts[i].hid_cid == cid) {
                            g_hosts[i].connected = false;
                            g_hosts[i].hid_cid = 0;
                            g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
                            matched = true;
                            break;
                        }
                    }
                    if (!matched && cid == 0) {
                        for (int i = 0; i < MAX_HOST_CONNS; i++) {
                            g_hosts[i].connected = false;
                            g_hosts[i].hid_cid = 0;
                            g_hosts[i].acl_handle = HCI_CON_HANDLE_INVALID;
                        }
                    }
                    g_host_connected = is_any_host_connected();
                    g_host_hid_cid = get_target_host_hid_cid();
                    update_status(app_state == APP_READY ? "ready" : "standby");
                    update_bthid_status();
                    break;
                }
                default:
                    break;
            }
            break;
        }

        default:
            break;
    }
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t addr;
    uint8_t addr_type;

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            log_msg("SM: Confirming Just Works pairing request");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
            break;
        case SM_EVENT_PAIRING_COMPLETE: {
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                log_msg("SMP pairing & bonding complete! Connecting HIDS...");
                app_state = APP_W4_HIDS;
                hids_host_connect(connection_handle, handle_gatt_client_event, protocol_mode, &hids_cid);
            } else {
                uint8_t reason = sm_event_pairing_complete_get_reason(packet);
                char msg[64];
                snprintf(msg, sizeof(msg), "SMP pairing failed (status=0x%02x, reason=0x%02x)", status, reason);
                log_msg(msg);
                gap_disconnect(connection_handle);
                schedule_reconnect(2000);
            }
            break;
        }
        case SM_EVENT_REENCRYPTION_STARTED: {
            sm_event_reencryption_complete_get_address(packet, addr);
            char msg[128];
            snprintf(msg, sizeof(msg), "Bonding info exists for %s -> re-encrypting link", bd_addr_to_str(addr));
            log_msg(msg);
            break;
        }
        case SM_EVENT_REENCRYPTION_COMPLETE: {
            uint8_t status = sm_event_reencryption_complete_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                log_msg("Link re-encryption complete -> Remote READY");
                app_state = APP_READY;
                update_status("ready");
            } else if (status == ERROR_CODE_PIN_OR_KEY_MISSING) {
                log_msg("Re-encryption failed: key missing on remote. Deleting local bond and re-pairing...");
                sm_event_reencryption_complete_get_address(packet, addr);
                addr_type = sm_event_reencryption_started_get_addr_type(packet);
                gap_delete_bonding(addr_type, addr);
                sm_request_pairing(sm_event_reencryption_complete_get_handle(packet));
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "Re-encryption failed (status=0x%02x)", status);
                log_msg(msg);
                gap_disconnect(connection_handle);
                schedule_reconnect(2000);
            }
            break;
        }
        default:
            break;
    }
}

static void sig_handler(int sig) {
    UNUSED(sig);
    g_running = 0;
    btstack_run_loop_trigger_exit();
}

int main(int argc, const char *argv[]) {
    UNUSED(argc);
    UNUSED(argv);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    FILE *pf = fopen(PID_FILE, "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }

    log_msg("=== Starting Hub BTstack Daemon ===");
    update_status("starting");
    update_bthid_status();

    // 1. Initialize BTstack memory and run loop
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());

    // 2. Initialize Linux HCI Transport & Stack FIRST
    hci_transport_config_linux_t transport_config = {
        .type = HCI_TRANSPORT_CONFIG_LINUX,
        .device_id = 0
    };
    hci_init(hci_transport_linux_instance(), &transport_config);

    // 3. Set up TLV database for persistent bonding (LE + Classic Link Keys) AFTER hci_init
    tlv_impl = btstack_tlv_posix_init_instance(&tlv_context, TLV_DB_PATH);
    btstack_tlv_set_instance(tlv_impl, &tlv_context);
    le_device_db_tlv_configure(tlv_impl, &tlv_context);
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_context));

    if (tlv_impl) {
        int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_HIDR, (uint8_t *)g_host_addr, sizeof(bd_addr_t));
        if (len == (int)sizeof(bd_addr_t)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Loaded paired Host BD_ADDR from TLV: %s", bd_addr_to_str(g_host_addr));
            log_msg(msg);
        }
    }

    // Enable packet dump only if explicit packet dump flag exists
    if (access("/data/harmony/bt_packet_dump.conf", F_OK) == 0) {
        hci_dump_init(hci_dump_posix_stdout_get_instance());
    }

    // 4. Initialize L2CAP and GATT Client
    l2cap_init();
    gatt_client_init();

    // 6. Security Manager (SMP Legacy Just Works / Bonding for BLE)
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
    sm_set_encryption_key_size_range(7, 16);

    // 7. SDP Server & Records
    sdp_init();

    // 7a. HID Device SDP record
    memset(hid_service_buffer, 0, sizeof(hid_service_buffer));
    hid_sdp_record_t hid_params = {
        .hid_device_subclass = 0x25C0,
        .hid_country_code = 33, // US
        .hid_virtual_cable = 0,
        .hid_remote_wake = 1,
        .hid_reconnect_initiate = 1,
        .hid_normally_connectable = 1,
        .hid_boot_device = 1,
        .hid_ssr_host_max_latency = 1600,
        .hid_ssr_host_min_timeout = 3200,
        .hid_supervision_timeout = 3200,
        .hid_descriptor = hid_descriptor_composite,
        .hid_descriptor_size = hid_descriptor_composite_len,
        .device_name = g_host_name
    };
    hid_create_sdp_record(hid_service_buffer, sdp_create_service_record_handle(), &hid_params);
    btstack_assert(de_get_len(hid_service_buffer) <= sizeof(hid_service_buffer));
    sdp_register_service(hid_service_buffer);

    // 7b. Device ID SDP record (Logitech 0x046D)
    memset(device_id_sdp_service_buffer, 0, sizeof(device_id_sdp_service_buffer));
    device_id_create_sdp_record(device_id_sdp_service_buffer, sdp_create_service_record_handle(),
                               DEVICE_ID_VENDOR_ID_SOURCE_USB, 0x046D, 0xC52B, 0x0100);
    btstack_assert(de_get_len(device_id_sdp_service_buffer) <= sizeof(device_id_sdp_service_buffer));
    sdp_register_service(device_id_sdp_service_buffer);

    char sdp_msg[96];
    snprintf(sdp_msg, sizeof(sdp_msg), "SDP registered: HID %u bytes, DeviceID %u bytes",
             de_get_len(hid_service_buffer), de_get_len(device_id_sdp_service_buffer));
    log_msg(sdp_msg);

    // 8. Classic HID Device service
    hid_device_init(1, hid_descriptor_composite_len, hid_descriptor_composite);
    hid_device_register_packet_handler(&packet_handler);

    // 9. BLE HID Service Host (for physical remote)
    hids_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));

    // 10. Packet callbacks
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sm_event_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    // 11. Command & FIFO polling timer (100ms) and IPC socket
    poll_bthid_fifo();
    update_status("initializing");
    update_bthid_status();
    btstack_run_loop_set_timer(&cmd_poll_timer, 100);
    btstack_run_loop_set_timer_handler(&cmd_poll_timer, &cmd_poll_timeout);
    btstack_run_loop_add_timer(&cmd_poll_timer);

    init_cmd_socket();
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    // 12. Power on
    app_state = APP_W4_WORKING;
    int pwr_err = hci_power_control(HCI_POWER_ON);
    if (pwr_err != 0) {
        log_msg("FATAL: hci_power_control failed, exiting");
        fprintf(stderr, "[hub_btstack] FATAL: hci_power_control failed (%d), exiting\n", pwr_err);
        cleanup_cmd_socket();
        if (g_bthid_fifo_fd >= 0) {
            close(g_bthid_fifo_fd);
            g_bthid_fifo_fd = -1;
        }
        unlink(BTHID_STATUS_FILE);
        unlink(PID_FILE);
        return 1;
    }

    // 13. Run loop
    btstack_run_loop_execute();

    cleanup_cmd_socket();
    if (g_bthid_fifo_fd >= 0) {
        close(g_bthid_fifo_fd);
        g_bthid_fifo_fd = -1;
    }
    unlink(BTHID_STATUS_FILE);
    hci_power_control(HCI_POWER_OFF);
    unlink(PID_FILE);
    return 0;
}
