# Harmony Elite 950 - Battery Life Optimization Guide

The Logitech Harmony Elite 950 remote contains a 1300 mAh rechargeable lithium-ion battery. Because the remote features a color LCD touchscreen, backlit buttons, a 3-axis accelerometer for lift-to-wake, an ARM9 SoC, and a 2.4 GHz CC2544 RF transceiver, optimizing hardware power states can dramatically extend standby and operational battery life.

---

## Hardware Power Breakdown

| Subsystem | Active Power | Idle / Sleep Power | Optimization Strategy |
| :--- | :--- | :--- | :--- |
| **LCD & Backlight** (`ap3156_backlight`) | ~120 - 180 mA | 0 mA (Blanked) | Aggressive blanking timeout (20s) & Level 2 brightness. |
| **Buttons LED** (`tps6507x:white`) | ~35 mA | 0 mA | Turn off button LEDs automatically during daytime or idle. |
| **Accelerometer / Gyro** (`i2c-1/1-0019`) | ~15 mA | ~1.2 mA | Throttle sample rate from 100 Hz to 10 Hz / 25 Hz. |
| **CC2544 2.4GHz RF** (`/dev/rfspi`) | ~18 - 24 mA | < 0.5 mA (PM2 Sleep) | Put CC2544 to sleep when screen blanks; wake on button IRQ. |
| **ARM926EJ-S SoC** (TI DaVinci) | ~80 mA | ~8 mA (WFI Idle) | Avoid busy-polling; use event-driven `select()` / `epoll()`. |

---

## 1. Accelerometer I2C Sample Rate Throttling

The remote's motion sensor driver exposes its data rate via sysfs:
```text
/sys/devices/platform/i2c_davinci.1/i2c-1/1-0019/data_rate
```
- **Stock setting**: `100` (100 samples per second). Generates continuous I2C bus traffic and prevents CPU C-states.
- **Optimized setting**: `10` or `25`. Provides instant lift-to-wake detection while cutting I2C interrupts and CPU wakeups by up to 90%.

### Implementation in `codex_elite.c`:
On boot, `codex_elite` sets:
```c
FILE *f = fopen(GYRO_DATA_RATE_SYS, "w");
if (f) {
    fputs("25\n", f);
    fclose(f);
}
```

---

## 2. CC2544 RF Transceiver Idle Sleep

The CC2544 transceiver supports Power Mode 2 (PM2), consuming less than 0.5 µA in deep sleep while retaining register configuration.

- **Active State**: Radio listens and transmits packets.
- **Sleep State**: When the LCD blanks (`fb_set_blank(true)`), `codex_elite` ceases periodic polling.
- **Wake State**: The physical keypad (`/dev/input/event0`) and gyro (`/dev/input/event1`) are kernel interrupt sources. As soon as a key is pressed or the remote is lifted, the CPU wakes, restores the CC2544 radio, and transmits the pending button event within < 12 milliseconds.

---

## 3. LCD Screen Timeout & Backlight Level

In the remote's **Settings** menu:
- **Display Timeout**: Recommended **30s** (saves ~35% battery compared to 120s).
- **Display Brightness**: Level **2** or **3** provides clear high-contrast readability on the OLED/LCD while reducing backlight LED consumption by up to 40%.

---

## 4. Disabling USB Ethernet When Not in Dock

When not docked or tethered for debugging, ensure USB Ethernet is toggled **OFF** in Settings:
```bash
/usr/bin/usbeth stop
```
Active USB gadget controller clocks consume ~18-22 mA continuously if left enabled while running on battery.
