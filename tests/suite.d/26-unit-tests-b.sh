# Second half of the C unit tests, IR receiver, reconnect queue, safe_reboot.sh, co-simulation and Bluetooth power links
# (moved from tests/run_mock_suite.sh; the suite sources this file)

build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_companion_timeout" "$SCRIPT_DIR/test_rf_companion_timeout.c" "$REPO_ROOT/payload/source/rf_keymap.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_companion_timeout"
# A companion key press is never seen half stored (strncpy wrapped to widen the window)
build_test -Wall -O2 -fno-builtin-strncpy -Wl,--wrap=strncpy -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_companion_race" "$SCRIPT_DIR/test_rf_companion_race.c" "$REPO_ROOT/payload/source/rf_keymap.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_companion_race"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_capture" "$SCRIPT_DIR/test_ir_capture.c" "$REPO_ROOT/payload/source/ir_encoder.c" "$REPO_ROOT/payload/source/ir_i2s.c" -lpthread && "$BUILD_DIR/test_ir_capture" "$SCRIPT_DIR/fixtures/ir/samsung36_volume_up.i2s"
# Remote: an activity change reported right after a wake that follows a suspend
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_wake_catchup" "$SCRIPT_DIR/test_remote_wake_catchup.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_wake_catchup"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_action_lifecycle" "$SCRIPT_DIR/test_action_lifecycle.c" "$REPO_ROOT/payload/source/hw_action.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_action_lifecycle"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_orch_current_activity" "$SCRIPT_DIR/test_orch_current_activity.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_orch_current_activity"
# Hub init.sh log cap in a host sandbox (every log in /tmp is capped)
python3 "$SCRIPT_DIR/test_init_log_cap.py"
python3 "$SCRIPT_DIR/test_init_bt_reattach.py"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_bt_state_migrate" "$SCRIPT_DIR/test_bt_state_migrate.c" "$REPO_ROOT/payload/source/bt_migrate.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_bt_state_migrate"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source/hub_btstack" -o "$BUILD_DIR/test_bt_hid_hex" "$SCRIPT_DIR/test_bt_hid_hex.c" && "$BUILD_DIR/test_bt_hid_hex"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_mfg" "$SCRIPT_DIR/test_mfg.c" "$REPO_ROOT/payload/source/mfg_id.c" && "$BUILD_DIR/test_mfg"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ws_read_frame" "$SCRIPT_DIR/test_ws_read_frame.c" "$REPO_ROOT/payload/source/ws_server.c" -lpthread && "$BUILD_DIR/test_ws_read_frame"
# Remote: windowed OTA data frames that arrive outside OTA mode (real RF receive thread)
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_stray_ota_data" "$SCRIPT_DIR/test_remote_stray_ota_data.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_stray_ota_data"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_mdns_packet" "$SCRIPT_DIR/test_mdns_packet.c" "$REPO_ROOT/payload/source/hub_mdns.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_mdns_packet"
# hub_rf config push targets (all hub_rf sources as build/compile_all.sh lists them, main renamed; /dev/rfspi is a socket pair)
build_test -Wall -O2 -fsanitize=address -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_push_targets" "$SCRIPT_DIR/test_rf_push_targets.c" \
    $(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g") "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm
ASAN_PRELOAD=$(ldd "$BUILD_DIR/test_rf_push_targets" 2>/dev/null | grep libasan | awk '{print $3}' || true)
HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_push_targets"
# hub_rf config push step list: device warmup step, leading delay step, default steps (own mock folder)
build_test -Wall -O2 -fsanitize=address -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_push_steps" "$SCRIPT_DIR/test_rf_push_steps.c" \
    $(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g") "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm
PUSH_STEPS_DIR="$(mktemp -d /tmp/hng_push_steps.XXXXXX)"
mkdir -p "$PUSH_STEPS_DIR/data/harmony/config"
HARMONY_MOCK_DIR="$PUSH_STEPS_DIR" LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_push_steps" \
    || { rm -rf "$PUSH_STEPS_DIR"; exit 1; }
