# hub_rf protocol and the remote's RF decoders (sourced by tests/run_mock_suite.sh)

# All hub_rf sources as build/compile_all.sh lists them, main renamed (the test has its own)
RF50_SRCS=$(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g")
# A mock root of its own, so these tests do not share /tmp/harmony_mock with the rest of the suite
RF50_MOCK=$(mktemp -d /tmp/hng_rf50.XXXXXX)

# ACKed sends from several threads: each gets its own ACK, from the slot it sent to
build_test -Wall -O2 -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_ack_match" "$SCRIPT_DIR/test_rf_ack_match.c" \
    $RF50_SRCS "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_ack_match"

# Frame fields: DEVICE_CMD_REQUEST layouts, escaped request bodies, slots above 2, slot name, update source
build_test -Wall -O2 -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_frame_fields" "$SCRIPT_DIR/test_rf_frame_fields.c" \
    -Wl,--wrap=enqueue_remote_device_cmd -Wl,--wrap=handle_companion_packet -Wl,--wrap=http_post_local \
    $RF50_SRCS "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && \
    HARMONY_MOCK_DIR="$RF50_MOCK/frame_fields" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_frame_fields"

# Threads and radio I/O: no connect on the receive thread, read errors stop hub_rf, icon push off the API thread
build_test -Wall -O2 -include sys/stat.h -include time.h -Dmain=hub_rf_main -DRF_API_PORT=9181 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_threads_io" "$SCRIPT_DIR/test_rf_threads_io.c" \
    -Wl,--wrap=connect -Wl,--wrap=write $RF50_SRCS "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && \
    HARMONY_MOCK_DIR="$RF50_MOCK/threads_io" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_threads_io"

# RF API: exact routes, the slot from the query or the JSON body, 400 for any other slot
build_test -Wall -O2 -include sys/stat.h -include time.h -Dmain=hub_rf_main -DRF_API_PORT=9183 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_api_params" "$SCRIPT_DIR/test_rf_api_params.c" \
    $RF50_SRCS "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && \
    HARMONY_MOCK_DIR="$RF50_MOCK/api_params" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_api_params"

# Unpair from either side clears the slot; the remote's settings backup keeps noScreen and powerDebug
build_test -Wall -O2 -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_unpair_backup" "$SCRIPT_DIR/test_rf_unpair_backup.c" \
    $RF50_SRCS "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && \
    HARMONY_MOCK_DIR="$RF50_MOCK/unpair_backup" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_unpair_backup"

# Sender retries: OTA_END sent again, a complete checkpoint gets only OTA_END, RF_CAP_DEVICE_CMDS
mkdir -p "$RF50_MOCK/sender_tmp"
build_test -Wall -O2 -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_sender_retries" "$SCRIPT_DIR/test_rf_sender_retries.c" \
    -Wl,--wrap=fopen -Wl,--wrap=access -Wl,--wrap=unlink $RF50_SRCS "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && \
    RF_TEST_TMP="$RF50_MOCK/sender_tmp" HARMONY_MOCK_DIR="$RF50_MOCK/sender" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_sender_retries"

# Remote: config version compare, chunks sent again, OTA receiver input checks, screen timeout, capability bit
mkdir -p "$RF50_MOCK/remote_tmp"
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -I"$REPO_ROOT/payload/source/remote" -o "$BUILD_DIR/test_remote_rf_decoders" "$SCRIPT_DIR/test_remote_rf_decoders.c" \
    -Wl,--wrap=fopen,--wrap=open,--wrap=unlink,--wrap=stat,--wrap=statvfs,--wrap=system \
    "$REPO_ROOT"/payload/source/remote/{remote_config,remote_fb,remote_i18n,remote_input,remote_power,remote_battery,remote_flightlog,remote_present,remote_rf,remote_rf_ota,remote_rf_sync,remote_state,remote_ui,remote_ui_activities,remote_ui_devices,remote_ui_pairing,remote_ui_menu,remote_ui_help,remote_ui_activity,remote_link,remote_ui_settings,remote_ui_settings_actions,remote_theme,remote_ui_scroll,remote_transition,remote_nav}.c \
    "$REPO_ROOT"/payload/source/{remote_stock_icons,cJSON,sha256,timezone_db}.c -lpthread -lm && \
    RF_TEST_TMP="$RF50_MOCK/remote_tmp" HARMONY_MOCK_DIR="$RF50_MOCK/remote" HARMONY_MOCK_ROLE=remote LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_remote_rf_decoders"

rm -rf "$RF50_MOCK"
