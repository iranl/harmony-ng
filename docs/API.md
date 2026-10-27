# Harmony Hub HTTP & WebSocket API Specification

`hub_daemon` provides an ultra-lightweight REST and WebSocket interface running natively on port 8089 (HTTP / WebSocket). RF hardware calls are transparently proxied to `hub_rf` on port 8092 via `/api/rf/*`.

---

## 1. System, Status & Diagnostics

### 1.1 Health Check
#### `GET /health` or `GET /`
Returns service status.
- **Response `200 OK`**:
  ```json
  {
    "status": "ok",
    "service": "hub_daemon"
  }
  ```

### 1.2 Full Hub Status
#### `GET /api/status`
Returns high-level hub status, firmware, active activity, memory, uptime, and MQTT connection status.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "hub_name": "Living Room Hub",
    "firmware": "4.15.600",
    "activity": "123456",
    "state": 2,
    "busy": false,
    "ip": "192.168.1.100",
    "total_kb": 5120,
    "free_kb": 1372,
    "uptime_sec": 34821,
    "mqtt": {
      "connected": true,
      "host": "192.168.1.50",
      "port": 1883,
      "base_topic": "harmony/hub"
    }
  }
  ```

### 1.3 System Info
#### `GET /api/system-info`
Returns Linux system uptime, CPU load average, kernel version, RAM breakdown, and network statistics.
- **Response `200 OK`**:
  ```json
  {
    "uptime": "1d 4h 12m (101520s)",
    "load": "0.12 0.08 0.05",
    "kernel": "Linux version 3.3.8 (mips Atheros AR9344)",
    "memory": "Total: 128 MB, Free: 48 MB (Avail: 72 MB)",
    "network": "=== Network Interfaces (/proc/net/dev) ===\n..."
  }
  ```

### 1.4 Disk Space
#### `GET /api/disk-space`
Returns partition storage statistics for `/data`, `/cache`, `/tmp`, and root `/`.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "partitions": [
      {
        "mount": "/data",
        "total_kb": 5120,
        "used_kb": 3748,
        "free_kb": 1372,
        "percent_used": 73
      }
    ]
  }
  ```

### 1.5 Network Status
#### `GET /api/network-status`
Returns active network interface, Wi-Fi (`ath0`), USB Ethernet (`eth0`/`usb0`), carrier, IP, netmask, MAC, and gateway.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "usb_host_mode": true,
    "active_interface": "eth0",
    "connection_type": "Dual (Ethernet Active + Wi-Fi Standby)",
    "mode": "ethernet",
    "ip": "192.168.1.100",
    "netmask": "255.255.255.0",
    "mac": "02:00:00:00:00:01",
    "gateway": "192.168.1.1",
    "eth_present": true,
    "eth_carrier": true,
    "eth_interface": "eth0",
    "eth_ip": "192.168.1.100",
    "eth_netmask": "255.255.255.0",
    "eth_mac": "02:00:00:00:00:01",
    "wifi_connected": true,
    "wifi_ssid": "ExampleSSID",
    "wifi_ip": "192.168.1.101",
    "wifi_netmask": "255.255.255.0",
    "wifi_mac": "02:00:00:00:00:02"
  }
  ```

### 1.6 Background Services
#### `GET /api/services`
Queries status of optional background daemons (`hub_rf` and `hub_btstack`).
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "rf_enabled": true,
    "rf_running": true,
    "bt_enabled": true,
    "bt_running": true
  }
  ```

#### `POST /api/services/rf-toggle` or `POST /api/rf-toggle`
Enables or disables the RF CC2544 daemon `hub_rf`.
- **Request Body**: `{"enable": true}`
- **Response `200 OK`**: `{"ok": true, "rf_enabled": true}`

#### `POST /api/services/bt-toggle` or `POST /api/bt-toggle`
Enables or disables the Bluetooth stack `hub_btstack`.
- **Request Body**: `{"enable": true}`
- **Response `200 OK`**: `{"ok": true, "bt_running": true}`