rm -rf "$PUSH_STEPS_DIR"
# hub_rf switch start notice only to remotes that report RF_CAP_SWITCH_START
build_test -Wall -O2 -fsanitize=address -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_switch_notice" "$SCRIPT_DIR/test_rf_switch_notice.c" \
    $(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g") "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm
HARMONY_MOCK_DIR="/tmp/harmony_mock" HARMONY_MOCK_ACTIVITY_FILES=1 LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_switch_notice"
build_test -Wall -Wextra -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_mqtt_client_io" "$SCRIPT_DIR/test_mqtt_client_io.c" "$REPO_ROOT/payload/source/mqtt_client.c" && "$BUILD_DIR/test_mqtt_client_io"
python3 "$SCRIPT_DIR/test_switched_marker.py"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_hbus_config" "$SCRIPT_DIR/test_hbus_config.c" "$REPO_ROOT/payload/source/hbus_config.c" "$REPO_ROOT/payload/source/sha256.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_hbus_config" "$SCRIPT_DIR/fixtures/ha/sample_config"
# Configuration trees shared by readers: a save never frees a tree a reader still holds (AddressSanitizer + LeakSanitizer)
build_test -Wall -Wextra -O1 -g -fsanitize=address -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_config_snap" "$SCRIPT_DIR/test_config_snap.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_config_snap"
# Key presses read the shared device list without copying it
build_test -Wall -O1 -g -fsanitize=address -pthread -DNET_NO_TLS -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_config_keypath" "$SCRIPT_DIR/test_config_keypath.c" \
    "$REPO_ROOT"/payload/source/{config_store,resource_cache,hw_action,bt_migrate,config_sanitize,plugin_mgr,http_util,sha256,cJSON,net_action,tls_client,http_client}.c -lm
ASAN_PRELOAD=$(ldd "$BUILD_DIR/test_config_keypath" 2>/dev/null | grep libasan | awk '{print $3}' || true)
LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_config_keypath"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_hub_events" "$SCRIPT_DIR/test_hub_events.c" "$REPO_ROOT/payload/source/hub_events.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_hub_events"
# hub_rf frame handler on a DEVICE_CMD_REQUEST with no screen button (all hub_rf sources as build/compile_all.sh lists them, main renamed)
build_test -Wall -O2 -fsanitize=address -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_device_cmd_frame" "$SCRIPT_DIR/test_rf_device_cmd_frame.c" \
    $(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g") "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm
ASAN_PRELOAD=$(ldd "$BUILD_DIR/test_rf_device_cmd_frame" 2>/dev/null | grep libasan | awk '{print $3}' || true)
HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_device_cmd_frame"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_status_json" "$SCRIPT_DIR/test_rf_status_json.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_status_json"
# hub_rf: key frames lost between the remote and the Hub, counted from the key event counter (all hub_rf sources as build/compile_all.sh lists them, main renamed)
build_test -Wall -O2 -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_key_counter" "$SCRIPT_DIR/test_rf_key_counter.c" \
    $(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g") "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_key_counter"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_model" "$SCRIPT_DIR/test_rf_model.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_model"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_battery" "$SCRIPT_DIR/test_rf_battery.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_battery"
# Remote: home selection and pending confirmation across a config push that rebuilds the list
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_selection_rebuild" "$SCRIPT_DIR/test_remote_selection_rebuild.c" \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_selection_rebuild"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_rf_ota_resume" "$SCRIPT_DIR/test_rf_ota_resume.c" "$REPO_ROOT/payload/source/remote/remote_rf_ota.c" "$REPO_ROOT/payload/source/sha256.c" && "$BUILD_DIR/test_rf_ota_resume"
python3 "$SCRIPT_DIR/test_inventory_string_ids.py"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_migrate" "$SCRIPT_DIR/test_rf_migrate.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_api.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_migrate"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -DOTA_EXTRA_PUBKEY="$(cat "$SCRIPT_DIR/ota_test_pubkey.txt")" -o "$BUILD_DIR/test_rf_ota_lifecycle" "$SCRIPT_DIR/test_rf_ota_lifecycle.c" "$REPO_ROOT/payload/source/rf_state.c" "$REPO_ROOT/payload/source/rf_sync.c" "$REPO_ROOT/payload/source/rf_ota.c" "$REPO_ROOT/payload/source/ota_verify.c" "$REPO_ROOT/payload/source/tweetnacl.c" "$REPO_ROOT/payload/source/sha256.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_ota_lifecycle"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -DOTA_EXTRA_PUBKEY="$(cat "$SCRIPT_DIR/ota_test_pubkey.txt")" -o "$BUILD_DIR/test_ota_verify" "$SCRIPT_DIR/test_ota_verify.c" "$REPO_ROOT/payload/source/ota_verify.c" "$REPO_ROOT/payload/source/tweetnacl.c" "$REPO_ROOT/payload/source/sha256.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_ota_verify"

