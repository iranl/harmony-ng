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
Returns high-level hub status, firmware, identity, active activity, memory, uptime, and MQTT connection status.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "hub_name": "Living Room Hub",
    "firmware": "4.15.600",
    "hub_id": "20140101120000-000001",
    "version": "2.2.0",
    "commit": "abc1234",
    "api_version": 2,
    "capabilities": ["events", "snapshot", "ws_auth", "ws_keepalive", "ir_raw", "rf_wake"],
    "legacy_enabled": false,
    "mdns_enabled": false,
    "activity": "123456",
    "state": 2,
    "busy": false,
    "ip": "192.168.1.100",
    "total_kb": 5120,
    "free_kb": 1372,
    "uptime_sec": 34821,
    "mqtt": {
      "connected": true,
      "broker": "192.168.1.50",
      "port": 1883,
      "baseTopic": "harmony/hub",
      "v2": true
    }
  }
  ```
  - `firmware`: the Logitech base firmware. `version` and `commit`: this firmware, as in `/api/system-info`.
  - `hub_id`: the factory unit id from the `mfg` partition (unique per unit, survives reflashes); the
    identifier for device registries and unique ids. Falls back to the Wi-Fi MAC (12 lowercase hex
    characters, logged as a warning) only when the partition has no unit id.
  - `api_version`: `2` from milestone 1 of the integration work on; it changes only on a breaking
    change to an existing endpoint or message. Additions are announced through `capabilities`.
  - `capabilities`: flags of the features compiled into this firmware, independent of settings:
    `events` (event bus, pushed WebSocket messages 10.8 to 10.13), `snapshot` (10.14), `ws_auth`
    (11.4), `ws_keepalive` (10.15), `ir_raw` (5.4), `rf_wake` (`POST /api/rf/wake`).
  - `legacy_enabled`, `mdns_enabled`: mirror the settings of the Logitech protocol shim and the mDNS
    responder; `false` while those features do not exist.

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
    "led_mode": "green",
    "mdns_enabled": true,
    "legacy_enabled": false
  }
  ```
  - `mdns_enabled` (default `true`): the mDNS responder (1.11). Changing it starts or stops the
    responder at once.
  - `legacy_enabled` (default `false`): the Logitech protocol shim on port 8088 and its SSDP
    responder (1.12). Changing it opens or closes the port at once; turning it off also closes
    every client connected to it.

#### `POST /api/settings`
Updates global hub settings.
- **Request Body**:
  ```json
  {
    "hub_name": "Living Room Hub",
    "dry_run": false,
    "ntp_server": "pool.ntp.org",
    "timezone": "Europe/Amsterdam",
    "led_mode": "green",
    "mdns_enabled": true,
    "legacy_enabled": false
  }
  ```
  *Note: `led_mode` sets the Hub's front light while it runs normally: `"green"` (default), `"amber"`, `"red"` or `"off"`. The light flashes green fast while it pairs a remote and alternates red and green slowly during an RF firmware update, then returns to this setting. A pairing that times out or a failed update flashes red slowly for 5 s first. The older values `"always_on"` (green) and `"always_off"` (off) are still accepted, and `"flashing_fast"` and `"flashing_slow"` now mean green.*
  *Note: `timezone` accepts standard IANA timezone identifiers (e.g. `"Europe/Amsterdam"`, `"America/New_York"`, `"Asia/Tokyo"`) or explicit POSIX TZ strings (e.g. `"CET-1CEST,M3.5.0,M10.5.0/3"`).*
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

### 1.11 Discovery (mDNS / DNS-SD)
The Hub advertises itself as `_harmony-ng._tcp.local.` on port 8089 (capability flag `mdns`,
setting `mdns_enabled`), so Home Assistant and `avahi-browse -r _harmony-ng._tcp` find it without
an address. The responder runs inside `hub_daemon` on UDP 5353 (multicast group 224.0.0.251,
unicast replies when the query asks for them); it probes for its names before announcing, announces
twice at start, re-announces on an IP change (polled every 10 s) and on a change of any TXT value,
and sends a goodbye (TTL 0) on a clean stop.
- Instance name: `hub_name` with characters outside `[A-Za-z0-9 _-]` replaced by `-`
  (`Harmony Hub` when empty); on a name conflict the hub id is appended. Consumers identify a Hub
  by the TXT `id`, never by the instance name or hostname.
- Hostname: `harmony-ng-<hub_id>.local.` with an A record for the active interface (no AAAA).
- TTLs: 120 s for A and SRV, 4500 s for PTR and TXT. Known-answer suppression for the PTR.
- TXT record, keys in this order:

  | Key | Value |
  | --- | --- |
  | `id` | `hub_id` (factory unit id) |
  | `ver` | firmware version (`HARMONY_VERSION`) |
  | `api` | `api_version` (2) |
  | `auth` | `1` when authentication is enabled |
  | `mqtt` | `1` when the MQTT client is configured |
  | `topic` | MQTT base topic, only when `mqtt=1` |
  | `legacy` | `1` when the Logitech protocol shim (1.12) is enabled |
  | `name` | raw `hub_name`, for display |

### 1.12 Logitech protocol compatibility (port 8088, SSDP)
With `legacy_enabled` on (capability flag `legacy`), `hub_daemon` speaks the stock Logitech Hub
protocol on port 8088, as Home Assistant's core `harmony` integration (aioharmony 1.0.8) and other
stock clients expect, and answers SSDP searches so the core integration discovers the Hub. Off by
default. Port 8088 is never authenticated: the stock protocol has no authentication, so enable it
only on a trusted network. The daemon's own API on 8089 (sections 1 to 11) is unchanged; both
integrations can be used at the same time.