### 1.7 Hub Logs
#### `GET /api/logs?source={daemon|btstack|rf|system|network}`
Returns tail (up to 12KB) of requested daemon or system log.
- **Parameters**: `source` (default: `daemon`).
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "source": "daemon",
    "log": "[INFO] Harmony hub daemon listening on port 8089\n..."
  }
  ```

### 1.8 Hub Assets & Icons
#### `GET /api/icons`
Returns list of available device and activity PNG icon filenames.
- **Response `200 OK`**:
  ```json
  [
    "tv.png",
    "receiver.png",
    "gamepad.png",
    "apple_tv.png"
  ]
  ```

### 1.9 System Settings
#### `GET /api/settings`
Returns global hub settings from `/data/harmony/config/settings.json`.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "hub_name": "Living Room Hub",
    "dry_run": false,
    "ntp_server": "pool.ntp.org",
    "timezone": "Europe/Amsterdam",
    "led_mode": "always_on"
  }
  ```

#### `POST /api/settings`
Updates global hub settings.
- **Request Body**:
  ```json
  {
    "hub_name": "Living Room Hub",
    "dry_run": false,
    "ntp_server": "pool.ntp.org",
    "timezone": "Europe/Amsterdam",
    "led_mode": "always_on"
  }
  ```
- **Response `200 OK`**: `{"ok": true}`

#### `POST /api/bt/reconnect` or `POST /bt/reconnect`
Triggers on-demand Bluetooth host reconnection supervision.
- **Request Body (optional)**:
  ```json
  {
    "target": "XX:XX:XX:XX:XX:XX",
    "duration": 30,
    "delay": 2
  }
  ```
- **Response `200 OK`**: `{"ok": true, "message": "Bluetooth reconnect triggered"}`

### 1.10 System Actions
#### `POST /api/system-action` or `POST /system`
Executes system maintenance operations.
- **Reboot**:
  ```json
  {"action": "reboot"}
  ```
- **Factory Reset**:
  ```json
  {"action": "factory_reset", "confirm": "RESET"}
  ```
- **MQTT Direct Publish**:
  ```json
  {
    "action": "mqtt_publish",
    "topic": "harmony/hub/custom",
    "payload": "{\"test\":1}",
    "retain": false
  }
  ```

---

## 2. Firmware & OTA Updates

### 2.1 Update Status
#### `GET /api/update-status`
Returns running version, commit hash, build date, architecture, and whether an update is pending.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "current_version": "1.0.4",
    "version": "1.0.4",
    "local_version": "v1.0.4 (a1b2c3d)",
    "remote_version": "v1.0.4",
    "commit": "a1b2c3d",
    "build_time": "Oct 21 2026 13:00:00",
    "fw_version": "4.15.600",
    "firmware": "4.15.600",
    "rf_version": "0x20 CC2544",
    "bt_version": "BTstack v1.5 (UART)",
    "arch": "mips (AR9344)",
    "update_available": false
  }
  ```

### 2.2 Check for Updates
#### `GET /api/update-check` or `POST /api/update-check`
Checks for newer release bundles.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "current_version": "1.0.4",
    "message": "Hub binaries and WebUI are up to date (v1.0.4).",
    "has_update": false
  }
  ```

