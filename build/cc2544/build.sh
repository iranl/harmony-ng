#!/usr/bin/env bash
set -e

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
TC="$REPO_ROOT/build/toolchains/mips32--uclibc--stable/bin/mips-buildroot-linux-uclibc-"
KDIR="${KERNEL_DIR:-$REPO_ROOT/build/tmp/linux-2.6.31.14}"

if [ ! -d "$KDIR" ]; then
    echo "[-] Error: Kernel directory not found at $KDIR" >&2
    exit 1
fi
export KERNEL_DIR="$KDIR"

echo "[*] Using Kernel directory: $KDIR"
echo "[*] Compiling cc2544.c..."

${TC}gcc -nostdinc \
  -isystem "$REPO_ROOT/build/toolchains/mips32--uclibc--stable/lib/gcc/mips-buildroot-linux-uclibc/5.4.0/include" \
  -I"$KDIR/include" -I"$KDIR/arch/mips/include" -include "$KDIR/include/linux/autoconf.h" \
  -D__KERNEL__ -D"VMLINUX_LOAD_ADDRESS=0xffffffff80100000" -D"DATAOFFSET=0" \
  -Wall -Wundef -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common \
  -Werror-implicit-function-declaration -Wno-format-security -fno-delete-null-pointer-checks \
  -O2 -ffunction-sections -mno-check-zero-division -mabi=32 -G 0 -mno-abicalls -fno-pic -pipe \
  -msoft-float -ffreestanding -march=mips32r2 -Wa,-mips32r2 -Wa,--trap \
  -I"$KDIR/arch/mips/include/asm/mach-malta" \
  -I"$KDIR/arch/mips/include/asm/mach-generic" \
  -Wframe-larger-than=1024 -fno-stack-protector -fomit-frame-pointer \
  -Wdeclaration-after-statement -Wno-pointer-sign -fno-strict-overflow -fno-dwarf2-cfi-asm \
  -DMODULE -mlong-calls -DKBUILD_MODNAME='"cc2544"' \
  -c -o "$SCRIPT_DIR/cc2544.o" "$SCRIPT_DIR/cc2544.c"

echo "[*] Generating cc2544.mod.c..."
python3 "$SCRIPT_DIR/gen_mod.py"

echo "[*] Compiling cc2544.mod.c..."
${TC}gcc -nostdinc \
  -isystem "$REPO_ROOT/build/toolchains/mips32--uclibc--stable/lib/gcc/mips-buildroot-linux-uclibc/5.4.0/include" \
  -I"$KDIR/include" -I"$KDIR/arch/mips/include" -include "$KDIR/include/linux/autoconf.h" \
  -D__KERNEL__ -D"VMLINUX_LOAD_ADDRESS=0xffffffff80100000" -D"DATAOFFSET=0" \
  -Wall -Wundef -Wstrict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common \
  -Werror-implicit-function-declaration -Wno-format-security -fno-delete-null-pointer-checks \
  -O2 -ffunction-sections -mno-check-zero-division -mabi=32 -G 0 -mno-abicalls -fno-pic -pipe \
  -msoft-float -ffreestanding -march=mips32r2 -Wa,-mips32r2 -Wa,--trap \
  -I"$KDIR/arch/mips/include/asm/mach-malta" \
  -I"$KDIR/arch/mips/include/asm/mach-generic" \
  -Wframe-larger-than=1024 -fno-stack-protector -fomit-frame-pointer \
  -Wdeclaration-after-statement -Wno-pointer-sign -fno-strict-overflow -fno-dwarf2-cfi-asm \
  -DMODULE -mlong-calls -DKBUILD_MODNAME='"cc2544"' \
  -c -o "$SCRIPT_DIR/cc2544.mod.o" "$SCRIPT_DIR/cc2544.mod.c"

echo "[*] Linking cc2544.ko..."
${TC}ld -r -m elf32btsmip --build-id -o "$SCRIPT_DIR/cc2544.ko" "$SCRIPT_DIR/cc2544.o" "$SCRIPT_DIR/cc2544.mod.o"

mkdir -p "$REPO_ROOT/payload/modules"
cp -f "$SCRIPT_DIR/cc2544.ko" "$REPO_ROOT/payload/modules/cc2544_custom.ko"

echo "[+] SUCCESS: Built $REPO_ROOT/payload/modules/cc2544_custom.ko"
