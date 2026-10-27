#ifndef HARMONY_ELITE_POWER_H
#define HARMONY_ELITE_POWER_H

#include "elite_state.h"

#ifdef __cplusplus
extern "C" {
#endif

extern volatile sig_atomic_t g_wake_requested;
extern char g_timezone_str[48];

void set_backlight_level(int level);
void clearpad_hw_init(void);
void clearpad_wake(bool full_reset);
void clearpad_sleep(void);
bool is_elite_docked(void);
void get_cpu_freq_status(char *out, size_t max_len);
void set_cpu_freq_standby(bool standby);
bool is_night_time(void);
void fb_set_blank(bool blank);
bool is_usb_hardware_connected(void);
void read_elite_battery(void);
void stop_usbeth_and_dropbear(void);
void start_usbeth_and_dropbear(void);
void toggle_usbeth(void);
void enter_kernel_suspend(void);
void trigger_manual_suspend_to_ram(void);
void *power_manager(void *arg);
void set_msp430_rtc(uint32_t epoch_sec);
void sync_time_from_rtc(void);
void expand_timezone(const char *in, char *out, size_t out_sz);
void sig_wake_handler(int sig);
void sig_deep_sleep_handler(int sig);
void init_gyro_interrupt(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_ELITE_POWER_H */
