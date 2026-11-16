# Harmony Remote - Battery Life Optimization Guide

The Logitech Harmony touchscreen remotes contain a rechargeable lithium-ion battery (1300 mAh on the Elite 950). Because the remote features a color LCD touchscreen, backlit buttons, a 3-axis accelerometer for lift-to-wake, an ARM9 SoC, and a 2.4 GHz CC2544 RF transceiver, optimizing hardware power states can dramatically extend standby and operational battery life.

Expected standby battery life on a fully charged battery is **~3 days**. This is achieved through a combination of hardware and software optimizations that reduce power consumption while the remote is not in use.

This guide covers the hardware power states. How the battery percentage is worked out is in [REMOTE.md](REMOTE.md#4-power) (`Battery Level`); when the remote suspends, how it counts the time asleep and when it reports its battery to the Hub is in [REMOTE.md](REMOTE.md#sleep-behavior--expected-run-times).

---

## Hardware Power Breakdown (Subsystems)

Light standby lasts from the screen blank until the suspend to RAM (deep standby) starts. The
**Deep Standby** setting chooses when that is: at once by default on battery, or after 15 to 240
minutes, or never. On the cradle, on USB or while charging the remote never suspends.

| Subsystem | Active Power | Light Standby | Deep Standby (suspend) | Optimization Mechanism |
| :--- | :--- | :--- | :--- | :--- |
| **LCD & Backlight** (`ap3156_backlight`) | ~120 - 180 mA | 0 mA | 0 mA | `FB_BLANK_POWERDOWN` (`val=4`) + PWM backlight to 0. |
| **Touchscreen** (Synaptics Clearpad) | ~11 mA | in its own doze (not measured) | in its own doze (not measured) | Left to the kernel's `clearpad` driver, which never puts it into sleep mode (section 2). |
| **Buttons LED** (`tps6507x:white`) | ~35 mA | 0 mA | 0 mA | Off when the screen blanks. With the screen on: OFF never, ON always, NIGHT (the default) only inside the night window. |
| **Accelerometer / Gyro** (`i2c-1/1-0019`) | ~15 mA | ~4.0 mA (1-5 Hz) | 0 mA (0 Hz) | Interrupt only (INT1) while the screen is off; 0 I2C bus transactions. |
| **CC2544 2.4GHz RF** (`/dev/rfspi`) | ~18 - 24 mA | < 0.5 mA | < 0.5 mA | Radio idle in PM2 sleep; wakes on button event. |
| **ARM926EJ-S SoC** (TI DaVinci DM365) | ~80 mA | ~12 mA | ~8 mA | `select()` sleeps up to 30s. The standby figures were measured with the processor stepped down to 96 MHz; that step is gone (it could stop the remote dead, see REMOTE.md), so awake with the screen off it now draws more. |

---

## 1. Framebuffer Blanking Fix (`FB_BLANK_POWERDOWN`)

In standard Linux framebuffers, `ioctl(fb_fd, FBIOBLANK, 1)` (`FB_BLANK_NORMAL`) is common. However, the TI DaVinci DM365 `davincifb` driver rejects value `1` with `EINVAL` (`-22`). As a result, the framebuffer remained active in the background, keeping the display controller and DDR memory DMA engine running continuously (~68 mA continuous waste).

### Fix:
In `fb_set_blank()` (`payload/source/remote/remote_power.c`):
```c
/* FB_BLANK_POWERDOWN (4) actually shuts down the DaVinci LCD DMA */
ioctl(g_fb_fd, FBIOBLANK, blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK);
```
This properly cuts power to the display controller, saving ~68.5 mA and extending standby from 12.8h to 40h.

---

## 2. Touchscreen: left to the kernel driver

The Synaptics Clearpad capacitive sensor consumes ~10.5 mA while actively scanning for finger contact.

`remote_ui` does not write to the touch controller. The kernel's `clearpad` driver owns the controller: at a suspend it masks the touch interrupt, at the resume it reads the controller once and unmasks it. Nobody puts the controller into its sleep mode, on stock either; it dozes by itself between touches. With the screen off and the remote awake (light standby) a touch wakes the screen when touch is enabled in Settings.

Older builds wrote the controller's registers over raw I2C (`/dev/i2c-1`, address `0x20`) at every blank, wake and suspend, next to the kernel driver. The one before a suspend put the sensor into its sleep mode. Those writes only ran on remotes where `/dev/i2c-1` had been created by hand, and touch worked the same without them, so they were removed. On remotes that had the node the controller now stays in its doze during a suspend, as on stock and on every other remote, so deep standby may draw slightly more there (not measured).

---

## 3. Hardware Interrupt-Driven Motion Wake (LIS3DH INT1)

The ST LIS3DH 3-axis accelerometer provides lift-to-wake functionality. In `remote_ui`, motion wake is entirely **event-driven via hardware interrupt**:

1. **Light Standby** (screen off, before the suspend):
   - Configures LIS3DH INT1 threshold (`int1_threshold=5`, `int1_duration=1`) at start.
   - Motion assertions route directly to Linux GPIO IRQ 108 (`/dev/input/event1`).
   - Main event thread `select()` multiplexes `/dev/input/event1` with physical buttons whenever the screen is off, Tilt / Pickup Wake is on and the remote is off the cradle.
   - **Zero I2C bus transactions** while stationary (eliminates 18,000 I2C reads/hour, saving ~4.0 mA).
   - Instantaneous wake on pickup when acceleration exceeds threshold.
2. **Deep Standby** (suspend to RAM, on battery):
   - The processor sleeps; motion does not wake it.
   - **Wake Triggers**: any physical keypad button, cradle or USB power, and the hourly MSP430 alarm.

---

## 4. Main Event Loop & Power Watcher Throttling

The remote's UI and input dispatch thread runs an event-driven `select()` loop monitoring `/dev/input/event0` (physical buttons), `/dev/input/event2` (touchscreen and capacitive keys) and, with the screen off, motion wake on and the remote on battery, `/dev/input/event1` (accelerometer motion interrupt). The radio (`/dev/rfspi`) cannot be polled: a thread of its own reads it with a blocking `read()`.
- When active: `select()` timeout is **100 ms**.
- With the screen off: **2 seconds**.
- Once deep standby is due on battery: **30 seconds** (the remote then normally suspends).
- The background `power_manager` thread wakes every **0.2 to 0.5 seconds**, with the screen on or off.
- Battery voltage sampling (`read_remote_battery()`) happens at most **once a second**; with the screen off on battery the power thread asks for it on every round.

---

## 5. Wake Triggers & Standby Summary

| Standby State | Duration | Wake Triggers | Active Sensors |
| :--- | :--- | :--- | :--- |
| **Docked / Charging** | Unlimited | • **Dock Connect / Disconnect** (cradle ↔ battery transition)<br>• **Any Physical Button**<br>• **A touch or a capacitive key** (when enabled)<br>• **The Hub** over RF | TPS6507x PMIC (AC powered, no battery drain); motion is not watched while powered |
| **Light Standby** | From the blank until deep standby starts | • **Motion / Pickup** (LIS3DH INT1 hardware IRQ)<br>• **Any Physical Button**<br>• **A touch or a capacitive key** (when enabled)<br>• **The Hub** over RF | LIS3DH INT1 (Hardware IRQ only, 0 I2C reads) |
| **Deep Standby** | Suspend to RAM on battery (at once by default) | • **Any Physical Button** (Power, Activities, Devices, OK, Vol, etc.)<br>• **Placing on Dock** (cradle connect)<br>• **The hourly MSP430 alarm** (reports, then suspends again) | None (Hardware IRQ only) |

*Note: In light standby a touch on the glass wakes the screen when touch is enabled in Settings. A suspended remote (deep standby) does not wake on touch.*

---