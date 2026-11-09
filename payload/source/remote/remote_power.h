#ifndef HARMONY_REMOTE_POWER_H
#define HARMONY_REMOTE_POWER_H

#include "remote_state.h"

#ifdef __cplusplus
extern "C" {
#endif

extern volatile sig_atomic_t g_wake_requested;
extern char g_timezone_str[64];

void set_backlight_level(int level);
void clearpad_hw_init(void);
void clearpad_wake(bool full_reset);
void clearpad_doze(void);
void clearpad_sleep(void);
bool is_remote_docked(void);
void get_cpu_freq_status(char *out, size_t max_len);
void set_cpu_freq_standby(bool standby);
bool is_night_time(void);
void fb_set_blank(bool blank);
bool is_usb_hardware_connected(void);
void read_remote_battery(void);
time_t ui_wall_time(void);
void stop_usbeth_and_dropbear(void);
void start_usbeth_and_dropbear(void);
void enter_kernel_suspend(void);
/* Deep standby (suspend to RAM, on battery) is due after the screen has been off this long:
 * the deep_standby setting, -1 = at once (2 s, time for a key release and its ACK), 0 = never */
#define REMOTE_SUSPEND_AT_ONCE_S 2
bool remote_deep_standby_due(uint64_t standby_s);
/* The whole suspend decision with the screen off: never when docked, on USB or charging (the
 * remote stays reachable for RF wake, config pushes and RF updates), never during a config sync
 * or an RF update or an alarm-wake hold, and only on battery once deep standby is due */
bool remote_should_suspend(uint64_t standby_s, uint64_t now_ms);
/* After an alarm wake the remote stays up until it has reported its battery (or this long),
 * then suspends again: without it a sleeping remote never reported */
#define REMOTE_ALARM_WAKE_HOLD_MS 8000ULL
extern uint64_t g_suspend_hold_until_ms;
/* Called after a battery report went out: an alarm-wake hold can end now */
void remote_power_report_sent(void);
/* After a resume from suspend to RAM that lasted suspended_ms: fresh battery samples, the next
 * reading counts as rested when the suspend was long enough, and a battery report is requested */
void remote_power_resumed(uint64_t suspended_ms);
void trigger_manual_suspend_to_ram(void);
void *power_manager(void *arg);
void set_msp430_rtc(uint32_t epoch_sec);
void sync_time_from_rtc(void);
void expand_timezone(const char *in, char *out, size_t out_sz);
void sig_wake_handler(int sig);
void sig_deep_sleep_handler(int sig);
void init_gyro_interrupt(void);
void remote_wdt_init(void);
void remote_wdt_ping(void);
void remote_wdt_close(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_POWER_H */