# hub_rf: frames from a CC2544 slot that holds no remote (all hub_rf sources as build/compile_all.sh lists them, main renamed)
build_test -Wall -O2 -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_foreign_slot" "$SCRIPT_DIR/test_rf_foreign_slot.c" \
    $(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g") "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_foreign_slot"
# 4b. IR receiver: decoding a real recording, and a send while a stalled receiver is closing
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_rx_decode" "$SCRIPT_DIR/test_ir_rx_decode.c" "$REPO_ROOT/payload/source/ir_i2s.c" && "$BUILD_DIR/test_ir_rx_decode" "$SCRIPT_DIR/fixtures/ir/samsung36_volume_up.i2s"
build_test -Wall -O1 -g -fsanitize=address -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_blob" "$SCRIPT_DIR/test_ir_blob.c" "$REPO_ROOT/payload/source/ir_i2s.c" && "$BUILD_DIR/test_ir_blob"
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_tx_stall" "$SCRIPT_DIR/test_ir_tx_stall.c" "$REPO_ROOT/payload/source/ir_i2s.c" && HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_ir_tx_stall"
build_test -Wall -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_capture_mode_race" "$SCRIPT_DIR/test_ir_capture_mode_race.c" "$REPO_ROOT/payload/source/ir_i2s.c" && HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_ir_capture_mode_race"
# Learning a press through the receiver thread, also while the capture mode is on
build_test -Wall -Wextra -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_learn_capture" "$SCRIPT_DIR/test_ir_learn_capture.c" "$REPO_ROOT/payload/source/ir_i2s.c" -lm && HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_ir_learn_capture"
# ... and a learn result crossing the waiter's limit (it gives up right at the deadline here): no write after it gave up
build_test -Wall -Wextra -O1 -g -fsanitize=address -pthread -DIR_LEARN_GRACE_MS=0 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_learn_capture_race" "$SCRIPT_DIR/test_ir_learn_capture.c" "$REPO_ROOT/payload/source/ir_i2s.c" -lm
ASAN_PRELOAD=$(ldd "$BUILD_DIR/test_ir_learn_capture_race" 2>/dev/null | grep libasan | awk '{print $3}' || true)
HARMONY_MOCK_DIR="/tmp/harmony_mock" ASAN_OPTIONS=detect_leaks=0 LD_PRELOAD="${ASAN_PRELOAD:+$ASAN_PRELOAD:}$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_ir_learn_capture_race" race

