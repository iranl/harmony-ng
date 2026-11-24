# The RF slot table and hub_rf's restart (sourced by tests/run_mock_suite.sh): a config push asked
# for while a slot moved, a push skipped during a remote update, the per-slot state after a
# restored table, the way out of the restart, and the table check of the backup import

RF52_SRCS=$(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g")
RF52_MOCK=$(mktemp -d /tmp/hng_rf52.XXXXXX)

build_test -Wall -O2 -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_push_again" "$SCRIPT_DIR/test_rf_push_again.c" \
    $RF52_SRCS "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm
for rf52_case in B D U; do
    HARMONY_MOCK_DIR="$RF52_MOCK/push_$rf52_case" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" "$BUILD_DIR/test_rf_push_again" "$rf52_case"
done

python3 "$SCRIPT_DIR/test_rf_restart_state.py"
python3 "$SCRIPT_DIR/test_import_slot_map.py"

rm -rf "$RF52_MOCK"
