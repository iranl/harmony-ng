#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

SRC="$REPO_ROOT/payload/source/hub_btstack"
BIN="$REPO_ROOT/payload/bin"
OUT="$SCRIPT_DIR/output"
TMP="$SCRIPT_DIR/tmp"
BTSTACK_DIR="$TMP/btstack"

for t in "$SCRIPT_DIR/toolchains"/mips32--uclibc--stable*; do
    if [ -d "$t/bin" ]; then
        export PATH="$t/bin:$PATH"
        break
    fi
done

if [ -z "${CC:-}" ]; then
    if command -v mips-buildroot-linux-uclibc-gcc >/dev/null 2>&1; then
        CC="mips-buildroot-linux-uclibc-gcc"
        STRIP="mips-buildroot-linux-uclibc-strip"
    elif command -v mips-linux-gnu-gcc >/dev/null 2>&1; then
        CC="mips-linux-gnu-gcc"
        STRIP="mips-linux-gnu-strip"
    else
        CC="mips-buildroot-linux-uclibc-gcc"
        STRIP="mips-buildroot-linux-uclibc-strip"
    fi
fi
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "[!] $CC not found. Extract the Bootlin mips32--uclibc--stable toolchain into build/toolchains/." >&2
    exit 1
fi
if [ -z "${STRIP:-}" ]; then
    STRIP="strip"
fi

mkdir -p "$BIN" "$OUT" "$TMP"

# BTstack is pinned: master moves, and build/patches/btstack.patch (four HID host instances and
# more) is made for this commit. To update, change the commit and check that the patch applies.
BTSTACK_REPO="https://github.com/bluekitchen/btstack.git"
BTSTACK_COMMIT="9edb5d82ea6ec4466a393415d65030929330acdf"  # master of 2026-10-30
BTSTACK_PATCH="$SCRIPT_DIR/patches/btstack.patch"

if [ -d "$BTSTACK_DIR" ] && [ "$(git -C "$BTSTACK_DIR" rev-parse HEAD 2>/dev/null || true)" != "$BTSTACK_COMMIT" ]; then
    echo "BTstack in $BTSTACK_DIR is not $BTSTACK_COMMIT, fetching it again..."
    rm -rf "$BTSTACK_DIR"
fi
if [ ! -d "$BTSTACK_DIR/src" ]; then
    echo "Fetching BTstack $BTSTACK_COMMIT..."
    rm -rf "$BTSTACK_DIR"
    git init -q "$BTSTACK_DIR"
    git -C "$BTSTACK_DIR" fetch -q --depth 1 "$BTSTACK_REPO" "$BTSTACK_COMMIT"
    git -C "$BTSTACK_DIR" checkout -q FETCH_HEAD
fi
# >>> patch step (tests/test_build_btstack_patch.sh runs this block on a toy repository)
# The patch is applied to a clean tree on every build. A check of the current patch against
# a clone left by an earlier build cannot see a change that was removed from the patch since:
# it stayed in the clone and was compiled. Resetting the local clone costs nothing.
if [ ! -f "$BTSTACK_PATCH" ]; then
    echo "[!] $BTSTACK_PATCH is missing: hub_btstack needs it (four HID host instances and more)" >&2
    exit 1
fi
(cd "$BTSTACK_DIR" && git checkout -q -- . && git clean -fdq)
if (cd "$BTSTACK_DIR" && git apply --ignore-whitespace --whitespace=nowarn "$BTSTACK_PATCH"); then
    echo "Applied btstack.patch"
else
    echo "[!] $BTSTACK_PATCH does not apply to BTstack $BTSTACK_COMMIT" >&2
    exit 1
fi
# <<< patch step

echo "=== Compiling hub_btstack for MIPS32 ==="

cd "$REPO_ROOT"

INCLUDES="-Ipayload/source/hub_btstack -Ipayload/source -Ibuild/tmp/btstack/src -Ibuild/tmp/btstack/platform/posix -Ibuild/tmp/btstack/3rd-party/micro-ecc -Ibuild/tmp/btstack/3rd-party/rijndael -Ibuild/tmp/btstack/3rd-party/bluedroid/encoder/include -Ibuild/tmp/btstack/3rd-party/bluedroid/decoder/include -Ibuild/tmp/btstack/3rd-party/yxml"