# 4c. hub_btstack reconnect queue and connected-hosts file
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source/hub_btstack" -o "$BUILD_DIR/test_bt_release_timers" "$SCRIPT_DIR/test_bt_release_timers.c" && "$BUILD_DIR/test_bt_release_timers"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source/hub_btstack" -o "$BUILD_DIR/test_bt_reconnect_queue" "$SCRIPT_DIR/test_bt_reconnect_queue.c" "$REPO_ROOT/payload/source/hub_btstack/bt_reconnect_queue.c" && "$BUILD_DIR/test_bt_reconnect_queue"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source/hub_btstack" -o "$BUILD_DIR/test_bt_le_pair_window" "$SCRIPT_DIR/test_bt_le_pair_window.c" "$REPO_ROOT/payload/source/hub_btstack/bt_le_pair_window.c" && "$BUILD_DIR/test_bt_le_pair_window"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source/hub_btstack" -o "$BUILD_DIR/test_bt_hid_queue" "$SCRIPT_DIR/test_bt_hid_queue.c" "$REPO_ROOT/payload/source/hub_btstack/bt_hid_queue.c" && "$BUILD_DIR/test_bt_hid_queue"
# The key pipe: hub_btstack's reader (bt_cmd.c with stubs), and hub_daemon's hand-off in one checked write
build_test -Wall -Wextra -Wno-format-truncation -Wno-stringop-truncation -O2 -I"$REPO_ROOT/payload/source/hub_btstack" -o "$BUILD_DIR/test_bt_key_pipe" "$SCRIPT_DIR/test_bt_key_pipe.c" && "$BUILD_DIR/test_bt_key_pipe"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -Wl,--wrap=open -o "$BUILD_DIR/test_bt_key_handoff" "$SCRIPT_DIR/test_bt_key_handoff.c" "$REPO_ROOT/payload/source/hw_action.c" "$REPO_ROOT/payload/source/cJSON.c" -lm && "$BUILD_DIR/test_bt_key_handoff"
# hub_rf's key queue: a quick tap keeps its press, a queued key is sent at once, the idle worker sleeps
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_key_queue" "$SCRIPT_DIR/test_rf_key_queue.c" "$REPO_ROOT/payload/source/rf_keymap.c" "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_key_queue"
# hub_daemon: a key from a remote goes out without the gaps between commands (daemon in --dry-run, mock key pipe)
python3 "$SCRIPT_DIR/test_live_key_gaps.py"
# Key trace (debug logging): the TS line from hub_daemon, and hub_btstack's line per key
python3 "$SCRIPT_DIR/test_key_trace.py"
build_test -Wall -Wextra -O2 -I"$REPO_ROOT/payload/source/hub_btstack" -o "$BUILD_DIR/test_bt_key_trace" "$SCRIPT_DIR/test_bt_key_trace.c" && "$BUILD_DIR/test_bt_key_trace"

# 4d. safe_reboot.sh in a host sandbox (supervisor loops stopped before reboot -f)
python3 "$SCRIPT_DIR/test_safe_reboot.py"
# update.sh apply in a host sandbox (a failed copy changes no installed file and no version)
python3 "$SCRIPT_DIR/test_update_apply.py"

# 5. Run Hub & Remote Full Co-Simulation
python3 "$SCRIPT_DIR/test_bt_connect_commands.py"
echo "[-] Launching Full Hub & Remote Co-Simulation..."
python3 "$SCRIPT_DIR/test_config_unreadable.py"
python3 "$SCRIPT_DIR/test_co_simulation.py"

# USB Ethernet adapters listed in the network guide reach a driver that binds them (start_usb_eth.sh in a host sandbox)
python3 "$SCRIPT_DIR/test_usb_eth_table.py"
# every module in payload/modules records the stock CRCs and has the stock struct sk_buff layout
python3 "$SCRIPT_DIR/test_usb_eth_modules_crc.py"

# 5a. Bluetooth links around activity power on and off (orchestrator, daemon in --dry-run)
python3 "$SCRIPT_DIR/test_activity_test_sequence.py"
python3 "$SCRIPT_DIR/test_bt_power_link.py"
# Legacy web UI modules import the functions they call (no build step reports a missing import)
python3 "$SCRIPT_DIR/test_ui_imports.py"
python3 "$SCRIPT_DIR/test_activity_queue.py"
python3 "$SCRIPT_DIR/test_activity_restore.py"
python3 "$SCRIPT_DIR/test_activity_switch_timing.py"
python3 "$SCRIPT_DIR/test_activity_save_match.py"
python3 "$SCRIPT_DIR/test_config_cache_race.py"
python3 "$SCRIPT_DIR/test_config_safe_writes.py"

# Hub boot scripts in a host sandbox (reboot counter, emergency recovery mode)
python3 "$SCRIPT_DIR/test_hub_boot_counter.py"
