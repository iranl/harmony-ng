#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, "2.6.31-g89d565c mod_unload modversions MIPS32_R2 32BIT ");

struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
 .name = KBUILD_MODNAME,
 .init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
 .exit = cleanup_module,
#endif
 .arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0xb6091ec0, "__copy_user" },
	{ 0xfb58121b, "__init_waitqueue_head" },
	{ 0x12da5bb2, "__kmalloc" },
	{ 0xdf8c695a, "__ndelay" },
	{ 0x00000000, "__this_module" },
	{ 0x9e7d6bd0, "__udelay" },
	{ 0xb6c70a7d, "__wake_up" },
	{ 0x97caa4b0, "ar7240_flash_spi_down" },
	{ 0x71a662b5, "ar7240_flash_spi_up" },
	{ 0xf455c31b, "ar7240_gpio_config_int" },
	{ 0xc8b57c27, "autoremove_wake_function" },
	{ 0x23869dc7, "cancel_work_sync" },
	{ 0x3ce4ca6f, "disable_irq" },
	{ 0xfcec0987, "enable_irq" },
	{ 0x51493d94, "finish_wait" },
	{ 0xf20dabd8, "free_irq" },
	{ 0x7d11c268, "jiffies" },
	{ 0x037a0cba, "kfree" },
	{ 0xdc74cc24, "kmem_cache_alloc" },
	{ 0x08728140, "malloc_sizes" },
	{ 0x71c90087, "memcmp" },
	{ 0x2bc95bd4, "memset" },
	{ 0x96815c9f, "misc_deregister" },
	{ 0xaabad1b9, "misc_register" },
	{ 0x3d04368f, "module_layout" },
	{ 0x8085c7b1, "prepare_to_wait" },
	{ 0xea147363, "printk" },
	{ 0xfda85a7d, "request_threaded_irq" },
	{ 0x01000e51, "schedule" },
	{ 0xc633495b, "schedule_work" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";

MODULE_INFO(srcversion, "C5958DD0429705D38F2B8C3");
