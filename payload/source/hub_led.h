#ifndef HARMONY_HUB_LED_H
#define HARMONY_HUB_LED_H

/* Front light of the Hub, driven by the Pimento input driver. Writing a value
 * selects a colour or a blink pattern; the driver does the blinking.
 *
 * A value holds one mode for each colour: red in bits 0-1 and green in bits 2-3.
 * Red and green on together show amber. When both flash at the same rate they
 * flash out of phase, so the light alternates red and green and never flashes
 * amber. When the two modes differ and neither is off, a fast flash turns off
 * and a slow flash turns solid: HUB_LED(HUB_LED_SLOW, HUB_LED_SOLID) shows amber
 * and HUB_LED(HUB_LED_FAST, HUB_LED_SOLID) green. That rule comes from trying all
 * 16 values on a Hub, so only the ten states named below are used. */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define HUB_LED_SYSFS "/sys/class/input/input0/led"

/* A colour's mode */
#define HUB_LED_UNLIT 0
#define HUB_LED_SOLID 1
#define HUB_LED_FAST  2   /* fast flash */
#define HUB_LED_SLOW  3   /* slow flash */

#define HUB_LED(red, green) ((red) | ((green) << 2))

#define HUB_LED_OFF        HUB_LED(HUB_LED_UNLIT, HUB_LED_UNLIT)  /* 0 */
#define HUB_LED_RED        HUB_LED(HUB_LED_SOLID, HUB_LED_UNLIT)  /* 1 */
#define HUB_LED_RED_FAST   HUB_LED(HUB_LED_FAST, HUB_LED_UNLIT)   /* 2 */
#define HUB_LED_RED_SLOW   HUB_LED(HUB_LED_SLOW, HUB_LED_UNLIT)   /* 3 */
#define HUB_LED_GREEN      HUB_LED(HUB_LED_UNLIT, HUB_LED_SOLID)  /* 4 */
#define HUB_LED_AMBER      HUB_LED(HUB_LED_SOLID, HUB_LED_SOLID)  /* 5 */
#define HUB_LED_GREEN_FAST HUB_LED(HUB_LED_UNLIT, HUB_LED_FAST)   /* 8 */
#define HUB_LED_ALT_FAST   HUB_LED(HUB_LED_FAST, HUB_LED_FAST)    /* 10: red and green alternating, very fast */
#define HUB_LED_GREEN_SLOW HUB_LED(HUB_LED_UNLIT, HUB_LED_SLOW)   /* 12 */
#define HUB_LED_ALT_SLOW   HUB_LED(HUB_LED_SLOW, HUB_LED_SLOW)    /* 15: red and green alternating, slow */

#define HUB_LED_DEFAULT_MODE "green"

/* Maps a led_mode setting to a driver value. led_mode is a solid colour or off;
 * the flashing states belong to Hub events. "always_on" was the default before
 * the light had a colour setting, so it maps to the default colour, as do the
 * old flashing values. */
static inline int hub_led_mode_value(const char *mode) {
    if (!mode || !mode[0]) return HUB_LED_GREEN;
    if (strcasecmp(mode, "off") == 0 || strcasecmp(mode, "always_off") == 0) return HUB_LED_OFF;
    if (strcasecmp(mode, "red") == 0) return HUB_LED_RED;
    if (strcasecmp(mode, "amber") == 0 || strcasecmp(mode, "orange") == 0) return HUB_LED_AMBER;
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
