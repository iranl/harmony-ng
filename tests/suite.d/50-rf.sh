# hub_rf protocol and the remote's RF decoders (sourced by tests/run_mock_suite.sh)

# All hub_rf sources as build/compile_all.sh lists them, main renamed (the test has its own)
RF50_SRCS=$(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g")
# A mock root of its own, so these tests do not share /tmp/harmony_mock with the rest of the suite
RF50_MOCK=$(mktemp -d /tmp/hng_rf50.XXXXXX)

# ACKed sends from several threads: each gets its own ACK, from the slot it sent to
build_test -Wall -O2 -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_ack_match" "$SCRIPT_DIR/test_rf_ack_match.c" \
    $RF50_SRCS "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && "$BUILD_DIR/test_rf_ack_match"

rm -rf "$RF50_MOCK"
