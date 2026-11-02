/* Emscripten has no <sys/reboot.h>; the renderer never reaches the reboot action. */
#ifndef ELITE_RENDER_WASM_SYS_REBOOT_H
#define ELITE_RENDER_WASM_SYS_REBOOT_H
#define RB_AUTOBOOT 0x01234567
static inline int reboot(int cmd) { (void)cmd; return -1; }
#endif
