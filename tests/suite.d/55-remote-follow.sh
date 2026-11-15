# hub_rf key fallback to another activity's mapping; the remote follows a Hub activity change only
# on its home and activity screens

# A key the current activity does not map: "-1", "default", then only the single activity of a map
build_test -Wall -O2 -I"$REPO_ROOT/payload/source" -o "$BUILD_DIR/test_rf_keymap_fallback" \
    "$SCRIPT_DIR/test_rf_keymap_fallback.c" "$REPO_ROOT/payload/source/rf_keymap.c" "$REPO_ROOT/payload/source/cJSON.c" \
    -Wl,--wrap=fopen -lpthread -lm
"$BUILD_DIR/test_rf_keymap_fallback"
