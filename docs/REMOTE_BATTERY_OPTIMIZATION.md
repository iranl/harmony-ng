# Harmony Remote - Battery Life Optimization Guide

The Logitech Harmony touchscreen remotes contain a rechargeable lithium-ion battery (1300 mAh on the Elite 950). Because the remote features a color LCD touchscreen, backlit buttons, a 3-axis accelerometer for lift-to-wake, an ARM9 SoC, and a 2.4 GHz CC2544 RF transceiver, optimizing hardware power states can dramatically extend standby and operational battery life.

Expected standby battery life on a fully charged battery is **~3 days**. This is achieved through a combination of hardware and software optimizations that reduce power consumption while the remote is not in use.

---

## Hardware Power Breakdown (Subsystems)

| Subsystem | Active Power | Standby (0-30m) | Deep Standby (>30m) | Optimization Mechanism |
| :--- | :--- | :--- | :--- | :--- |
| **LCD & Backlight** (`ap3156_backlight`) | ~120 - 180 mA | 0 mA | 0 mA | `FB_BLANK_POWERDOWN` (`val=4`) + PWM backlight to 0. |
| **Touchscreen** (Synaptics Clearpad) | ~11 mA | in its own doze (not measured) | in its own doze (not measured) | Left to the kernel's `clearpad` driver, which never puts it into sleep mode (section 2). |
| **Buttons LED** (`tps6507x:white`) | ~35 mA | 0 mA | 0 mA | Button LEDs disabled when screen blanks or daytime. |
| **Accelerometer / Gyro** (`i2c-1/1-0019`) | ~15 mA | ~4.0 mA (1-5 Hz) | 0 mA (0 Hz) | Suspended after 30 min; 0 I2C bus transactions. |
| **CC2544 2.4GHz RF** (`/dev/rfspi`) | ~18 - 24 mA | < 0.5 mA | < 0.5 mA | Radio idle in PM2 sleep; wakes on button event. |
| **ARM926EJ-S SoC** (TI DaVinci DM365) | ~80 mA | ~12 mA | ~8 mA | `select()` sleeps up to 30s. The standby figures were measured with the processor stepped down to 96 MHz; that step is gone (it could stop the remote dead, see REMOTE.md), so awake with the screen off it now draws more. |

---

## 1. Framebuffer Blanking Fix (`FB_BLANK_POWERDOWN`)

In standard Linux framebuffers, `ioctl(fb_fd, FBIOBLANK, 1)` (`FB_BLANK_NORMAL`) is common. However, the TI DaVinci DM365 `davincifb` driver rejects value `1` with `EINVAL` (`-22`). As a result, the framebuffer remained active in the background, keeping the display controller and DDR memory DMA engine running continuously (~68 mA continuous waste).

### Fix:
In `remote_ui.c`:
```c
/* Pass FB_BLANK_POWERDOWN (4) to actually shut down DaVinci LCD DMA */
ioctl(fb_fd, FBIOBLANK, 4);
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

1. **Light Standby (0 - 30 min)**:
   - Configures LIS3DH INT1 threshold (`int1_threshold=10`, `int1_duration=3`).
   - Motion assertions route directly to Linux GPIO IRQ 108 (`/dev/input/event1`).
   - Main event thread `select()` multiplexes `/dev/input/event1` with physical buttons.
   - **Zero I2C bus transactions** while stationary (eliminates 18,000 I2C reads/hour, saving ~4.0 mA).
   - Instantaneous wake on pickup when acceleration exceeds threshold.
2. **Deep Standby (> 30 min)**:
   - Accelerometer descriptor is removed from `select()` sets.
   - Prevents accidental nudges from waking the remote during sleep.
   - **Wake Trigger**: Any physical keypad button instantly wakes the remote from Deep Standby.

---

## 4. Main Event Loop & Power Watcher Throttling

The remote's UI and input dispatch thread runs an event-driven `select()` loop monitoring `/dev/input/event0` (physical buttons), `/dev/input/event1` (accelerometer motion interrupt), and `/dev/rfspi`.
- When active: `select()` timeout is **100 ms**.
- When in Light Standby (0 - 30 min): `select()` timeout scales to **10 seconds**.
- When in Deep Standby (> 30 min): `select()` timeout scales to **30 seconds**.
- Background `power_manager` sleeps for **10 seconds** in standby (reduced from 1s).
- Battery voltage sampling (`read_remote_battery()`) throttles from every 60s to **300s (5 min)** in standby.

---

## 5. Wake Triggers & Standby Summary

| Standby State | Duration | Wake Triggers | Active Sensors |
| :--- | :--- | :--- | :--- |
| **Docked / Charging** | Unlimited | • **Dock Connect / Disconnect** (cradle ↔ battery transition)<br>• **Motion / Pickup** (LIS3DH INT1 hardware IRQ)<br>• **Any Physical Button** | LIS3DH INT1 & TPS6507x PMIC (AC powered, no battery drain) |
| **Light Standby** | 0 – 30 minutes | • **Motion / Pickup** (LIS3DH INT1 hardware IRQ)<br>• **Any Physical Button** | LIS3DH INT1 (Hardware IRQ only, 0 I2C reads) |
| **Deep Standby** | > 30 minutes | • **Any Physical Button** (Power, Activities, Devices, OK, Vol, etc.)<br>• **Placing on Dock** (cradle connect) | None (Hardware IRQ only) |

*Note: In light standby a touch on the glass wakes the screen when touch is enabled in Settings. A suspended remote (deep standby) does not wake on touch.*

---