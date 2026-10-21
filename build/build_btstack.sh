#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

SRC="$REPO_ROOT/payload/source/codex_btstack"
BIN="$REPO_ROOT/payload/bin"
OUT="$SCRIPT_DIR/output"
TMP="$SCRIPT_DIR/tmp"
TOOLS="$SCRIPT_DIR/toolchains/mips32--uclibc--stable"
BTSTACK_DIR="$TMP/btstack"

if [ -d "$TOOLS/bin" ]; then
    export PATH="$TOOLS/bin:$PATH"
fi

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
if [ -z "${STRIP:-}" ]; then
    STRIP="strip"
fi

mkdir -p "$BIN" "$OUT" "$TMP"

if [ ! -d "$BTSTACK_DIR/src" ]; then
    echo "Cloning BTstack..."
    git clone --depth 1 https://github.com/bluekitchen/btstack.git "$BTSTACK_DIR"
fi
if [ -f "$SCRIPT_DIR/patches/btstack.patch" ]; then
    (cd "$BTSTACK_DIR" && git apply "$SCRIPT_DIR/patches/btstack.patch" 2>/dev/null || true)
fi

echo "=== Compiling codex_btstack for MIPS32 ==="

cd "$REPO_ROOT"

INCLUDES="-Ipayload/source/codex_btstack -Ipayload/source -Ibuild/tmp/btstack/src -Ibuild/tmp/btstack/platform/posix -Ibuild/tmp/btstack/3rd-party/micro-ecc -Ibuild/tmp/btstack/3rd-party/rijndael -Ibuild/tmp/btstack/3rd-party/bluedroid/encoder/include -Ibuild/tmp/btstack/3rd-party/bluedroid/decoder/include -Ibuild/tmp/btstack/3rd-party/yxml"

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

CODEX_SRCS="
payload/source/cJSON.c
payload/source/codex_btstack/hci_transport_linux.c
payload/source/codex_btstack/codex_btstack.c
"

# Compile and link
$CC $CFLAGS $INCLUDES -o "$BIN/codex_btstack" $BTSTACK_SRCS $CODEX_SRCS -lm
cp -f "$BIN/codex_btstack" "$OUT/codex_btstack.debug"
$STRIP "$BIN/codex_btstack"
cp -f "$BIN/codex_btstack" "$OUT/codex_btstack"

echo "Binary built successfully:"
ls -lh "$BIN/codex_btstack"
file "$BIN/codex_btstack"