CFLAGS="-Os -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -static -DNDEBUG -ffunction-sections -fdata-sections -Wl,--gc-sections"

BTSTACK_SRCS="
build/tmp/btstack/src/ad_parser.c
build/tmp/btstack/src/btstack_crypto.c
build/tmp/btstack/src/btstack_hid_parser.c
build/tmp/btstack/src/btstack_linked_list.c
build/tmp/btstack/src/btstack_memory.c
build/tmp/btstack/src/btstack_memory_pool.c
build/tmp/btstack/src/btstack_run_loop.c
build/tmp/btstack/src/btstack_run_loop_base.c
build/tmp/btstack/src/btstack_tlv.c
build/tmp/btstack/src/btstack_util.c
build/tmp/btstack/src/hci.c
build/tmp/btstack/src/hci_cmd.c
build/tmp/btstack/src/hci_dump.c
build/tmp/btstack/src/hci_event.c
build/tmp/btstack/src/hci_event_builder.c
build/tmp/btstack/src/l2cap.c
build/tmp/btstack/src/l2cap_signaling.c
build/tmp/btstack/src/ble/att_db.c
build/tmp/btstack/src/ble/att_dispatch.c
build/tmp/btstack/src/ble/att_server.c
build/tmp/btstack/src/ble/gatt_client.c
build/tmp/btstack/src/ble/le_device_db_tlv.c
build/tmp/btstack/src/ble/sm.c
build/tmp/btstack/src/ble/gatt-service/hids_host.c
build/tmp/btstack/3rd-party/rijndael/rijndael.c
build/tmp/btstack/3rd-party/micro-ecc/uECC.c
build/tmp/btstack/src/classic/sdp_server.c
build/tmp/btstack/src/classic/sdp_util.c
build/tmp/btstack/src/classic/device_id_server.c
build/tmp/btstack/src/classic/hid_device.c
build/tmp/btstack/src/classic/btstack_link_key_db_tlv.c
build/tmp/btstack/platform/posix/btstack_run_loop_posix.c
build/tmp/btstack/platform/posix/btstack_tlv_posix.c
build/tmp/btstack/platform/posix/hci_dump_posix_stdout.c
"

HUB_BT_SRCS="
payload/source/cJSON.c
payload/source/hub_btstack/hci_transport_linux.c
payload/source/hub_btstack/bt_state.c
payload/source/hub_btstack/bt_pairing.c
payload/source/hub_btstack/bt_status_json.c
payload/source/hub_btstack/bt_hid.c
payload/source/hub_btstack/bt_cmd.c
payload/source/hub_btstack/bt_adv.c
payload/source/hub_btstack/bt_listen.c
payload/source/hub_btstack/bt_remote.c
payload/source/hub_btstack/bt_main.c
payload/source/hub_btstack/bt_reconnect_queue.c
payload/source/hub_btstack/bt_le_pair_window.c
payload/source/hub_btstack/bt_hid_queue.c
"

# Compile and link
$CC $CFLAGS $INCLUDES -o "$BIN/hub_btstack" $BTSTACK_SRCS $HUB_BT_SRCS -lm
cp -f "$BIN/hub_btstack" "$OUT/hub_btstack.debug"
$STRIP "$BIN/hub_btstack"
cp -f "$BIN/hub_btstack" "$OUT/hub_btstack"

# Dev build with BTstack info logs, for debugging only. It stays in build/output
# (never in payload/bin, so no release or OTA ships it); deploy_hub.py --dev-btstack installs it.
$CC $CFLAGS -DHUB_BT_DEBUG_LOGS $INCLUDES -o "$OUT/hub_btstack_dev" $BTSTACK_SRCS $HUB_BT_SRCS -lm
# its addresses differ from the release build: a dev CRASH: line needs this .debug file
cp -f "$OUT/hub_btstack_dev" "$OUT/hub_btstack_dev.debug"
$STRIP "$OUT/hub_btstack_dev"

echo "Binary built successfully:"
ls -lh "$BIN/hub_btstack" "$OUT/hub_btstack_dev"
file "$BIN/hub_btstack"
