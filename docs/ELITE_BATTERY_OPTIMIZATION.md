# Harmony Elite 950 - Battery Life Optimization Guide

The Logitech Harmony Elite 950 remote contains a 1300 mAh rechargeable lithium-ion battery. Because the remote features a color LCD touchscreen, backlit buttons, a 3-axis accelerometer for lift-to-wake, an ARM9 SoC, and a 2.4 GHz CC2544 RF transceiver, optimizing hardware power states can dramatically extend standby and operational battery life.

Expected standby battery life on a fully charged battery is **~3 days**. This is achieved through a combination of hardware and software optimizations that reduce power consumption while the remote is not in use.

---

## Hardware Power Breakdown (Subsystems)

| Subsystem | Active Power | Standby (0-30m) | Deep Standby (>30m) | Optimization Mechanism |
| :--- | :--- | :--- | :--- | :--- |
| **LCD & Backlight** (`ap3156_backlight`) | ~120 - 180 mA | 0 mA | 0 mA | `FB_BLANK_POWERDOWN` (`val=4`) + PWM backlight to 0. |
| **Touchscreen** (Synaptics Clearpad) | ~11 mA | < 0.5 mA | < 0.5 mA | `clearpad_sleep()` writes `0x01` to F01 control reg `0x35`. |
| **Buttons LED** (`tps6507x:white`) | ~35 mA | 0 mA | 0 mA | Button LEDs disabled when screen blanks or daytime. |
| **Accelerometer / Gyro** (`i2c-1/1-0019`) | ~15 mA | ~4.0 mA (1-5 Hz) | 0 mA (0 Hz) | Suspended after 30 min; 0 I2C bus transactions. |
| **CC2544 2.4GHz RF** (`/dev/rfspi`) | ~18 - 24 mA | < 0.5 mA | < 0.5 mA | Radio idle in PM2 sleep; wakes on button event. |
| **ARM926EJ-S SoC** (TI DaVinci DM365) | ~80 mA | ~12 mA | ~8 mA | CPU throttled to 24MHz; `select()` sleeps up to 30s. |

---

## 1. Framebuffer Blanking Fix (`FB_BLANK_POWERDOWN`)

In standard Linux framebuffers, `ioctl(fb_fd, FBIOBLANK, 1)` (`FB_BLANK_NORMAL`) is common. However, the TI DaVinci DM365 `davincifb` driver rejects value `1` with `EINVAL` (`-22`). As a result, the framebuffer remained active in the background, keeping the display controller and DDR memory DMA engine running continuously (~68 mA continuous waste).

### Fix:
In `codex_elite.c`:
```c
/* Pass FB_BLANK_POWERDOWN (4) to actually shut down DaVinci LCD DMA */
ioctl(fb_fd, FBIOBLANK, 4);
```
This properly cuts power to the display controller, saving ~68.5 mA and extending standby from 12.8h to 40h.

---

## 2. Touchscreen Sleep Mode (`clearpad_sleep()`)

When the remote is in standby, touchscreen taps should not wake the remote (accidental pocket/table touches). The Synaptics Clearpad capacitive matrix sensor consumes ~10.5 mA while actively scanning for finger contact.

### Fix:
When entering standby (`fb_set_blank(true)`), `codex_elite` issues an SMBus byte write over I2C bus 1 to the Clearpad controller (`0x20`):
```c
static void clearpad_sleep(void) {
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd >= 0) {
        if (ioctl(fd, I2C_SLAVE_FORCE, 0x20) >= 0) {
            union i2c_smbus_data data;
            struct i2c_smbus_ioctl_data args;
            data.byte = 0x01; /* 0x01 = Sleep Mode */
            args.read_write = I2C_SMBUS_WRITE;
            args.command = 0x35; /* F01 Control Register */
            args.size = I2C_SMBUS_BYTE_DATA;
            args.data = &data;
            ioctl(fd, I2C_SMBUS, &args);
        }
        close(fd);
    }
}
```
Upon screen wake (`fb_set_blank(false)`), the Clearpad input event descriptor is re-polled, instantly restoring full multi-touch sensitivity.

---

## 3. Two-Stage Gyro Polling & Deep Standby (>30m)

The ST LIS3DH 3-axis accelerometer provides lift-to-wake functionality. Stock firmware polls the sensor over I2C at 100 Hz. In `codex_elite`, motion polling uses a 3-tier power-saving schedule:

1. **Active / Recent Standby (0 - 5 min)**:
   - Polling rate: **5 Hz (200 ms)** when stationary, **25 Hz (40 ms)** when moving.
   - Sensor descriptor kept open across iterations to eliminate syscall overhead.
2. **Extended Standby (5 - 30 min)**:
   - Polling rate: throttled to **1 Hz (1,000 ms)** when stationary, **20 Hz (50 ms)** when moving.
   - Reduces I2C traffic by 80% while preserving lift-to-wake responsiveness.
3. **Deep Standby (> 30 min)**:
   - Gyro I2C polling is **completely suspended (0 Hz)**.
   - Sensor descriptor closed. Eliminates all 18,000 I2C reads/hour, saving ~4.0 mA.
   - **Wake Trigger**: Any physical keypad button instantly wakes the remote from Deep Standby.

---

## 4. Main Event Loop Timeout Scaling

The remote's UI and input dispatch thread runs an event-driven `select()` loop monitoring `/dev/input/event0` (physical buttons) and `/dev/rfspi`.
- When active: `select()` timeout is **100 ms**.
- When in Light Standby (0 - 30 min): `select()` timeout scales to **5 seconds**.
- When in Deep Standby (> 30 min): `select()` timeout scales to **30 seconds**.
This cuts unnecessary CPU context switches and timer wakeups to negligible levels, allowing the ARM9 core to stay in low-power Wait-For-Interrupt (WFI) idle.

---

## 5. Wake Triggers & Standby Summary

| Standby State | Duration | Wake Triggers | Active Sensors |
| :--- | :--- | :--- | :--- |
| **Light Standby** | 0 – 30 minutes | • **Motion / Pickup** (LIS3DH accelerometer)<br>• **Any Physical Button** | Gyro (1–5 Hz) |
| **Deep Standby** | > 30 minutes | • **Any Physical Button** (Power, Activities, Devices, OK, Vol, etc.) | None (Hardware IRQ only) |

*Note: Touchscreen glass tap does not wake the remote in either standby mode because Clearpad is powered down to prevent accidental activation.*

---