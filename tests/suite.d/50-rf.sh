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

rm -rf "$RF50_MOCK"
