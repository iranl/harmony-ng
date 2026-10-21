import os
import subprocess

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "../.."))
TC = os.path.join(REPO_ROOT, "build/toolchains/mips32--uclibc--stable/bin/mips-buildroot-linux-uclibc-")

# Read stock symbol CRCs from stock cc2544.ko
stock_ko = os.path.join(REPO_ROOT, "scratch/stock_modules/cc2544.ko")
syms = {}

# Parse stock __versions section using readelf
readelf_out = subprocess.check_output([
    TC + "readelf", "-x", "__versions", stock_ko
]).decode('latin1')

# Extract CRCs directly: each struct modversion_info is 68 bytes (4 bytes CRC, 64 bytes name)
with open(stock_ko, "rb") as f:
    elf_data = f.read()

# Locate __versions section header or symbols
# Let's read symbols via nm from cc2544.o
obj_file = os.path.join(SCRIPT_DIR, "cc2544.o")
out = subprocess.check_output([TC + "nm", obj_file]).decode('latin1')
needed = []
for line in out.splitlines():
    parts = line.strip().split()
    if len(parts) == 2 and parts[0] == 'U':
        needed.append(parts[1])

# Parse Module.symvers from kernel directory
symvers_path = "/home/yr/linux-2.6.31.14/Module.symvers"
if not os.path.exists(symvers_path):
    symvers_path = os.path.join(REPO_ROOT, "build/tmp/linux-2.6.31.14/Module.symvers")

if os.path.exists(symvers_path):
    with open(symvers_path, "r") as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 2:
                syms[parts[1]] = int(parts[0], 16)

# Hardcode the known exact CRCs extracted from stock cc2544.ko as ground truth
known_stock_crcs = {
    "module_layout": 0x3d04368f,
    "disable_irq": 0x3ce4ca6f,
    "__kmalloc": 0x12da5bb2,
    "ar7240_flash_spi_up": 0x71a662b5,
    "autoremove_wake_function": 0xc8b57c27,
    "malloc_sizes": 0x08728140,
    "schedule_work": 0xc633495b,
    "cancel_work_sync": 0x23869dc7,
    "__ndelay": 0xdf8c695a,
    "ar7240_gpio_config_int": 0xf455c31b,
    "jiffies": 0x7d11c268,
    "__copy_user": 0xb6091ec0,
    "__init_waitqueue_head": 0xfb58121b,
    "misc_register": 0xaabad1b9,
    "memset": 0x2bc95bd4,
    "printk": 0xea147363,
    "memcmp": 0x71c90087,
    "request_threaded_irq": 0xfda85a7d,
    "kmem_cache_alloc": 0xdc74cc24,
    "schedule": 0x01000e51,
    "__wake_up": 0xb6c70a7d,
    "enable_irq": 0xfcec0987,
    "kfree": 0x037a0cba,
    "prepare_to_wait": 0x8085c7b1,
    "ar7240_flash_spi_down": 0x97caa4b0,
    "finish_wait": 0x51493d94,
    "misc_deregister": 0x96815c9f,
    "__udelay": 0x9e7d6bd0,
    "free_irq": 0xf20dabd8,
}

needed_set = set(needed)
needed_set.add("module_layout")

entries = []
for name in sorted(needed_set):
    crc = known_stock_crcs.get(name)
    if crc is None:
        crc = syms.get(name, 0)
    entries.append((crc, name))

mod_c = f"""#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, "2.6.31-g89d565c mod_unload modversions MIPS32_R2 32BIT ");

struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {{
 .name = KBUILD_MODNAME,
 .init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
 .exit = cleanup_module,
#endif
 .arch = MODULE_ARCH_INIT,
}};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {{
"""

for crc, name in entries:
    mod_c += f'\t{{ 0x{crc:08x}, "{name}" }},\n'

mod_c += """};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";

MODULE_INFO(srcversion, "C5958DD0429705D38F2B8C3");
"""

out_file = os.path.join(SCRIPT_DIR, "cc2544.mod.c")
with open(out_file, "w") as f:
    f.write(mod_c)

print(f"[+] Wrote {out_file} with {len(entries)} symbol version records")
