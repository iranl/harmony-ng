# Device save keeps command ranges (post-merge check of #253, finding 6)
python3 "$SCRIPT_DIR/test_device_save_ranges.py"
# Icon pack: each part's CRC once per loaded index, a file replaced under a read checked again,
# an unreadable index logged once per file state, a pack of another version reported with its
# version and not read (post-merge check of #258, findings 7 and 8; whether it is unpacked into
# /tmp is decided by the free RAM, tests/test_hub_route_checks.py case 10)
python3 "$SCRIPT_DIR/icon_pack_verify_fixture.py" "$BUILD_DIR/icon_pack_verify" && \
build_test -Wall -Wextra -O2 -fsanitize=address -I"$REPO_ROOT/payload/source" -DICON_PACK_TEST -DICON_PACK_ARCHIVES="{\"$BUILD_DIR/icon_pack_verify/icons.tar.gz\"}" -o "$BUILD_DIR/test_icon_pack_verify" "$SCRIPT_DIR/test_icon_pack_verify.c" "$REPO_ROOT/payload/source/icon_pack.c" -lpthread && "$BUILD_DIR/test_icon_pack_verify" "$BUILD_DIR/icon_pack_verify"