- `POST /` on 8088 (body `{"id ":1,"cmd":"setup.account?getProvisionInfo","params":{}}`, the
  trailing space in `id ` is what the client sends) answers the provisioning object:
  `{"id":1,"code":200,"msg":"OK","data":{"activeRemoteId":"<id>","hubSwVersion":"<version>",
  "discoveryServer":"https://svcs.myharmony.com/Discovery/Discovery.svc","hubName":"<hub_name>",
  "accountId":"0","email":""}}`. `activeRemoteId` is the client's unique id for the Hub: the stock
  `remoteId` from `/data/luaworks/provision/preferences` when that file survived the harmony-ng
  install (a user migrating from stock keeps the core integration's entry), else a stable number
  derived from `hub_id`. A bare TCP connect without data (the client's probe) is accepted quietly.
- `GET /description` on 8088: the UPnP device description (`deviceType`
  `urn:myharmony-com:device:harmony:1`, `manufacturer` Logitech, `friendlyName` = `hub_name`,
  `modelName` Harmony Hub, `UDN` `uuid:<v5 of hub_id>`, `serialNumber` = `hub_id`). Any other
  path is `404`.
- WebSocket on `ws://<hub>:8088/?domain=svcs.myharmony.com&hubId=<activeRemoteId>`. Requests are
  `{"hubId":"<activeRemoteId>","timeout":5,"hbus":{"cmd":"<mime>?<command>","id":"<uuid>",
  "params":{...}}}`, replies `{"cmd":"<cmd>","id":"<id>","code":200,"msg":"OK","data":{...}}`
  (`code` is a number; unknown commands get `404`). A digest notify is sent right after the
  upgrade. Commands:

  | `cmd` | Reply `data` |
  | --- | --- |
  | `vnd.logitech.harmony/vnd.logitech.harmony.engine?config` | the stock configuration object built from `devices.json` and `activities.json` on every request: `activity[]` (with the `-1` `PowerOff` entry last), `device[]` with `controlGroup[].function[].action` as a JSON-encoded string, `global.timeStampHash` `<inventory_version>;<activeRemoteId>`. Ids are the daemon's ids when numeric, else a stable CRC32-derived number (mapped back on `runactivity` and `holdAction`) |
  | `...?getCurrentActivity` | `{"result":"<activity id>"}`, `"-1"` when idle |
  | `harmony.activityengine?runactivity` (`activityId`, `"-1"` = power off) | `{}` with `code` 200 at once (`500` "Busy" or "Unknown activity"), then the start sequence below |
  | `...?holdAction` (`status` `press` or `release`, `action` with `::` for `:`) | no reply on success; `code` 500 reply on error. Same press, hold and release path as `send_command` on 8089 |
  | `harmony.engine?changeChannel` (`channel` digits) | `{}`; the digits go to the first device of the running activity with digit commands, 200 ms apart; `500` without one |
  | `setup.sync?None` | `{}` |
  | `vnd.logitech.connect/vnd.logitech.statedigest?get` | the digest: `activityId`, `activityStatus` (0 idle, 1 starting, 2 running, 3 stopping), `configVersion` (changes when the inventory changes), `syncStatus` 0, `hubSwVersion`, `runningActivityList`, and the other stock fields |
  | `connect.discoveryinfo?get` | `friendlyName`, `remoteId`, `hubId`, `current_fw_version`, `protocolVersion` |

- Activity start sequence for a `runactivity` with id M: the `code` 200 reply; on the first
  step a `{"type":"connect.stateDigest?notify","data":{...activityStatus 1...}}` to every 8088
  client; per step `{"cmd":"harmony.engine?startActivity","id":M,"code":100,"data":{"done":n,
  "total":m}}` to the requester; on completion `{"cmd":"harmony.engine?startActivity","id":M,
  "code":200,"data":{}}` to the requester, then `{"type":"harmony.engine?startActivityFinished",
  "data":{"activityId":"<id>","errorCode":"200","errorString":"OK"}}` and a final digest notify
  (`activityStatus` 2, or 0 after a power off) to every 8088 client. Switches made elsewhere (the
  remote, the web UI, 8089 clients, MQTT) produce the notifications only, so stock clients follow
  them. A config change produces a digest notify (new `configVersion`) so clients refetch.
- SSDP: `M-SEARCH` for `ssdp:all`, `upnp:rootdevice` or `urn:myharmony-com:device:harmony:1` on
  239.255.255.250:1900 is answered with `LOCATION: http://<ip>:8088/description`, `ST`, `USN:
  uuid:<uuid>::urn:myharmony-com:device:harmony:1`, `CACHE-CONTROL: max-age=1800`; `NOTIFY
  ssdp:alive` on enable and every 900 s, `ssdp:byebye` on disable and at shutdown.

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
Publishes the MQTT pulse of a device once, the way a command to an MQTT-type device does (8.1,
"MQTT-type devices"): `"1"` on the resolved topic, then `"0"` after the pulse.

#### `POST /api/device-mqtt-test`
- **Request Body**: `{"deviceId": "3", "device": "Plug", "command": "PowerToggle", "topic": "{base_topic}/button/{device}/{command}", "pulseMs": 1000}`.
  `topic` and `pulseMs` override the device's `mqttTopic` and `mqttPulseMs`; `command` defaults to
  `PowerToggle`.
- **Response `200 OK`**: `{"ok": true, "topic": "harmony/hub/button/Plug/PowerToggle", "pulseMs": 1000, "message": "MQTT test publish sent"}`.
- **Response `503`**: MQTT is disabled or the broker is not connected.

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
    "busy": false,
    "sending": false
  }
  ```
  - `state`: `0` = Idle / Stopped, `1` = Starting, `2` = Running, `3` = Stopping.
  - `busy`: an activity change holds the interlock. It ends when the activity counts as switched, which can be before its last commands are sent.
  - `sending`: commands of the last activity change or Help, including background steps, are still to be sent.

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

### 4.11 Help: Repair the Running Activity
#### `POST /api/activity/help`
Re-sends the running activity's start sequence, as if starting from Power Off, except power toggles (`PowerToggle`, `Power`), and lists the devices whose power command is a toggle so the caller can ask whether each is on.
- **Request Body** (both fields optional; `activity` defaults to the running activity):
  ```json
  {
    "activity": "1",
    "dry_run": false
  }
  ```
- **Response `200 OK`**:
  ```json
  {
    "success": true,
    "activity": "1",
    "toggleDevices": [
      { "id": "4", "name": "Sony Projector", "command": "PowerToggle" }
    ]
  }
  ```
  With `dry_run`, nothing is sent and `preview` holds the steps that would be sent.
- **Error `409 Conflict`**: `Interlock busy`, or `Activity is not running`.

#### `POST /api/activity/help/answer`
Answers the question for one device of the last Help. `"on": false` sends that device's power toggle; `"on": true` sends nothing. Each device takes one answer.
- **Request Body**: `{"device": "4", "on": false}`
- **Response `200 OK`**: `{"success": true, "sent": true}`
- **Error `404 Not Found`**: the device is not waiting for a Help answer.

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

### 5.2a IR Learning (capture)
#### `POST /api/capture`
Listens on the Hub's IR receiver until a signal arrives or the timeout ends, then returns one frame as a Harmony raw timing string (`F<carrier hex>P<pulse us hex>S<space us hex>...`, the carrier measured, the last space the gap to the next frame). The string can be stored as a command's `raw` (`POST /ir/command`) and sent like any other keycode. The sensor is short-range: hold the original remote about 8 cm from the Hub and hold the button for 1 to 2 seconds (the first frame of a press is often cut, so the decoder keeps the first complete frame after it).
- **Request Body** (optional): `{"timeout": 10}` in seconds, 1 to 30 (default 10). `?timeout=` works too.
- **Response `200 OK`**:
  ```json
  {"ok": true, "raw": "F9470P...S...", "keycode": "F9470P...S...", "mode": "raw", "carrier_hz": 38000, "pulses": 26, "duration_us": 24000}
  ```
  `pulses` is the number of timing intervals in `raw`, marks and spaces together (`P` and `S` entries); `duration_us` is their sum.
- **Errors**: `408` no signal before the timeout, or only a flash that is not a code, `409` another capture is running or an IR send holds the hardware, `503` no IR receiver, or the receiver stopped delivering data (the capture gives up after 2 s without data instead of hanging; a reboot clears a stuck receiver), `500` other failures.

#### `POST /api/ir-test-learned`
Sends a captured code once through the IR ports of a device (its `controlPort`, default all).
- **Request Body**: `{"deviceId": "3", "keycode": "F9470P...S..."}` (`raw` or `code` are accepted for `keycode`).
- **Response `200 OK`**: `{"ok": true, "port": 7}`
- **Errors**: `400` missing code or a code the IR encoder does not understand, `500` transmit failure.

### 5.3 Button Press & MQTT Dispatch
#### `POST /api/button-press` or `POST /api/mqtt-publish`
Reports a physical key. Every call gives a `button` event on the WebSocket (10.13) and on MQTT
(`<base>/remote/<slot>/button`, 8.1) for `action` `press` (the default) and `release`; `hold`
repeats give nothing (they are sent to the device by `hub_rf` itself). `hub_rf` posts physical keys as `{"button": <key name>,
"action", "source": "remote_rf", "slot"}` plus `deviceId` and `command` when the key was mapped to
a device command; other callers may keep posting the key as `command`.
- **Request Body**:
  ```json
  {
    "deviceId": "70321456",
    "command": "VolumeUp",
    "pulseMs": 100
  }
  ```
- **Response `200 OK`**: `{"ok": true, "button": "VolumeUp"}` (the MQTT pulse topics of the old
  layer are gone, see 8.1).

---

### 5.4 Raw Infrared
#### `POST /api/ir-raw`
Sends a raw infrared frame given as carrier and microsecond timings (what an IR receiver or
Home Assistant's `infrared` platform produces). Capability flag: `ir_raw`.
- **Request Body** (`Content-Type: application/json`, up to 2048 timings):
  ```json
  {"frequency": 38000, "timings": [9000, -4500, 560, -560, 560, -1690], "port": 1, "repeats": 1}
  ```
  - `frequency`: carrier in Hz, `0` for unmodulated, otherwise 20000 to 60000. Default 38000.
  - `timings`: microseconds, 2 to 2048 values, each 1 to 65535 in absolute value. Either signed
    (positive mark, negative space, alternating) or all positive alternating mark/space starting
    with a mark. Mark/space pairs ending with the gap after the frame are the usual form; an odd
    count (ending with a mark) is accepted and the Hub adds 40 ms of silence after the last mark
    (not counted in `duration_ms`).
  - `port`: bitmask as `/api/ir-send` (`1` front blaster, `2` jack 1, `4` jack 2, `7` all). Default 7.
  - `repeats`: 1 to 10, default 1.
- **Response `200 OK`**: `{"ok": true, "count": 6, "duration_ms": 17}` (`duration_ms` = sum of the
  timings times `repeats`).
- **Error `400`**: `{"ok": false, "error": "<field>: ..."}` naming the offending field.
- **Error `409`**: `{"ok": false, "error": "busy"}` while another IR transmission holds the I2S
  blaster (the same lock the orchestrator and `/api/ir-send` take).
- Nothing of the frame is logged.

## 6. Bluetooth Stack & HID

### 6.1 Bluetooth Status
#### `GET /api/bt-status`
Returns `hub_btstack`'s status (`/tmp/hub_btstack_status.json`) with four fields `hub_daemon` adds:
`ok`, `running` (the `hub_btstack` process is alive), `connected` (any host connected) and
`pairing` (pairing mode on). `hosts` lists every paired host with its address (`addr`), name,
`connected`, `paired` and `cid`; `host` is the host keys go to. `target` and `remote` are the
Bluetooth LE remote, not a host; `state` is that link's state.
- **Response `200 OK`**:
  ```json
  {
    "backend": "btstack",
    "pid": 1234,
    "state": "connecting",
    "target": "11:22:33:44:55:66",
    "last_button": "",
    "last_seen": 0,
    "remote": {"connected": true, "addr": "11:22:33:44:55:66"},
    "host": {"connected": true, "addr": "AA:BB:CC:DD:EE:FF", "name": "Media Player", "pairing": false},
    "hosts": [
      {"addr": "AA:BB:CC:DD:EE:01", "name": "Television", "connected": false, "paired": true, "cid": 2},
      {"addr": "AA:BB:CC:DD:EE:FF", "name": "Media Player", "connected": true, "paired": true, "cid": 6}
    ],
    "ok": true,
    "running": true,
    "connected": true,
    "pairing": false
  }
  ```
  Without a status file (`hub_btstack` not started): `{"ok": true, "running": false, "state": "stopped", "connected": false, "pairing": false, "hosts": []}`.

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

The Hub connects only to the remote paired this way (the scan lists the remotes in range, the pair
request pins one). With no remote pinned it does not scan and does not connect to any BLE device on
its own; older builds bonded with any discoverable BLE HID device in range.

#### `POST /api/remote-unpair`
Forgets the pinned BLE remote: removes its address from `bt_remote_map.json`, and `hub_btstack`
disconnects it, deletes its target file, its LE bond and its stored HOGD record, then stays idle.
The Classic Bluetooth hosts (link keys, `devices.json`) are not touched.
- **Response `200 OK`**: `{"ok": true, "state": "standby", "map_cleared": true, "message": "Bluetooth remote forgotten"}` (`map_cleared` false when the map named no remote)

### 6.7 Remote Key Mappings
#### `GET /api/remote-mapping`
Returns physical key bindings for BLE remote.

#### `POST /api/remote-mapping-save`
Saves BLE remote button mapping JSON.

---

## 7. RF Subsystem & Physical Remotes (`/api/rf/*`, touchscreen remotes, Companion, Smart Control)

Requests starting with `/api/rf/*` are proxied to the native RF daemon `hub_rf` running on port 8092.

### 7.1 RF Subsystem Status
#### `GET /api/rf` or `GET /api/rf/status`
Returns CC2544 radio state, paired remote slots, model identification, screen capabilities, and channel hopping status.
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
      {
        "slot": 1,
        "active": true,
        "address": "12345678",
        "rf_address": "12345678",
        "model": "Harmony Companion",
        "has_screen": false,
        "supports_ota": false,
        "link": { "frames_rx": 1520, "acked_sent": 412, "acked_ok": 398, "retries": 14, "naks": 9, "last_seen_s": 2 }
      },
      {
        "slot": 2,
        "active": false,
        "address": "None",
        "rf_address": "None",
        "model": "None",
        "has_screen": false,
        "supports_ota": false
      }
    ]
  }
  ```
- `slots[].link`: RF traffic with the remote since `hub_rf` started: protocol frames received, frames sent that wait for an ACK (resends included) and how many were ACKed, resends, sends the Hub's CC2544 refused (NAK), and seconds since the last frame from the remote (`-1` before the first).

- `slots[].remote_id`: the remote's factory unit id (its `mfg` partition, `RF_MSG_UNIT_ID`),
  present once the remote reported it since pairing. The identifier for the remote's device in
  integrations; the slot number is only an attribute. Cleared on unpair.

#### `POST /api/rf/status-changed` (internal)
`hub_rf` posts the object above to this endpoint whenever it changed (once a second at most; the
`link` counters and `battery.voltage_mv` are not changes). The daemon turns it into an `rf_status`
WebSocket event (10.12). Accepted from loopback only (`403` otherwise); the body must be the status
object (`400` otherwise). Reply: `{"ok": true, "changed": true|false}`.

### 7.2 RF Pairing
#### `GET /api/rf/pair?slot={1|2}` or `POST /api/rf/pair`
Initiates RF pairing beacon on slot 1 or slot 2 for Harmony touchscreen remotes (Elite, Ultimate), Harmony Companion, or Harmony Smart Control remotes.
- **Response `200 OK`**: `{"status": "pairing_started"}`

#### `POST /api/rf/wake?slot={1|2}`
Turns the remote's screen on over RF (`RF_MSG_WAKE`), as a key press would; with the screen already on it restarts the idle timeout. Returns `202` `{"status":"waking","slot":1,"ok":true}` at once and tries for about 10 s in the background; `hub_rf` logs whether the remote acknowledged it. One wake runs at a time (`202` `already_waking` while one runs); `409` during an RF OTA, `400` for a Companion slot. Remotes without this message do not answer. A remote in deep standby (suspended to RAM on battery) does not receive it either: it reports again at its next hourly alarm or key press. An RF OTA start (`/api/rf/ota-start`) sends the same request first (4 attempts). Over USB/SSH the same is `touch /tmp/trigger_wake` on the remote.

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
Returns brightness, display timeout, tilt gyro, touch sensitivity, night mode, and nav button configuration for the touchscreen remote.

#### `POST /api/rf/remote-config?slot={1|2}`
Saves remote preferences and transmits push packet to the remote over RF.
- **Request Body**:
  ```json
  {
    "brightness": 3,
    "timeout": 120,
    "touchEnabled": true,
    "capButtonsEnabled": true,
    "gyroEnabled": true,
    "gyroRate": 50,
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
Puts paired touchscreen remotes to sleep (immediate Suspend-to-RAM on battery).
- **Query Parameter**: `slot` (optional: `1` for remote 1, `2` for remote 2, `0` or omitted for all active remotes).
- **Response `200 OK`**: `{"status": "sleep_dispatched", "slot": 1, "sent": 1, "ok": true}`
- **MQTT equivalent**: Publish to `harmony/hub/remote/1/sleep` or `harmony/hub/remote/sleep`.
- **Home Assistant**: Auto-discovered as `button.<hub>_remote_1_sleep`.

### 7.5 Direct Buttons & Firmware OTA (Touchscreen Remotes)
#### `GET /api/rf/remote-direct-buttons?slot={1|2}`
Returns touch screen direct command button grid for touchscreen remotes.

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
Starts background remote firmware flash update (touchscreen remotes only).

### 7.6 Physical Remote Key Mappings (Touchscreen Remotes, Companion, Smart Control)
#### `GET /api/rf-mapping?slot={1|2}`
Returns physical button and screen mappings for the remote in Slot 1 or Slot 2.
- **Query Parameter**: `slot` (`1` or `2`, defaults to `1`).
- **File Persistence**: Slot 1 uses `/data/harmony/remote_slot1_map.json` (fallback `/data/harmony/elite_remote_map.json`); Slot 2 uses `/data/harmony/remote_slot2_map.json`.
- **Remote Model Awareness**: Mappings apply to any paired remote in that slot: a Harmony touchscreen remote (Elite, Ultimate), Harmony Companion, or Harmony Smart Control.

#### `POST /api/rf-mapping-save?slot={1|2}`
Saves button assignments for the specified slot.
- **Query Parameter**: `slot` (`1` or `2`, defaults to `1`).

#### `POST /api/rf-mapping-copy?from={1|2}&to={1|2}`
Copies entire button mapping configuration from one remote slot to another.
- **Query Parameters**: `from` (source slot, 1 or 2), `to` (destination slot, 1 or 2).
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "message": "Copied mapping from slot 1 to slot 2"
  }
  ```

#### `POST /api/ui-remote-layout`
Saves virtual remote keypad layout for WebUI and Android App.

---

## 8. Network & Broker Configuration

### 8.1 MQTT (v2)
The MQTT layer carries the same information as the WebSocket (section 10): every state change is
published on change, retained where it is state, never on a timer; commands are acknowledged.
Capability flag `mqtt_v2`; `GET /api/mqtt-status` and the `mqtt` member of `/api/status` carry
`"v2": true`. On the first start of this firmware the old state topics get an empty retained
payload once, so brokers drop them (marker `/data/harmony/mqtt_v2_migrated`). The Home Assistant
discovery entities, the inbound command topics of the first MQTT layer and the pulse of MQTT-type
devices are kept (below).

#### `POST /api/mqtt-save` or `POST /mqtt`
- **Request Body** (every field optional; `password` is kept when absent or empty):
  ```json
  {
    "enabled": true,
    "host": "192.168.1.50",
    "port": 1883,
    "username": "homeassistant",
    "password": "password",
    "baseTopic": "harmony/hub",
    "clientId": "harmony_hub",
    "discoveryPrefix": "homeassistant",
    "haDiscovery": true
  }
  ```
  `haDiscovery` (default true) publishes the Home Assistant discovery entities under
  `discoveryPrefix`; turning it off clears them. The fields of the old layer with no v2 equivalent
  (`name`, `keepAlive`, `btRemoteEvents`/`Topic`, `remoteEvents`/`Topic`,
  `slotNRemoteEvents`/`Topic`) are ignored and removed from the stored file: button events are
  always published on `<base>/remote/<slot>/button`.
- **Response `200 OK`**: `{"ok": true, "message": "MQTT settings saved"}`; the client reconnects.

#### `GET /api/mqtt-status`
`{"ok": true, "connected": true, "broker": "...", "port": 1883, "baseTopic": "harmony/hub", "v2": true, "config": {...}}`

#### Topics
`<base>` is `baseTopic` (default `harmony/hub`). Payloads are JSON objects with `type`, `seq` and
`ts` as on the WebSocket (10.8 to 10.13), except `status`.

| Topic | Retained | QoS | Payload |
| --- | --- | --- | --- |
| `<base>/status` | yes | 1 | `online` / `offline` (last will) |
| `<base>/hub` | yes | 1 | `{"type":"hub", ...}`: the `hub` object of the snapshot (10.14) plus `ip`, `device_count` and `command_count` |
| `<base>/activity_state` | yes | 1 | `activity_state` (10.8) |
| `<base>/progress` | no | 0 | `activity_progress` (10.9) |
| `<base>/bt` | yes | 1 | `bt_status` (10.11) |
| `<base>/rf` | yes | 1 | `rf_status` (10.12) |
| `<base>/config` | yes | 1 | `{"type":"config","files":{mtimes},"inventory_version":"<8 hex>","ts"}` |
| `<base>/inventory/index` | yes | 1 | `{"type":"inventory_index","inventory_version","devices":[{"id","name","deviceType","manufacturer","model"}],"activities":[{"id","name","activityType","order"}]}` |
| `<base>/inventory/<deviceId>` | yes | 1 | `{"type":"inventory_device","id","name","deviceType","manufacturer","model","controlPort","commands":["PowerToggle", ...]}` (names only); split into `<deviceId>/<n>` with `part`/`parts` when a device does not fit one packet |
| `<base>/remote/<slot>/button` | no | 1 | `button` (10.13); events without a slot go to `<base>/button` |
| `<base>/cmd` | in | 1 | command request (below) |
| `<base>/cmd/response` | no | 1 | command reply (below) |
| `<base>/ir/raw` | in | 1 | the body of `POST /api/ir-raw` (5.4), optional `id` for a reply |

`inventory_version` is the first 8 hex digits of the SHA-256 over the device and activity files. A
device chunk is republished only when its own content changed, a removed device gets an empty
retained payload, and the index is published last. Retained topics are republished on every
(re)connect from the bus's last payloads. While the broker is down events are dropped (nothing is
queued) and the retained state is resent on reconnect.

The old state topics `<base>/state`, `<base>/activity`, `<base>/current_activity`,
`<base>/activity_id`, `<base>/activity/status` and the remote pulse topics (`remotes/slotN/Cmd`,
the per-remote custom topics) are gone: read `activity_state`, `hub`, `inventory/index` and
`remote/<slot>/button` instead.

#### Inbound topics of the first MQTT layer (kept)
These are aliases of the command channel for existing automations and for the discovery buttons:

| Topic | Payload | Does |
| --- | --- | --- |
| `<base>/activity/set` | an activity name, an activity id, `PowerOff` or `-1` | `start_activity` / `stop_activity` |
| `<base>/send_command` | `{"device": "<name>"` or `"deviceId": "<id>", "command": "<cmd>"` or `"commands": [...]`, optional `repeats`, `delay` (s) or `delay_ms`, `hold` (s) or `hold_ms`, `action`}` | the commands in order, `repeats` times, `delay` apart (0.25 s default for more than one) |
| `<base>/command/<device>/<command>` | any | one command to the device (id or name) |
| `<base>/sync` | any | republishes every retained topic and the discovery entities |
| `<base>/remote/<n>/sleep`, `<base>/remote/sleep` (slot 1) | any | `rf_sleep` |
| `harmony/ir/pronto` | a Pronto hex code | sends it on every IR port once |

They answer nothing on `cmd/response`; use `<base>/cmd` for acknowledged commands.

#### MQTT-type devices
A device of type `MQTT` (or with `mqttEnabled`) is driven through the broker instead of IR or
Bluetooth: a command to it (an activity sequence, `POST /api/ir-send`, the remote, the command
channel) publishes `"1"` on its topic, then `"0"` after `mqttPulseMs` (default 1000), and raises the
`button` event (10.13) as well. The topic comes from the device's `mqttTopic` template
(default `{base_topic}/button/{device}/{command}`; placeholders `{base_topic}` or `{root}`,
`{device}` or `{device_id}`, `{command}` or `{button}`; a template starting with `/` is appended to
the base topic). `POST /api/device-save` stores `mqttTopic` and `mqttPulseMs`;
`POST /api/device-mqtt-test` (3.8) publishes one pulse.

#### Home Assistant discovery
With `haDiscovery` on, the Hub publishes retained discovery configs under
`<discoveryPrefix>/<component>/<ident>_<object>/config` (`ident` = the client id, lower case) on
every connect, on Home Assistant's birth message (`<discoveryPrefix>/status` = `online`), on
`<base>/sync` and whenever the configuration changes; turning `haDiscovery` off publishes empty
payloads to the same topics. The object ids are those of the first MQTT layer, so entities created
before v2 are kept. All entities belong to one device (identifiers: `ident` and
`harmony-ng-<hub_id>`, name = the hub name, `configuration_url` = the web UI) and use
`<base>/status` for availability:

| Entity | Reads | Writes |
| --- | --- | --- |
| select `Activity` (options `PowerOff` and the activity names) | `activity_state.activity`, mapped to the name in the template | `<base>/activity/set` |
| sensor `Activity ID` (diagnostic) | `activity_state.activity` | |
| sensor `IP Address` (diagnostic, attributes = the `hub` object) | `hub.ip` | |
| sensor `Firmware` (diagnostic) | `hub.version` | |
| sensor `IR Devices`, `IR Commands` (diagnostic) | `hub.device_count`, `hub.command_count` | |
| button `Power Off` | | `<base>/activity/set` = `PowerOff` |
| button `Sync` (config) | | `<base>/sync` |
| button `Remote 1 Sleep`, `Remote 2 Sleep` (config) | | `<base>/remote/<n>/sleep` |

The harmony-ng Home Assistant integration does not use these entities; they are for installations
that drive the Hub through the MQTT integration only.

#### Command channel
Publish to `<base>/cmd`:
```json
{"id": "c7d1", "action": "start_activity", "activity": "123456"}
```
`id` is an opaque client string (up to 64 characters), echoed in the reply; a request without `id`
is executed and not answered. Actions and fields mirror the WebSocket actions (section 10):
`ping`, `get_state`, `get_snapshot` (the reply carries the snapshot), `start_activity` (`activity`),
`stop_activity`, `cancel_activity`, `send_command` (`deviceId`, `command`, `status` = `press`,
`hold`, `release` or omitted for a tap), `bt_key` (`key`, `keyAction`, `target`), `bt_text`,
`bt_mouse`, `rf_wake` (`slot`), `rf_sleep` (`slot`), `sync`, `ir_raw` (fields of 5.4),
`update_check`, `update_apply`, `activity_help`, `activity_help_answer` (10.6a, 10.6b).

Reply on `<base>/cmd/response`:
```json
{"type": "cmd_response", "id": "c7d1", "action": "start_activity", "ok": true}
{"type": "cmd_response", "id": "c7d1", "action": "start_activity", "ok": false, "code": 409, "error": "busy"}
{"type": "cmd_response", "id": "c7d2", "action": "get_state", "ok": true, "activity_state": {...}}
```
Codes and errors equal the REST ones (400 bad input, 404 not found, 409 busy, 503 a service is not
running). An ack carries the fields of the REST reply as well (`count` and `duration_ms` for
`ir_raw`, the update and Help fields), on MQTT next to `ok`:
```json
{"type": "cmd_response", "id": "u1", "action": "update_check", "ok": true, "current_version": "2.3.0", "remote_version": "2.4.0", "update_available": true, "updating": false, "message": "Update available: v2.4.0 (Current: v2.3.0)", "...": "..."}
```
Malformed JSON is answered with `code` 400 when an `id` can be recovered, otherwise logged.

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

#### 10.8a Full State Snapshot
```json
{
  "action": "get_snapshot",
  "msgId": 8
}
```
**Response**: a `snapshot` message (10.14) with the same `msgId`. One is also sent, without
`msgId`, as the first frame after the upgrade.

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

#### 10.6a Firmware Update Check and Install
`update_check` runs the check of `POST /api/update-check` (optional `repo`, `token`,
`remote_version`, as there) and answers with the fields of `GET /api/update-status` read after the
check, plus the check's `message`. The check runs `update.sh check` or downloads the published
version, which takes seconds: the action runs it on a thread and the reply comes when it ends
(other actions are answered meanwhile). One check at a time; another request meanwhile gets
`409`. `update_apply` starts `POST /api/update-apply` (optional `repo`, `token`) and answers at
once with its `status` (`applying`) and `message`; the Hub restarts its services when the update
is installed. Both are admin actions, like their REST endpoints.
```json
{"action": "update_check", "msgId": 41}
```
**Response** (when the check ended):
```json
{
  "type": "ack",
  "action": "update_check",
  "ok": true,
  "msgId": 41,
  "current_version": "2.3.0",
  "remote_version": "2.4.0",
  "update_available": true,
  "updating": false,
  "message": "Update available: v2.4.0 (Current: v2.3.0)",
  "commit": "...", "firmware": "...", "local_version": "...", "build_time": "..."
}
```

#### 10.6b Activity Help
`activity_help` is `POST /api/activity/help`: re-sends the running activity's commands except
power toggles and lists the devices with a power toggle to ask about (`activity`, defaults to the
running one; `dry_run` returns the steps as `preview` and sends nothing). `activity_help_answer`
is `POST /api/activity/help/answer` (`device`, `on`): for `on: false` the device's power toggle is
sent; only devices the last Help listed, one answer each (`404` otherwise). A Help on an activity
that is not running gets `409`.
```json
{"action": "activity_help", "msgId": 51}
{"action": "activity_help_answer", "device": "2", "on": false, "msgId": 52}
```
**Responses**:
```json
{"type": "ack", "action": "activity_help", "ok": true, "msgId": 51, "activity": "11",
 "toggleDevices": [{"id": "2", "name": "Media Player", "command": "PowerToggle"}]}
{"type": "ack", "action": "activity_help_answer", "ok": true, "msgId": 52, "sent": true}
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

### Server-to-Client Messages (pushed events)

Every state change the Hub knows about goes through its event bus and is pushed to every
WebSocket client as one text frame. Each pushed message carries `type`, `seq` (sequence number,
from 1 per daemon run, increasing across all types) and `ts` (Unix seconds). Nothing is replayed to
a client that connects later: it fetches the current state over REST (or the `snapshot`, see
10.10). A client that fails a send is dropped.

#### 10.8 Activity State
Sent on every change of the orchestrator state or of the current activity, including cancel and
dry runs, and once at daemon start. Same shape as the `get_state` reply and `GET /api/activity`.
```json
{
  "type": "activity_state",
  "seq": 17,
  "ts": 1762387200,
  "activity": "123456",
  "state": 2,
  "stateName": "running",
  "busy": false,
  "sending": true
}
```
- `state`: `0` idle, `1` starting, `2` running, `3` stopping; `stateName` is the same as a word.
- `activity` is `"-1"` when idle; while starting it is still the previous activity.

#### 10.9 Activity Progress
One frame per executed step of an activity switch (unchanged shape, now carrying `seq` and `ts`).
```json
{
  "type": "activity_progress",
  "seq": 18,
  "ts": 1762387200,
  "activity": "123456",
  "activityId": "123456",
  "state": "starting",
  "stateCode": 1,
  "step": 2,
  "currentStep": 2,
  "total": 6,
  "totalSteps": 6,
  "desc": "TV PowerOn",
  "stepDesc": "TV PowerOn"
}
```

#### 10.10 Configuration Changed
Sent when a configuration file changed on disk (checked once a second by mtime and size):
`devices`, `activities`, `settings` or `remote_map` (either slot's button map). Reload the
affected inventory over REST.
```json
{"type": "config_changed", "seq": 19, "ts": 1762387200, "files": ["devices"]}
```

#### 10.11 Bluetooth Status
The `GET /api/bt-status` object (section 6.1), sent when it changes (`last_button`, `last_seen` and
`updated` do not count as a change; `running` turning false when `hub_btstack` dies does).
```json
{"type": "bt_status", "seq": 20, "ts": 1762387200, "ok": true, "running": true, "connected": true, "pairing": false, "host": {...}, "hosts": [{"addr": "AA:BB:CC:DD:EE:FF", "name": "Media Player", "connected": true, "paired": true, "cid": 6}], "...": "..."}
```

#### 10.12 RF Remote Status
The `GET /api/rf/status` object (section 7.1), sent when it changes (the `link` counters and
`battery.voltage_mv` do not count as a change, but are included).
```json
{"type": "rf_status", "seq": 21, "ts": 1762387200, "ok": true, "slots": [ ... ]}
```

#### 10.13 Remote Button
One event for the first press and one for the release of a physical key on an RF remote (held
keys repeat on the Hub but give no further events). `button` is the key name as the remote's
keymap knows it; `deviceId` and `command` are the device command the key was mapped to in the
current activity, absent when the key was not mapped to a device command. Calls of
`POST /api/button-press` from other sources give the same event with `source` `"api"`.
```json
{
  "type": "button",
  "seq": 22,
  "ts": 1762387200,
  "slot": 1,
  "button": "VolumeUp",
  "action": "press",
  "source": "remote_rf",
  "deviceId": "70321456",
  "command": "VolumeUp"
}
```

#### 10.13a MQTT Status
The broker connection state, pushed when hub_daemon starts, when the client connects or loses the
broker, and after `POST /api/mqtt-save`. `broker` is empty while MQTT is disabled. Both web UIs
update the MQTT page's badge from it.
```json
{"type": "mqtt_status", "seq": 23, "ts": 1762387200, "enabled": true, "connected": true,
 "broker": "192.168.1.50", "port": 1883, "baseTopic": "harmony/hub", "haDiscovery": true}
```

#### 10.14 Snapshot
The first frame a client receives after the upgrade, and the reply to `get_snapshot` (10.8a).
`activity_state`, `bt_status`, `rf_status` and `mqtt_status` are the last payloads pushed for those types
(`null` before the first one; `activity_state` is built from the orchestrator if needed).
`config.files` holds the mtimes (Unix seconds) the watcher tracks, so a client can tell whether its
cached inventory is stale. `seq` is the bus sequence at the time of the snapshot: a client that
stored the last `seq` it processed can tell whether it missed events while disconnected and
refetch over REST. The frame can exceed 4 KB (outbound frames may be up to 64 KB).
```json
{
  "type": "snapshot",
  "msgId": 8,
  "hub": {"hub_id": "20140101120000-000001", "hub_name": "Living Room Hub", "version": "2.2.0",
          "api_version": 2, "capabilities": ["events", "snapshot", "ws_auth", "ws_keepalive", "ir_raw", "rf_wake"],
          "auth_enabled": false, "uptime_sec": 34821, "legacy_enabled": false, "mdns_enabled": false},
  "activity_state": {"type": "activity_state", "activity": "-1", "state": 0, "stateName": "idle", "busy": false, "sending": false, "seq": 3, "ts": 1762387100},
  "bt_status": {"type": "bt_status", "seq": 20, "ts": 1762387200, "...": "..."},
  "rf_status": {"type": "rf_status", "seq": 21, "ts": 1762387200, "...": "..."},
  "mqtt_status": {"type": "mqtt_status", "seq": 23, "ts": 1762387200, "enabled": true, "connected": true, "...": "..."},
  "config": {"files": {"devices": 1762387000, "activities": 1762387000, "settings": 1762380000, "remote_map": 1762387000}},
  "seq": 42,
  "ts": 1762387200
}
```

#### 10.15 Keepalive
The server sends a WebSocket ping to a client that has sent nothing for 30 s; a client that
answers nothing (pong or any other frame) within 10 s of the ping is closed. Client pings are
still answered with pongs. Capability flag: `ws_keepalive`.

---

## 11. Authentication & Security Architecture

Harmony Hub supports a multi-tier authentication model:

### 11.1 HTTP Digest Authentication (RFC 7616 / RFC 2617)
Protects credentials on the wire without requiring SSL/TLS overhead on embedded hardware. Nonces are HMAC-SHA256 signed with dynamic expiry.

```bash
# Digest authentication with curl
curl --digest -u admin:secret123 http://192.168.1.1:8089/api/status
```

### 11.2 OAuth2 Bearer Session Tokens (RFC 6749 / RFC 6750)
Recommended for SPAs, WebUI, and Mobile Apps.

```bash
# 1. Acquire Bearer token
curl -X POST http://192.168.1.1:8089/api/auth/token \
  -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"secret123"}'

# Response:
# {"ok":true,"access_token":"hht_018f...","token_type":"Bearer","expires_in":86400,"username":"admin","role":"admin"}

# 2. Use Bearer token
curl -H "Authorization: Bearer hht_018f..." http://192.168.1.1:8089/api/status

# 3. Invalidate token / Logout
curl -X POST -H "Authorization: Bearer hht_018f..." http://192.168.1.1:8089/api/auth/logout
```

### 11.3 Personal Access Tokens (PAT) for Home Automation
Designed for Home Assistant, Homebridge, Node-RED, or automation scripts. Never requires sharing or hardcoding admin passwords.

```bash
# 1. Generate new Personal Access Token (Admin only)
curl -X POST -H "Authorization: Bearer hht_..." \
  -H "Content-Type: application/json" \
  -d '{"name":"Home Assistant","role":"admin"}' \
  http://192.168.1.1:8089/api/auth/tokens

# Response:
# {"ok":true,"id":"tok_6a1d8f","name":"Home Assistant","role":"admin","token":"pat_3f8a9b..."}

# 2. Authenticate Home Assistant REST sensor / command
curl -H "Authorization: Bearer pat_3f8a9b..." http://192.168.1.1:8089/api/current_activity

# 3. List active tokens
curl -H "Authorization: Bearer hht_..." http://192.168.1.1:8089/api/auth/tokens

# 4. Revoke a token
curl -X DELETE -H "Authorization: Bearer hht_..." "http://192.168.1.1:8089/api/auth/tokens?id=tok_6a1d8f"
```

### 11.4 WebSocket Token Authentication
When authentication is enabled, the WebSocket upgrade (`GET /ws`) must carry a token: either
`Authorization: Bearer <token>` or, since browsers cannot set headers on a WebSocket, `?token=<token>`
in the request line:
`ws://192.168.1.1:8089/ws?token=pat_3f8a9b...`

- Personal access tokens (`pat_...`, 11.3) and session tokens (`hht_...`, 11.2) are both accepted.
  Digest and Basic credentials are not supported on the upgrade.
- Missing, unknown or revoked token: `401 Unauthorized` with the usual challenge; the connection is
  closed, never upgraded. Callers on the Hub itself (`127.0.0.1`, `::1`: `hub_rf`, `hub_btstack`)
  need no token, as for every request. With authentication disabled nothing changes.
- The role of the token is kept for the connection. A `user` role receives every pushed message
  and may send `ping`, `get_state` and `get_snapshot`; its command actions follow the same
  permission table as the REST equivalents (`send_command` = `/api/ir-send`, `start_activity` =
  `/api/activity`, `stop_activity` = `/api/poweroff`, `cancel_activity` = `/api/activity-cancel`,
  `bt_key`, `bt_text`, `bt_mouse` = their endpoints). A refused action is answered with
  `{"type": "ack", "action": ..., "ok": false, "error": "Forbidden: Admin access required"}`.
- Both web UIs pass their session token as `?token=` when they open the WebSocket.
- Capability flag: `ws_auth`.

