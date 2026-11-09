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
if [ -f "$BTSTACK_PATCH" ]; then
    # A patch that does not apply used to be ignored, building hub_btstack without it
    if (cd "$BTSTACK_DIR" && git apply --check --reverse --ignore-whitespace --whitespace=nowarn "$BTSTACK_PATCH" 2>/dev/null); then
        echo "btstack.patch already applied"
    elif (cd "$BTSTACK_DIR" && git apply --ignore-whitespace --whitespace=nowarn "$BTSTACK_PATCH"); then
        echo "Applied btstack.patch"
    elif (cd "$BTSTACK_DIR" && git checkout -q -- . && git clean -fdq && git apply --ignore-whitespace --whitespace=nowarn "$BTSTACK_PATCH"); then
        # A clone left by an earlier build carries the previous version of the patch
        echo "Applied btstack.patch to a reset clone"
    else
        echo "[!] $BTSTACK_PATCH does not apply to BTstack $BTSTACK_COMMIT" >&2
        exit 1
    fi
fi

echo "=== Compiling hub_btstack for MIPS32 ==="

cd "$REPO_ROOT"

INCLUDES="-Ipayload/source/hub_btstack -Ipayload/source -Ibuild/tmp/btstack/src -Ibuild/tmp/btstack/platform/posix -Ibuild/tmp/btstack/3rd-party/micro-ecc -Ibuild/tmp/btstack/3rd-party/rijndael -Ibuild/tmp/btstack/3rd-party/bluedroid/encoder/include -Ibuild/tmp/btstack/3rd-party/bluedroid/decoder/include -Ibuild/tmp/btstack/3rd-party/yxml"

CFLAGS="-Os -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -static -DNDEBUG"

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
payload/source/hub_btstack/bt_remote.c
payload/source/hub_btstack/bt_main.c
payload/source/hub_btstack/bt_reconnect_queue.c
"

# Compile and link
$CC $CFLAGS $INCLUDES -o "$BIN/hub_btstack" $BTSTACK_SRCS $HUB_BT_SRCS -lm
cp -f "$BIN/hub_btstack" "$OUT/hub_btstack.debug"
$STRIP "$BIN/hub_btstack"
cp -f "$BIN/hub_btstack" "$OUT/hub_btstack"

echo "Binary built successfully:"
ls -lh "$BIN/hub_btstack"
file "$BIN/hub_btstack"
