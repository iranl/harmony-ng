#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/../build/tmp/busybox-1.36.1-arm"

echo "[-] Configuring without CONFIG_LFS..."
sed -i 's/CONFIG_LFS=y/# CONFIG_LFS is not set/' .config
scripts/kconfig/conf -D .config Config.in

echo "[-] Compiling BusyBox with arm-linux-gnueabi-gcc..."
make -j"$(nproc)" CROSS_COMPILE="arm-linux-gnueabi-" EXTRA_CFLAGS="-U_TIME_BITS -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32" busybox
arm-linux-gnueabi-strip busybox

cp busybox ../../../elite/bin/busybox
echo "[+] Done: ../../../elite/bin/busybox"