### 2.3 Apply Update
#### `POST /api/update-apply`
Executes `/data/harmony/update.sh` in the background and restarts services.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "status": "applying",
    "message": "Software update triggered. Hub services restarting in background."
  }
  ```

---

## 3. Inventory & Device Management

### 3.1 Get Inventory
#### `GET /api/inventory`
Returns all registered devices and basic commands from `/data/harmony/config/devices.json`.
- **Response `200 OK`**:
  ```json
  {
    "devices": [
      {
        "id": "70321456",
        "name": "LG OLED TV",
        "type": "Television",
        "manufacturer": "LG",
        "model": "OLED65C2",
        "controlPort": 0,
        "mqttTopic": "home/livingroom/tv",
        "mqttEnabled": true,
        "mqttPulse": 100,
        "commands": [
          {"name": "PowerToggle", "raw": "0000 006d ...", "port": 7, "repeats": 3}
        ]
      }
    ]
  }
  ```

### 3.2 Save Device
#### `POST /api/device-save` or `POST /ir/device`
Creates or updates a device definition.
- **Request Body**:
  ```json
  {
    "deviceId": "70321456",
    "name": "LG OLED TV",
    "type": "Television",
    "manufacturer": "LG",
    "model": "OLED65C2",
    "controlPort": 0,
    "mqttTopic": "home/livingroom/tv",
    "mqttEnabled": true,
    "mqttPulse": 100
  }
  ```
- **Response `200 OK`**: `{"ok": true}`

### 3.3 Delete Device
#### `POST /api/device-delete` or `POST /ir/delete-device`
Deletes a device and scrubs its references from activities.
- **Request Body**: `{"deviceId": "70321456"}`
- **Response `200 OK`**: `{"ok": true}`

### 3.4 Get Device Commands
#### `GET /api/device-commands?deviceId=70321456`
Returns all IR/BT commands for the specified device.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "commands": [
      {
        "name": "PowerToggle",
        "raw": "0000 006d 0022 0002 0155 00aa ...",
        "port": 7,
        "repeats": 3
      }
    ]
  }
  ```

### 3.5 Save Command
#### `POST /api/command-save` or `POST /ir/command` or `POST /bt/command`
Adds or updates a single command for a device.
- **Request Body**:
  ```json
  {
    "deviceId": "70321456",
    "name": "VolumeUp",
    "raw": "0000 006d 0022 0002 0155 00aa ...",
    "port": 7,
    "repeats": 2
  }
  ```
- **Response `200 OK`**: `{"ok": true}`

### 3.6 Batch Import Commands
#### `POST /api/commands-batch` or `POST /ir/commands-batch`
Imports multiple commands into a device in one atomic write.
- **Request Body**:
  ```json
  {
    "deviceId": "70321456",
    "commands": [
      {"name": "PowerOn", "raw": "0000 ...", "port": 7, "repeats": 3},
      {"name": "PowerOff", "raw": "0000 ...", "port": 7, "repeats": 3}
    ]
  }
  ```
- **Response `200 OK`**: `{"ok": true, "count": 2}`

### 3.7 Delete Command
#### `POST /api/command-delete` or `POST /ir/delete-command` or `POST /bt/delete-command`
Deletes a command by name from a device.
- **Request Body**:
  ```json
  {
    "deviceId": "70321456",
    "command": "VolumeUp"
  }
  ```
- **Response `200 OK`**: `{"ok": true}`

