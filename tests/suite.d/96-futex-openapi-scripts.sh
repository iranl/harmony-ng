# No futex waits in Hub code, OpenAPI contract, device shell scripts on the host
# (moved from tests/run_mock_suite.sh; the suite sources this file)

# 5d. No futex-based waits in Hub code: the Hub's kernel has no futex system call, so such a
# wait never sleeps there and spins. The behaviour tests run under tests/no_futex_exec, which
# makes every futex wait return at once, as on the Hub (skipped where seccomp is not available).
python3 "$SCRIPT_DIR/test_hub_no_futex_waits.py"
gcc -Wall -O2 -o "$BUILD_DIR/no_futex_exec" "$SCRIPT_DIR/no_futex_exec.c"
NO_FUTEX="$BUILD_DIR/no_futex_exec"
"$NO_FUTEX" /bin/true 2>/dev/null || { echo "    (seccomp not available: futex waits sleep in the tests below)"; NO_FUTEX=""; }
# hub_rf's wait for a remote's ACK (all hub_rf sources as build/compile_all.sh lists them, main renamed)
build_test -Wall -O2 -include sys/stat.h -include time.h -Dmain=hub_rf_main -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_ack_wait" "$SCRIPT_DIR/test_rf_ack_wait.c" \
    $(sed -n 's/^ *RF_SRCS="\(.*\)"$/\1/p' "$REPO_ROOT/build/compile_all.sh" | sed "s|payload/|$REPO_ROOT/payload/|g") "$REPO_ROOT/payload/source/cJSON.c" -lpthread -lm && \
    $NO_FUTEX "$BUILD_DIR/test_rf_ack_wait"
# IR capture mode turned off while the receiver's read is stalled
build_test -Wall -Wextra -O2 -pthread -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_ir_rx_wait_cpu" "$SCRIPT_DIR/test_ir_rx_wait_cpu.c" "$REPO_ROOT/payload/source/ir_i2s.c" && \
    HARMONY_MOCK_DIR="/tmp/harmony_mock" LD_PRELOAD="$BUILD_DIR/libmock_hardware.so" $NO_FUTEX "$BUILD_DIR/test_ir_rx_wait_cpu"

# 6. Run OpenAPI Specification Contract Validation (docs/openapi.yaml must parse as YAML)
python3 "$SCRIPT_DIR/test_ethernet_apply.py"
echo "[-] Validating docs/openapi.yaml with PyYAML..."
python3 "$SCRIPT_DIR/test_mqtt_device_command.py"
python3 -c "import yaml, sys; yaml.safe_load(open('$REPO_ROOT/docs/openapi.yaml')); print('    openapi.yaml parses')"
# Every C file in payload/source is built by a build script or a test
python3 "$SCRIPT_DIR/test_source_orphans.py"
echo "[-] Running OpenAPI Specification Test Suite..."
# --harness: the live contract tests run against hub_daemon_test (they skipped without a daemon)
python3 "$SCRIPT_DIR/test_openapi.py" --harness --port 9262
python3 "$SCRIPT_DIR/test_mqtt_ha_template.py"

# 7. Device shell scripts run on the host (paths moved to a temp dir)
python3 "$SCRIPT_DIR/test_hub_user_sh.py"
