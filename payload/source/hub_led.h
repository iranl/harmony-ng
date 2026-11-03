#ifndef HARMONY_HUB_LED_H
#define HARMONY_HUB_LED_H

/* Front light of the Hub, driven by the Pimento input driver. Writing a value
 * selects a colour or a blink pattern; the driver does the blinking. */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define HUB_LED_SYSFS "/sys/class/input/input0/led"

#define HUB_LED_OFF        0
#define HUB_LED_RED        1
#define HUB_LED_RED_FAST   2   /* red, fast flash */
#define HUB_LED_RED_SLOW   3   /* red, slow flash */
#define HUB_LED_GREEN      4
#define HUB_LED_AMBER      5
#define HUB_LED_RED_AMBER  10  /* red and amber alternating */

#define HUB_LED_DEFAULT_MODE "green"

/* Maps a led_mode setting to a driver value. "always_on" was the default before
 * the light had a colour setting, so it maps to the default colour. */
static inline int hub_led_mode_value(const char *mode) {
    if (!mode || !mode[0]) return HUB_LED_GREEN;
    if (strcasecmp(mode, "off") == 0 || strcasecmp(mode, "always_off") == 0) return HUB_LED_OFF;
    if (strcasecmp(mode, "red") == 0) return HUB_LED_RED;
    if (strcasecmp(mode, "amber") == 0 || strcasecmp(mode, "orange") == 0) return HUB_LED_AMBER;
    if (strcasecmp(mode, "flashing_fast") == 0 || strcasecmp(mode, "fast") == 0) return HUB_LED_RED_FAST;
    if (strcasecmp(mode, "flashing_slow") == 0 || strcasecmp(mode, "slow") == 0) return HUB_LED_RED_SLOW;
    return HUB_LED_GREEN;
}

static inline void hub_led_write(int value) {
    int fd = open(HUB_LED_SYSFS, O_WRONLY);
    if (fd < 0) return;
    char buf[8];
    int n = snprintf(buf, sizeof(buf), "%d\n", value);
    ssize_t w = write(fd, buf, (size_t)n);
    (void)w;
    close(fd);
}

#endif /* HARMONY_HUB_LED_H */