### 3.8 Device MQTT Test
#### `POST /api/device-mqtt-test`
Tests device MQTT command routing without running full sequences.
- **Request Body**:
  ```json
  {
    "deviceId": "70321456",
    "command": "PowerToggle",
    "topic": "home/livingroom/tv",
    "pulseMs": 500
  }
  ```
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "pulseMs": 500,
    "topic": "home/livingroom/tv/command",
    "message": "MQTT test publish sent"
  }
  ```

---

## 4. Activities & Orchestration

### 4.1 List Activities
#### `GET /api/activities`
Returns all configured activities, active activity ID, and state enum.
- **Response `200 OK`**:
  ```json
  {
    "activities": [
      {
        "id": "123456",
        "name": "Watch Apple TV",
        "type": "Custom",
        "icon": "apple_tv.png",
        "order": 1,
        "startSequence": [],
        "stopSequence": []
      }
    ],
    "currentId": "-1",
    "state": 0
  }
  ```

### 4.2 Current Activity State
#### `GET /api/activity` or `GET /api/current_activity`
Returns active activity and orchestration status.
- **Response `200 OK`**:
  ```json
  {
    "activity": "-1",
    "state": 0,
    "busy": false
  }
  ```
  - `state`: `0` = Idle / Stopped, `1` = Starting, `2` = Running, `3` = Stopping.

### 4.3 Start Activity
#### `POST /api/activity` or `POST /api/activity-start`
Switches to or starts the target activity sequence.
- **Request Body**:
  ```json
  {
    "activity": "123456",
    "dry_run": false
  }
  ```
- **Response `200 OK`**:
  ```json
  {
    "success": true,
    "state": "starting"
  }
  ```
- **Error `409 Conflict`**: State machine interlock busy executing another sequence.

### 4.4 Stop Activity / Power Off
#### `POST /api/poweroff` or `POST /api/activity-stop`
Executes exit sequences for running devices and transitions to idle (`-1`).
- **Request Body**:
  ```json
  {
    "dry_run": false
  }
  ```
- **Response `200 OK`**:
  ```json
  {
    "success": true,
    "state": "stopping"
  }
  ```

### 4.5 Reset Interlock / Cancel Activity
#### `POST /api/activity-cancel` or `POST /api/interlock-reset` or `POST /api/activity-reset`
Forcibly clears stuck orchestrator step queues and resets state machine to idle.
- **Response `200 OK`**: `{"ok": true}`

### 4.6 Save Activity
#### `POST /api/activity-save`
Saves activity configuration with start and stop sequence steps.
- **Request Body**:
  ```json
  {
    "id": "123456",
    "name": "Watch TV",
    "type": "Custom",
    "icon": "tv.png",
    "order": 1,
    "startSequence": [
      {"type": "Command", "deviceId": "70321456", "command": "PowerOn"},
      {"type": "Delay", "delay": 1500}
    ],
    "stopSequence": [
      {"type": "Command", "deviceId": "70321456", "command": "PowerOff"}
    ]
  }
  ```
- **Response `200 OK`**: `{"ok": true}`

### 4.7 Delete Activity
#### `POST /api/activity-delete`
Deletes an activity by ID.
- **Request Body**: `{"id": "123456"}`
- **Response `200 OK`**: `{"ok": true}`

### 4.8 Test Sequence Execution
#### `POST /api/activity-test-sequence`
Executes a test sequence immediately without modifying activity state.
- **Request Body**:
  ```json
  {
    "steps": [
      {"type": "Command", "deviceId": "70321456", "command": "PowerToggle"},
      {"type": "Delay", "delay": 500}
    ]
  }
  ```
- **Response `200 OK`**: `{"ok": true, "executed": 2}`

### 4.9 Activity Progress Webhook
#### `POST /api/activity-progress`
Internal step progress notification. Broadcasts state payload to WebSockets and MQTT.

### 4.10 Reload / Sync Config
#### `POST /api/sync` or `POST /api/reload`
Forces daemon to reload JSON configs from flash.
- **Response `200 OK`**: `{"ok": true}`

---

## 5. Hardware & IR Control

### 5.1 Send IR Command
#### `POST /api/ir-send` or `POST /ir/send`
Transmits an IR command either by device ID and command name, or raw Pronto hex.
- **Request by Device Command**:
  ```json
  {
    "deviceId": "70321456",
    "command": "PowerToggle"
  }
  ```
- **Request by Raw Pronto**:
  ```json
  {
    "pronto": "0000 006d 0022 0002 0155 00aa ...",
    "port": 7,
    "repeats": 3
  }
  ```
  - `port`: Port bitmask (`1` = Front Hub blaster, `2` = Jack 1, `4` = Jack 2, `7` = All emitters).
  - `repeats`: Repeat count (default: 1).
- **Response `200 OK`**: `{"ok": true}`

### 5.2 Raw Pronto Blast
#### `POST /api/pronto-blast`
Dedicated blaster endpoint for raw Pronto hex code streams.
- **Request Body**:
  ```json
  {
    "pronto": "0000 006d ...",
    "ports": 7,
    "repeats": 3
  }
  ```
- **Response `200 OK`**: `{"success": true}`

### 5.3 Button Press & MQTT Dispatch
#### `POST /api/button-press` or `POST /api/mqtt-publish`
Dispatches a button press pulse to local MQTT broker and hardware handlers.
- **Request Body**:
  ```json
  {
    "deviceId": "70321456",
    "command": "VolumeUp",
    "pulseMs": 100
  }
  ```
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "pulseMs": 100,
    "topic": "harmony/hub/button/70321456/VolumeUp",
    "reply": "ok"
  }
  ```

---

## 6. Bluetooth Stack & HID

### 6.1 Bluetooth Status
#### `GET /api/bt-status`
Returns connection status, BTstack daemon state, paired hosts, and active target.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "running": true,
    "connected": true,
    "target": "AA:BB:CC:DD:EE:FF",
    "devices": [
      {"bdaddr": "AA:BB:CC:DD:EE:FF", "name": "Apple TV"}
    ]
  }
  ```

### 6.2 Host Pairing Mode
#### `POST /api/bt-pairing`
Puts hub HID keyboard emulator into pairing/discoverable mode for target device.
- **Request Body**:
  ```json
  {
    "enable": true,
    "name": "Harmony Keyboard",
    "deviceId": "70321456"
  }
  ```
- **Response `200 OK`**: `{"ok": true, "pairing": true}`

### 6.3 Connect Host
#### `POST /api/bt-connect`
Connects to a paired Bluetooth host MAC address.
- **Request Body**: `{"bdaddr": "AA:BB:CC:DD:EE:FF"}`
- **Response `200 OK`**: `{"ok": true}`

### 6.4 Disconnect Host
#### `POST /api/bt-disconnect`
Disconnects active Bluetooth host link.
- **Request Body**: `{"bdaddr": "AA:BB:CC:DD:EE:FF"}`
- **Response `200 OK`**: `{"ok": true}`

### 6.5 Link Device to BT Address
#### `POST /api/bt-link-device`
Associates a logical device ID with a paired Bluetooth MAC address.
- **Request Body**:
  ```json
  {
    "deviceId": "70321456",
    "bdaddr": "AA:BB:CC:DD:EE:FF"
  }
  ```
- **Response `200 OK`**: `{"ok": true}`

### 6.6 Bluetooth Remote (B25) Status & Pairing
#### `GET /api/remote-status` or `GET /api/remote-pair-status`
Returns status of connected Bluetooth BLE remote (e.g. Homatics B25).

> [!NOTE]
> **Sleeping BLE Remotes & First Button Press**:
> When a BLE remote (such as the Homatics B25) enters low-power sleep mode, the first button press wakes up the remote's radio rather than transmitting a keycode. On the Homatics B25, this initial wake press drops the command and flashes the red indicator light. The second button press will register and execute normally once the BLE remote finishes reconnecting to the Harmony Hub.

#### `POST /api/remote-scan`
Initiates Bluetooth scan for nearby BLE remotes.

#### `POST /api/remote-pair`
Pairs with target BLE remote MAC address.
- **Request Body**: `{"bdaddr": "11:22:33:44:55:66"}`

### 6.7 Remote Key Mappings
#### `GET /api/remote-mapping`
Returns physical key bindings for BLE remote.

#### `POST /api/remote-mapping-save`
Saves BLE remote button mapping JSON.

---

## 7. Elite Remote & RF Subsystem (`/api/rf/*`)

Requests starting with `/api/rf/*` are proxied to the native RF daemon `hub_rf` running on port 8092.

### 7.1 RF Subsystem Status
#### `GET /api/rf` or `GET /api/rf/status`
Returns CC2544 radio state, paired remotes, and channel hopping status.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "running": true,
    "state": "running",
    "connected": true,
    "pairing_active": false,
    "paired": true,
    "rf_address": "00000000",
    "hub_rf_address": "00000000",
    "slots": [
      {"slot": 1, "active": true, "address": "00000000", "rf_address": "00000000"},
      {"slot": 2, "active": false, "address": "None", "rf_address": "None"}
    ]
  }
  ```

### 7.2 RF Pairing
#### `GET /api/rf/pair?slot={1|2}` or `POST /api/rf/pair`
Initiates RF pairing beacon on slot 1 or slot 2.
- **Response `200 OK`**: `{"status": "pairing_started"}`

#### `GET /api/rf/stop-pair`
Stops active pairing mode.
- **Response `200 OK`**: `{"status": "pairing_stopped"}`

#### `GET /api/rf/unpair?slot={1|2}` or `POST /api/rf/unpair`
Unpairs target remote slot.
- **Response `200 OK`**: `{"status": "unpaired"}`

### 7.3 Raw RF Frame Transmit
#### `GET /api/rf/send?hex={hex_stream}`
Transmits raw RF bytes through CC2544 SPI driver.
- **Response `200 OK`**: `{"status": "sent"}`

### 7.4 RF Remote Configuration & OTA Push
#### `GET /api/rf/remote-config?slot={1|2}`
Returns brightness, display timeout, tilt gyro, touch sensitivity, night mode, and nav button configuration for Elite remote.

#### `POST /api/rf/remote-config?slot={1|2}`
Saves remote preferences and transmits push packet to Elite over RF.
- **Request Body**:
  ```json
  {
    "brightness": 3,
    "timeout": 120,
    "touchEnabled": true,
    "capButtonsEnabled": true,
    "gyroEnabled": true,
    "gyroRate": 25,
    "btnBacklight": 1,
    "nightStart": 22,
    "nightEnd": 7,
    "navToggleKey": 139,
    "navToggleMode": 1,
    "confirmActivity": false,
    "touchSensitivity": 2,
    "remoteMode": 0,
    "usbEth": false
  }
  ```
- **Response `200 OK`**: `{"status": "saved_and_pushed", "ok": true}`

#### `GET /api/rf/push-config` or `POST /api/rf/push-config`
Increments config version and broadcasts instant over-the-air sync packet to remote.
- **Response `200 OK`**: `{"status": "push_started"}`

#### `GET /api/rf/sync`
Forces remote activity display synchronization with hub current state.
- **Response `200 OK`**: `{"status": "synced"}`

#### `GET /api/rf/sleep?slot={0|1|2}` or `POST /api/rf/sleep?slot={0|1|2}`
Puts paired Elite remotes to sleep (immediate Suspend-to-RAM on battery).
- **Query Parameter**: `slot` (optional: `1` for remote 1, `2` for remote 2, `0` or omitted for all active remotes).
- **Response `200 OK`**: `{"status": "sleep_dispatched", "slot": 1, "sent": 1, "ok": true}`
- **MQTT equivalent**: Publish to `harmony/hub/remote/1/sleep` or `harmony/hub/remote/sleep`.
- **Home Assistant**: Auto-discovered as `button.<hub>_remote_1_sleep`.

### 7.5 Direct Buttons & Firmware OTA
#### `GET /api/rf/remote-direct-buttons?slot={1|2}`
Returns touch screen direct command button grid.

#### `POST /api/rf/remote-direct-buttons?slot={1|2}`
Saves direct touch buttons configuration.

#### `GET /api/rf/ota-status`
Returns remote firmware chunk update progress.
- **Response `200 OK`**:
  ```json
  {
    "in_progress": false,
    "slot": 1,
    "status": 0,
    "sent_bytes": 0,
    "total_bytes": 0,
    "sent_chunks": 0,
    "total_chunks": 0,
    "error": ""
  }
  ```

#### `POST /api/rf/ota-start?slot={1|2}`
Starts background remote firmware flash update.

### 7.6 Elite Remote Key Mappings
#### `GET /api/elite-mapping`
Returns physical button and screen mappings for the Harmony Elite remote.

#### `POST /api/elite-mapping-save`
Saves Harmony Elite button assignments to `/data/harmony/elite_remote_map.json`.

#### `POST /api/ui-remote-layout`
Saves virtual remote keypad layout for WebUI and Android App.

---

## 8. Network & Broker Configuration

### 8.1 MQTT Settings
#### `POST /api/mqtt-save` or `POST /mqtt`
Configures hub MQTT broker credentials, port, and base topic.
- **Request Body**:
  ```json
  {
    "enabled": true,
    "host": "192.168.1.50",
    "port": 1883,
    "username": "homeassistant",
    "password": "password",
    "baseTopic": "harmony/hub",
    "clientId": "harmony_hub"
  }
  ```
- **Response `200 OK`**: `{"ok": true}`

#### `GET /api/mqtt-status`
Queries current MQTT connection state and parameters.

### 8.2 Wi-Fi Scanning & Configuration
#### `GET /api/wifi-scan`
Scans for nearby 2.4GHz Wi-Fi networks via `wpa_cli -i ath0 scan` and returns deduplicated APs sorted by signal strength.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "networks": [
      {
        "ssid": "MyHomeNetwork",
        "bssid": "7c:10:c9:b6:ae:58",
        "signal": -45,
        "channel": 1,
        "frequency": 2412,
        "security": "WPA2",
        "flags": "[WPA2-PSK-CCMP][ESS]"
      }
    ]
  }
  ```

#### `POST /api/wifi-test`
Tests connection to an AP in memory without modifying `/etc/wpa_supplicant.conf`. Protected by an automatic 20-second watchdog rollback to ensure the Hub never gets locked out if authentication fails.
- **Request Body**:
  ```json
  {
    "ssid": "MyWiFiNetwork",
    "password": "WiFiPassword",
    "open": 0,
    "hidden": 0,
    "keep_password": 0
  }
  ```
- **Response `200 OK`**: `{"ok": true, "message": "Wi-Fi test connection successful"}`
- **Response `400 Bad Request`**: `{"ok": false, "error": "Authentication failed: incorrect Wi-Fi password"}`

#### `POST /api/wifi-save` or `POST /wifi`
Saves Wi-Fi credentials to `/etc/wpa_supplicant.conf` and `/etc/wpa_supplicant.ath0.conf`.
When `apply` is `reconfigure`, `apply`, or `reboot`, the Hub automatically runs an in-memory association test first. If the test connection fails, the switch is rejected and existing settings are preserved.
- **Request Body**:
  ```json
  {
    "ssid": "MyWiFiNetwork",
    "password": "WiFiPassword",
    "apply": "reconfigure",
    "keep_password": 1,
    "open": 0,
    "hidden": 0,
    "force": 0
  }
  ```
- **Response `200 OK`**: `{"ok": true, "message": "Wi-Fi settings verified and applied"}`
- **Response `400 Bad Request`**: `{"ok": false, "error": "Wi-Fi test connection failed: Connection timed out. Settings not changed."}`

### 8.3 USB Ethernet Configuration
#### `POST /api/ethernet-save` or `POST /ethernet`
Configures USB Ethernet interface mode (DHCP or static IP).
- **Request Body**:
  ```json
  {
    "eth_mode": "dhcp",
    "eth_ip": "192.168.1.100",
    "eth_netmask": "255.255.255.0",
    "eth_gateway": "192.168.1.1",
    "eth_dns": "192.168.1.1",
    "apply": "apply"
  }
  ```
- **Response `200 OK`**: `{"ok": true, "message": "Ethernet configuration saved"}`

---

## 9. Backup, Import & Export

### 9.1 Export Configuration
#### `GET /export/{target}`
Exports complete hub configuration bundle or individual configuration elements.
- Targets:
  - `bundle` (Full JSON snapshot of devices, activities, settings, MQTT, network, mappings, keys)
  - `devices`
  - `activities`
  - `settings`
  - `mqtt`
  - `network`
  - `wifi`
  - `bluetooth`
  - `bt_remote_map`
  - `elite_remote_map`
  - `ui_remote_layout`
  - `auth`
  - `hub_id`

### 9.2 Import Configuration
#### `POST /import`
Imports and atomically applies configuration files or full bundle.
- **Request Body**:
  ```json
  {
    "target": "bundle",
    "payload": {
      "devices": {...},
      "activities": {...},
      "settings": {...}
    }
  }
  ```
- **Response `200 OK`**: `{"ok": true}`

---

## 10. WebSocket Protocol (`/ws` or Port 8089)

Connect via WebSocket to `ws://<hub_ip>:8089/ws` or `ws://<hub_ip>/ws`.

### Client-to-Server Messages

#### 10.1 Ping / Heartbeat
```json
{
  "action": "ping",
  "msgId": 1
}
```
**Response**:
```json
{
  "type": "pong",
  "msgId": 1,
  "activity": "-1",
  "state": 0
}
```

#### 10.2 Query State
```json
{
  "action": "get_state",
  "msgId": 2
}
```
**Response**:
```json
{
  "type": "activity_state",
  "msgId": 2,
  "activity": "123456",
  "state": 2
}
```

#### 10.3 Send Device Command
```json
{
  "action": "send_command",
  "deviceId": "70321456",
  "command": "Mute",
  "msgId": 3
}
```
**Response**:
```json
{
  "type": "ack",
  "action": "send_command",
  "ok": true,
  "msgId": 3
}
```

#### 10.4 Start Activity
```json
{
  "action": "start_activity",
  "id": "123456",
  "msgId": 4
}
```
**Response**:
```json
{
  "type": "ack",
  "action": "start_activity",
  "ok": true,
  "msgId": 4
}
```

#### 10.5 Stop Activity / Power Off
```json
{
  "action": "stop_activity",
  "msgId": 5
}
```
**Response**:
```json
{
  "type": "ack",
  "action": "stop_activity",
  "ok": true,
  "msgId": 5
}
```

#### 10.6 Cancel Activity / Interlock Reset
```json
{
  "action": "cancel_activity",
  "msgId": 6
}
```
**Response**:
```json
{
  "type": "ack",
  "action": "cancel_activity",
  "ok": true,
  "msgId": 6
}
```

#### 10.7 Bluetooth HID Key Simulation
```json
{
  "action": "bt_key",
  "key": "KEY_ENTER",
  "keyAction": "tap",
  "target": "AA:BB:CC:DD:EE:FF",
  "msgId": 7
}
```
- `keyAction`: `"tap"`, `"down"`, `"up"`, or `"release_all"`.
- `key`: Key name or `"release_all"`.
- `target`: Optional Bluetooth host MAC address.
**Response**:
```json
{
  "type": "ack",
  "action": "bt_key"
}
```

---

## 11. Authentication & Security Architecture

Harmony Hub supports a multi-tier authentication model:

### 11.1 HTTP Digest Authentication (RFC 7616 / RFC 2617)
Protects credentials on the wire without requiring SSL/TLS overhead on embedded hardware. Nonces are HMAC-SHA256 signed with dynamic expiry.

```bash
# Digest authentication with curl
curl --digest -u admin:secret123 http://10.234.234.98:8089/api/status
```

### 11.2 OAuth2 Bearer Session Tokens (RFC 6749 / RFC 6750)
Recommended for SPAs, WebUI, and Mobile Apps.

```bash
# 1. Acquire Bearer token
curl -X POST http://10.234.234.98:8089/api/auth/token \
  -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"secret123"}'

# Response:
# {"ok":true,"access_token":"hht_018f...","token_type":"Bearer","expires_in":86400,"username":"admin","role":"admin"}

# 2. Use Bearer token
curl -H "Authorization: Bearer hht_018f..." http://10.234.234.98:8089/api/status

# 3. Invalidate token / Logout
curl -X POST -H "Authorization: Bearer hht_018f..." http://10.234.234.98:8089/api/auth/logout
```

### 11.3 Personal Access Tokens (PAT) for Home Automation
Designed for Home Assistant, Homebridge, Node-RED, or automation scripts. Never requires sharing or hardcoding admin passwords.

```bash
# 1. Generate new Personal Access Token (Admin only)
curl -X POST -H "Authorization: Bearer hht_..." \
  -H "Content-Type: application/json" \
  -d '{"name":"Home Assistant","role":"admin"}' \
  http://10.234.234.98:8089/api/auth/tokens

# Response:
# {"ok":true,"id":"tok_6a1d8f","name":"Home Assistant","role":"admin","token":"pat_3f8a9b..."}

# 2. Authenticate Home Assistant REST sensor / command
curl -H "Authorization: Bearer pat_3f8a9b..." http://10.234.234.98:8089/api/current_activity

# 3. List active tokens
curl -H "Authorization: Bearer hht_..." http://10.234.234.98:8089/api/auth/tokens

# 4. Revoke a token
curl -X DELETE -H "Authorization: Bearer hht_..." "http://10.234.234.98:8089/api/auth/tokens?id=tok_6a1d8f"
```

### 11.4 WebSocket Token Authentication
Connect with token in query string:
`ws://10.234.234.98:8089/?token=pat_3f8a9b...`

