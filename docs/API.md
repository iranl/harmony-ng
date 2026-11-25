# Harmony Hub HTTP & WebSocket API Specification

`hub_daemon` provides an ultra-lightweight REST and WebSocket interface running natively on port 8089 (HTTP / WebSocket). RF hardware calls are transparently proxied to `hub_rf` on port 8092 via `/api/rf/*`.

**Time limits.** A request's line and headers must arrive within 10 s and its body within 20 s, with no gap longer than 5 s; a keep-alive connection takes no new request after 30 s (only the web UI's static files are served keep-alive: every API answer closes the connection). A client that is slower is disconnected, so it cannot hold one of the Hub's 8 request workers (the same limits apply on port 8088).

**Requests from web pages.** Port 8089 refuses requests and WebSocket upgrades that a browser sends on behalf of another web site: an `Origin` header that is not the Hub's own clients answers `403`, and so does a request without `Origin` that the browser marks `Sec-Fetch-Site: cross-site` or `same-site`. A WebSocket upgrade refused this way gets no answer: the connection is closed. Allowed origins: the Hub itself (the `Origin` host matches the request's `Host`, or `X-Forwarded-Host` behind a reverse proxy), `http://` and `https://` with `localhost`, `127.0.0.1` or `[::1]` on any port (the Android app, local development servers), `capacitor://localhost` and `ionic://localhost`. Clients outside a browser (the Home Assistant integration, scripts, `curl`) send neither header and are not affected. A CORS preflight (`OPTIONS`) from any other origin is answered `403`. On port 8088 (Logitech protocol) only the WebSocket upgrade is checked: one with an `Origin` other than the request's own `Host` is answered `403`; its two HTTP routes (1.12) are not checked.

**Request size.** A request body may be at most 64 KB, except `POST /api/rf-mapping-save`, `POST /api/remote-mapping-save` and `POST /api/ui-remote-layout` (a whole button map or the web remote layouts, up to 256 KB), and `POST /import`, `POST /api/custom-icons` and `POST /api/plugins/install` (up to 2 MB; the icon image itself may be at most 16 KB, see 1.8). A larger `Content-Length` is answered `413` before the body is read.

**Request bodies.** Most `POST` routes take a JSON object, or a form-encoded body (`a=1&b=2`; every value is then a string). JSON only: activity start, power off, Help and its answer, `activity-progress`, `pronto-blast`, `ir-raw`, `ir-capture`, `button-press` and `custom-icons`.

---

## 1. System, Status & Diagnostics

### 1.1 Health Check
#### `GET /health` or `GET /`
Returns service status. `/health` needs no credentials. `GET /` gives this answer only on a Hub
without a web UI: when one is installed, `/` is its `index.html`.
- **Response `200 OK`**:
  ```json
  {
    "status": "ok",
    "service": "hub_daemon",
    "version": "2.4.2",
    "min_app_version": "1.3.0"
  }
  ```
  `version` is this firmware's version; `min_app_version` is the oldest Android app the Hub works with.

### 1.2 Full Hub Status
#### `GET /api/status`
Returns high-level hub status, firmware, identity, active activity, flash space, uptime, and MQTT connection status.
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
    "capabilities": ["events", "snapshot", "ws_auth", "ws_keepalive", "ir_raw", "ir_capture", "mdns", "legacy", "mqtt_v2", "rf_wake"],
    "legacy_enabled": false,
    "mdns_enabled": true,
    "boot_time": 1762352379,
    "ir_capture": false,
    "activity": "123456",
    "state": 2,
    "busy": false,
    "ip": "192.168.1.100",
    "total_kb": 5120,
    "free_kb": 1372,
    "cache_total_kb": 8192,
    "cache_free_kb": 2048,
    "updating": false,
    "auth_enabled": true,
    "role": "admin",
    "user": "admin",
    "uptime_sec": 34821,
    "mqtt": {
      "connected": true,
      "broker": "192.168.1.50",
      "port": 1883,
      "baseTopic": "harmony/hub",
      "v2": true
    },
    "has_icons": true
  }
  ```
  - `firmware`: the Logitech base firmware. This route always reports `"4.15.600"`; the version
    read from the Hub (`/etc/version`) is in `/api/system-info` and `/api/update-status`. `version`
    and `commit`: this firmware, as in `/api/system-info`.
  - `total_kb`, `free_kb`: size and free space of the `/data` flash partition, in KB (not memory);
    `cache_total_kb`, `cache_free_kb`: the same for `/cache`.
  - `updating`: `true` while a firmware update runs or the services restart after it, as in `GET /api/update-status` (2.1).
  - `auth_enabled`: whether authentication is on. `role` (`"admin"` or `"user"`) and `user` are the
    caller's own: the account name, `pat:<token name>` for a personal access token, `"admin"` when
    authentication is off or the request comes from the Hub itself.
  - `has_icons`: the stock icon set can be served (read from the packed archive, or unpacked into `/tmp`).
  - `mqtt.broker` is `""` while MQTT is disabled.
  - `hub_id`: the factory unit id from the `mfg` partition (unique per unit, survives reflashes); the
    identifier for device registries and unique ids. Falls back to the Wi-Fi MAC (12 lowercase hex
    characters, logged as a warning) only when the partition has no unit id, and is the text
    `"unknown"` when the Hub has neither.
  - `api_version`: `2` from milestone 1 of the integration work on; it changes only on a breaking
    change to an existing endpoint or message. Additions are announced through `capabilities`.
  - `uptime_sec`: seconds since boot, right when the object was built. `boot_time`: the boot time as
    a Unix epoch, `null` while the Hub's clock is not set yet (it starts at 1970 and `hub_sntp` sets
    it after the network is up). Use `boot_time` for anything stored or retained: `uptime_sec` from
    an older copy of the object gives a wrong boot time. The same two fields are in the snapshot's
    `hub` object (10.14) and the retained MQTT `<base>/hub` (8.1), which is published again once the
    clock is set.
  - `capabilities`: flags of the features compiled into this firmware, independent of settings:
    `events` (event bus, pushed WebSocket messages 10.8 to 10.13), `snapshot` (10.14), `ws_auth`
    (11.4), `ws_keepalive` (10.15), `ir_raw` (5.4), `ir_capture` (5.5), `mdns` (1.11), `legacy`
    (1.12), `mqtt_v2` (8.1), `rf_wake` (`POST /api/rf/wake`).
  - `legacy_enabled`, `mdns_enabled`: mirror the settings of the Logitech protocol shim (default
    off) and the mDNS responder (default on), see 1.9.
  - `ir_capture`: whether the IR capture mode (5.5) is on; `false` at every start, never stored.
    `ir_capture_error` appears only while the mode is on and its receiver has a problem. Both are
    also in the snapshot's `hub` object (10.14) and the retained MQTT `<base>/hub` (8.1); a change is
    pushed as `ir_capture` (10.13c) and `<base>/hub` is published again.

### 1.3 System Info
#### `GET /api/system-info`
Returns Linux system uptime, CPU load average, kernel version, RAM breakdown, and network statistics.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "hub_name": "Living Room Hub",
    "uptime": "1d 4h 12m (101520s)",
    "loadavg": "0.12 0.08 0.05",
    "kernel": "Linux version 3.3.8 (mips Atheros AR9344)",
    "firmware": "4.15.600",
    "version": "2.4.2",
    "commit": "abc1234",
    "build_time": "Nov 14 2026 18:00:00",
    "memory": "Total: 128 MB, Free: 48 MB (Avail: 72 MB)",
    "mem_total_kb": 131072,
    "mem_free_kb": 49152,
    "mem_available_kb": 73728,
    "network_details": "=== Network Interfaces (/proc/net/dev) ===\n..."
  }
  ```

### 1.4 Disk Space
#### `GET /api/disk-space`
Returns the flash space of `/data` (of root `/` when `/data` cannot be read) and of `/cache`
(the `cache_` fields), as one flat object.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "total_kb": 5120,
    "free_kb": 1372,
    "used_kb": 3748,
    "percent_free": 26,
    "cache_total_kb": 8192,
    "cache_free_kb": 2048,
    "cache_used_kb": 6144,
    "cache_percent_free": 25,
    "stock_resources_kb": 251,
    "stock_resources_saved": true,
    "can_empty_stock_resources": true
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
    "eth_onboard": false,
    "eth_carrier": true,
    "eth_interface": "eth0",
    "eth_ip": "192.168.1.100",
    "eth_mask": "255.255.255.0",
    "eth_mac": "02:00:00:00:00:01",
    "wifi_connected": true,
    "wifi_interface": "ath0",
    "wifi_ip": "192.168.1.101",
    "wifi_mask": "255.255.255.0",
    "wifi_mac": "02:00:00:00:00:02",
    "wifi_ssid": "ExampleSSID",
    "config": {
      "eth_enabled": true,
      "eth_fallback_wifi": true,
      "eth_mode": "dhcp",
      "eth_ip": "",
      "eth_netmask": "255.255.255.0",
      "eth_gateway": "",
      "eth_dns": "",
      "usb_wifi_setup": true
    },
    "ifconfig": "ath0      Link encap:Ethernet ..."
  }
  ```
  - `eth_interface` is `"none"` without an Ethernet adapter; `eth_onboard` tells an Ethernet port
    of the Hub itself from a USB adapter.
  - `config`: the stored Ethernet settings (`ethernet.conf`, written by `POST /api/ethernet-save`,
    8.3); only `usb_wifi_setup` when there is no file. `usb_wifi_setup` is always there: `false`
    when `ethernet.conf` holds `USB_WIFI_SETUP=0`, else `true` (the default).
  - `ifconfig`: the text output of `/sbin/ifconfig`.

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
- **Request Body**: `{"enable": true}`. A request without `enable` turns the service off (the same
  for `bt-toggle`).
- **Response `200 OK`**: `{"ok": true, "rf_enabled": true}`
- **Response `500`**: the setting could not be stored when turning the service off (the same for
  `bt-toggle`).

#### `POST /api/services/bt-toggle` or `POST /api/bt-toggle`
Enables or disables the Bluetooth stack `hub_btstack`. Off writes `/data/harmony/bt_disabled.conf`
and stops `hub_btstack`; the supervisor in `init.sh` keeps it stopped while the file exists. On
removes the file and the supervisor starts it again within about 5 s. `GET /api/services` reports
`bt_enabled` from the file and `bt_running` from the process list.
- **Request Body**: `{"enable": true}`
- **Response `200 OK`**: `{"ok": true, "bt_enabled": true, "bt_running": false}`. `bt_enabled` is
  the new setting; `bt_running` is the process at the time of the answer, so right after "on" it is
  usually still `false`: read `GET /api/services` again to see the supervisor start it.

### 1.7 Hub Logs
#### `GET /api/logs?source={daemon|btstack|rf|system|network}`
Returns tail (up to 12KB) of requested daemon or system log.
- **Parameters**: `source` (default: `daemon`). `daemon` and `rf` read `/tmp/harmony-init.log` (the output of hub_daemon and hub_rf), `btstack` `/tmp/hub-btstack.log`, `network` `/tmp/harmony-network.log`, `system` the output of `logread` (or `dmesg`); for `system` it is not the tail but the first 16 KB of that output. Any other value gives the `daemon` log, and `source` in the reply says which one was sent.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "source": "daemon",
    "logs": "[+] HTTP & WebSocket server listening on port 8089\n..."
  }
  ```

**Debug logs.** While `/data/harmony/debug_logging.conf` (or `/data/harmony/bt_remote_debug`)
exists on the Hub, two more logs are written. `touch` the file over SSH to turn them on and `rm` it
to turn them off; no restart is needed. Both are in RAM (`/tmp`, gone after a reboot) and are not
served by `/api/logs`: read them over SSH.
- `/tmp/bt_sent.log`: one line per Bluetooth command that `hub_daemon` hands to `hub_btstack`
  (device, command, action, HID page and usage, result). At 64 KB it becomes `/tmp/bt_sent.log.1`.
- `/tmp/key_trace.log`: one line per key from a remote that goes to a Bluetooth device, with the
  time the key spent in each part of the Hub, in milliseconds:
  ```
  21:04:07.512 KBD_DOWN 4F      rf>daemon=1 daemon=1 pipe=0 bt=1 total=3 ms
  ```
  `rf>daemon`: from `hub_rf` receiving the key from the remote until `hub_daemon` took it from
  the key channel. `daemon`: until `hub_daemon` wrote the command to the key pipe. `pipe`: until
  `hub_btstack` read it. `bt`: until the report was handed to the Bluetooth stack; the line ends
  with `(waiting in the report queue)` when the stack did not take the report at once. `total`:
  from `hub_rf` to the stack. The time on the radio link before `hub_rf` and after the stack is
  not in it. At 256 KB the file becomes `/tmp/key_trace.log.1`.

### 1.8 Hub Assets & Icons
#### `GET /api/icons`
Returns list of available device and activity PNG icon filenames: the stock icons, then the
uploaded ones (`/data/harmony/custom_icons`).
- **Response `200 OK`**:
  ```json
  [
    "tv.png",
    "receiver.png",
    "gamepad.png",
    "apple_tv.png"
  ]
  ```

#### `GET /api/icons/archive` (also `HEAD`, and `GET /assets/icons.tar.gz`)
The stock icon archive (`icons.tar.gz` from `/cache`, `/data/harmony` or the web UI folder) as
`application/gzip`; `404` when the Hub has none.

#### `GET /api/custom-icons`
Lists the uploaded icons: `{"ok": true, "icons": [{"filename", "url", "size", "type", "id"}]}`
(`type` `activity` or `device` when the file name tells it).

#### `POST /api/custom-icons`
Uploads one PNG: `{"type": "activity", "id": "12", "image_base64": "<base64 PNG>"}` (or `name`
instead of `type` and `id`; `data` instead of `image_base64`). Stored as `<type>_<id>.png` (or
`custom_<name>.png`) in `/data/harmony/custom_icons/`, and `hub_rf` is asked to send it to the
paired touchscreen remotes. Reply `{"ok": true, "filename", "url", "size"}`; `400` for a missing
field or data that is not a PNG image, `413` when the decoded image is larger than 16 KB, `507`
when the upload would give more than 128 uploaded icons or more than 512 KB of them (an icon of
the same name is replaced and does not count).

#### `DELETE /api/custom-icons?filename=<file>` (or `?type=<type>&id=<id>`)
Deletes an uploaded icon. Reply `{"ok": true, "deleted": "<file>"}`.

#### `GET /api/custom-icons/proxy-fetch?url=<http or https URL>`
Admin only. The Hub downloads the image at that URL itself and returns it as `image/png`, so the
web UI can take an icon from another site. Only `http://` and `https://` URLs; `502` when the
download fails or takes longer than 20 s, `413` when the file is empty or over 2 MB.

### 1.9 System Settings
#### `GET /api/settings`
Returns global hub settings from `/data/harmony/config/settings.json`.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "hub_name": "Living Room Hub",
    "dry_run": false,
    "ntp_server": "",
    "timezone": "Europe/Amsterdam",
    "led_mode": "green",
    "mdns_enabled": true,
    "legacy_enabled": false
  }
  ```
  - `dry_run`: while `true`, `hub_daemon` simulates its outputs: nothing goes out over infrared,
    Bluetooth or MQTT, the sends are only logged. It is the state of the running daemon (set
    here, or by starting `hub_daemon` with `--dry-run`), not a stored setting: it is never stored
    as `true`, so the Hub always starts with it off (after a restart or reboot real commands go
    out again).
  - `ntp_server` (default empty, meaning `pool.ntp.org`): the time server `hub_daemon` takes when it
    starts; a new value is stored at once but used only after the next restart of `hub_daemon`.
  - `mdns_enabled` (default `true`): the mDNS responder (1.11). Changing it starts or stops the
    responder at once.
  - `legacy_enabled` (default `false`): the Logitech protocol shim on port 8088 and its SSDP
    responder (1.12). Changing it opens or closes the port at once; turning it off also closes
    every client connected to it.
  - Time sync: `ntp_server` (empty means `pool.ntp.org`) is the time server asked first; the public pools
    (`pool.ntp.org`, `0.pool.ntp.org`, `1.pool.ntp.org`, `time.cloudflare.com`,
    `time.google.com`) follow. A correction of up to an hour from one server is applied at once
    (one server can still move the clock by up to an hour in each round). A larger step, and the
    first time after a boot, needs two servers at different addresses that agree within 5 s;
    until then the clock is not stepped. When the clock is not set and only one server answers,
    its time is used for now if it is not later than the firmware's build date (UTC) plus 5 years.
    Rounds: every 20 s while the clock is not set; while a time from one server is not checked yet
    or a step was refused, every 5 minutes for the first hour, then once an hour; otherwise every
    4 hours.

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
  *Note: `led_mode` sets the Hub's front light while it runs normally: `"green"` (default), `"amber"` (`"orange"` is read as amber), `"red"` or `"off"`. The light flashes green fast while it pairs a remote and alternates red and green slowly during an RF firmware update, then returns to this setting. A pairing that times out or a failed update flashes red slowly for 5 s first. The older values `"always_on"` (green) and `"always_off"` (off) are still accepted, and `"flashing_fast"` and `"flashing_slow"` now mean green.*
  *Note: `timezone` accepts standard IANA timezone identifiers (e.g. `"Europe/Amsterdam"`, `"America/New_York"`, `"Asia/Tokyo"`) or explicit POSIX TZ strings (e.g. `"CET-1CEST,M3.5.0,M10.5.0/3"`). An `Area/City` name the Hub's table does not know is refused with `400` (it used to run as Central European time).*
  *Note: `led_mode` is stored as sent, without a check: a value the Hub does not know shows green. Fields left out keep their stored values.*
- **Response `200 OK`**: `ok` plus every setting after the save, in the same form as `GET /api/settings`. An empty `hub_name` is ignored (the stored name is kept).
- **Response `500`**: the settings could not be stored (flash full).

#### `POST /api/bt/reconnect` or `POST /bt/reconnect`
Triggers on-demand Bluetooth host reconnection supervision. `target` (also accepted as `bdaddr` or `deviceId`) must be a Bluetooth address `XX:XX:XX:XX:XX:XX` or be left out (the default host); anything else answers `400`. `duration` (default 30 s) and `delay` (default 0) are capped at 3600 s.
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
Answers of the actions below:
- `reboot`: `{"ok": true, "message": "Rebooting hub now"}`; `409` while a firmware update runs
  or the services restart after it (`updating` of `GET /api/update-status`, 2.1).
- `factory_reset`: `{"ok": true, "message": "Factory reset initiated. Reverting to stock firmware and rebooting."}`;
  `400` without `"confirm": "RESET"`.
- `clean_stock_resources`: empties `/data/resources`, the stock Logitech files. Needs
  `"confirm": "EMPTY"` (`400` without). `409` while `/data/harmony/config/devices.json` is missing or empty
  (the stock import still needs the files), when `/data/resources` is a link, and while no saved copy of
  the files as they are exists: `/data/harmony/stock_resources_saved` holds `<copy name> <count>:<hex>`,
  written by `tools/backup_hub.py` once `backups/stock_resources_<hub>*.tar` holds exactly the files (every
  `deploy_hub.py` run makes that backup). `<hex>` is the XOR of sha256(`"<size> <path>"`) over every
  entry under `/data/resources` but directories (path relative to it, a link counts as size 0); the
  route computes it again and refuses when it differs. `500` when a file could
  not be removed or the directory not read. Answers
  `{"ok": true, "freed_kb": 48, "stock_resources_kb": 0, "message": "..."}`: `freed_kb` is the free space
  of `/data` gained. In `GET /api/disk-space`, `stock_resources_kb` is the files' own size; `/data` is
  JFFS2, which compresses, so the flash space they take (and `freed_kb`) is smaller.
  `stock_resources_saved` says whether the mark matches the files there now (false when there are
  none), `can_empty_stock_resources` whether the action would run. The stock firmware needs these
  files: `tools/restore_stock.py` puts the saved copy back on a stock restore whose archive holds none,
  and refuses without it.
- `mqtt_publish`: only `topic` and `payload` are read. `retain` is ignored: the message goes out
  with QoS 0 and is not retained. An empty `payload` is sent as `cmd`. The answer is
  `{"ok": true}` also when MQTT is off or `topic` is empty, so it does not tell whether anything
  was published.
- A fifth action, `auth`, turns authentication on or off and sets the admin account (11.6).
- An unknown or missing `action` answers `200` `{"ok": true}` and does nothing.

#### `POST /api/system-action` or `POST /system`
Executes system maintenance operations.
- **Reboot**:
  ```json
  {"action": "reboot"}
  ```
- **Factory Reset** (runs `factory_reset.sh` on the Hub, the same script `tools/factory_reset.py` uses): reverts to the stock Logitech firmware (stock `rcS.local` and `/etc/network/interfaces` back) and deletes `/data/harmony` (devices, activities, button maps, accounts and tokens, MQTT settings, Bluetooth keys), the Wi-Fi configuration and SSH access, then reboots. The stock firmware's own data stays. Save a backup first:
  ```json
  {"action": "factory_reset", "confirm": "RESET"}
  ```
  Answers `500` when the reset script is not installed where the Hub runs it.
- **MQTT Direct Publish**:
  ```json
  {
    "action": "mqtt_publish",
    "topic": "harmony/hub/custom",
    "payload": "{\"test\":1}"
  }
  ```
- **Authentication**: `{"action": "auth", "authEnabled": true, "authUsername": "admin", "authPassword": "..."}`
  sets the first admin account; a request that turns authentication on also answers `auth_enabled`
  and `to_check`, the accounts and access tokens to check (11.1).

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
  | `auth` | `1` when authentication is enabled, else `0` |
  | `mqtt` | `1` when MQTT is enabled and a broker host is set, else `0` |
  | `topic` | MQTT base topic, only when `mqtt=1` |
  | `legacy` | `1` when the Logitech protocol shim (1.12) is enabled, else `0` |
  | `name` | raw `hub_name`, for display |

### 1.12 Logitech protocol compatibility (port 8088, SSDP)
With `legacy_enabled` on (capability flag `legacy`), `hub_daemon` speaks the stock Logitech Hub
protocol on port 8088, as Home Assistant's core `harmony` integration (aioharmony 1.0.8) and other
stock clients expect, and answers SSDP searches so the core integration discovers the Hub. Off by
default. Port 8088 is never authenticated: the stock protocol has no authentication, so enable it
only on a trusted network. The daemon's own API on 8089 (sections 1 to 11) is unchanged; both
integrations can be used at the same time.

**Authentication on 8089 does not cover 8088.** With the shim on, any client on the LAN can read the
inventory, start and stop activities and send any device command through 8088 without credentials,
even when authentication is enabled for 8089. Leave `legacy_enabled` off unless the core `harmony`
integration is needed.

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
  | `harmony.activityengine?runactivity` (`activityId`, `"-1"` = power off) | `{}` with `code` 200 at once (`500` "Unknown activity"), then the start sequence below. A request while a transition runs is acknowledged the same way and queued (REST 4.3); its frames follow when it starts |
  | `...?holdAction` (`status` `press` or `release`; any other value, `hold` included, counts as a new press; `action` with `::` for `:`) | no reply on success; `code` 500 reply on error. Same press, hold and release path as `send_command` on 8089 |
  | `harmony.engine?changeChannel` (`channel` digits) | `{}`; the digits go to the first device of the running activity with digit commands, 200 ms apart; `500` without one |
  | `setup.sync?None` | `{}` |
  | `vnd.logitech.connect/vnd.logitech.statedigest?get` | the digest: `activityId`, `activityStatus` (0 idle, 1 starting, 2 running, 3 stopping), `configVersion` (changes when the inventory changes), `syncStatus` 0, `hubSwVersion`, `runningActivityList`, and the other stock fields |
  | `connect.discoveryinfo?get` | `friendlyName`, `remoteId`, `hubId`, `current_fw_version`, `protocolVersion` |

- Activity start sequence for a `runactivity` request whose `id` is M (the request's id, not the
  activity's): the `code` 200 reply; on the first
  step a `{"type":"connect.stateDigest?notify","data":{...activityStatus 1...}}` to every 8088
  client (`activityStatus` 0 when the transition is a power off, so `3` only shows in a
  `statedigest?get` reply); per step `{"cmd":"harmony.engine?startActivity","id":M,"code":100,
  "msg":"OK","data":{"done":n,"total":m}}` to the requester; on completion
  `{"cmd":"harmony.engine?startActivity","id":M,"code":200,"msg":"OK","data":{}}` to the requester, then `{"type":"harmony.engine?startActivityFinished",
  "data":{"activityId":"<id>","errorCode":"200","errorString":"OK"}}` and a final digest notify
  (`activityStatus` 2, or 0 after a power off) to every 8088 client. Switches made elsewhere (the
  remote, the web UI, 8089 clients, MQTT) produce the notifications only, so stock clients follow
  them. A config change produces a digest notify (new `configVersion`) so clients refetch.
- SSDP: `M-SEARCH` for `ssdp:all`, `upnp:rootdevice` or `urn:myharmony-com:device:harmony:1` on
  239.255.255.250:1900 is answered with `LOCATION: http://<ip>:8088/description`, `ST`, `USN:
  uuid:<uuid>::urn:myharmony-com:device:harmony:1`, `CACHE-CONTROL: max-age=1800`; `NOTIFY
  ssdp:alive` on enable and every 900 s, `ssdp:byebye` on disable and at shutdown. A search for
  `upnp:rootdevice` gets `USN: uuid:<uuid>::upnp:rootdevice`; one for `ssdp:all` is answered with
  the `urn:myharmony-com:device:harmony:1` `ST`. Only searches from a private address are
  answered; one from a public address gets no reply.

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
    "update_available": false,
    "updating": false,
    "update_state": "idle"
  }
  ```
  `update_state` is the state of the last `POST /api/update-apply` since the Hub started:
  - `running`: `update.sh apply` downloads and installs (`/tmp/hub_updating.flag` exists and the
    run started less than 30 minutes ago, counted on the uptime clock, which SNTP does not step).
    `update_step` is the last line of its output, untranslated, when it has one.
  - `restarting`: the update is installed and the Hub services restart (`init.sh restart`, no
    reboot); until then the old daemon answers. `update_target` is the version installed. The
    state ends when the new daemon starts, or after 10 minutes of uptime when none starts: from
    then on `current_version` tells whether the new
    version runs.
  - `failed`: the update ended with an error and nothing or not all was installed. `update_error`
    is its first error line (`[-] ...`, the cause), untranslated. Every failed run ends with
    `[-] The software update ended with exit code N.`, which is the only one when the run was
    killed.
  - `idle`: no update since the Hub started, or the update ended (compare `current_version` with
    the version before). An `update.sh` from a release before `update_state` writes no state (it
    runs the first update to this release, and reboots the Hub at its end): there the update
    reports `running` while it runs and `idle` after it, whether it failed or not.

  `updating` is `true` for `running` and `restarting`, here and in `GET /api/status`. While it is
  `true`, a second `POST /api/update-apply` gets `409` with `"status": "busy"` and a reboot through
  `POST /api/system-action` gets `409`.
  `fw_version` and `firmware` are the Logitech base firmware read from `/etc/version`.

### 2.2 Check for Updates
#### `GET /api/update-check` or `POST /api/update-check`
Checks for newer release bundles (also answered as `/api/update-check-state`).
- **Request Body** (optional, all fields optional):
  ```json
  {"repo": "https://raw.githubusercontent.com/iranl/harmony-ng/ota", "token": "ghp_example", "remote_version": "2.4.2"}
  ```
  - `repo`: the update source, the base URL under which `VERSION`, `manifest.json` and the bundle
    files are published. Default: the official `ota` branch. Only the characters `A-Z a-z 0-9 / . :
    - _ @ ~` are taken; any other value is ignored (the default is used).
  - `token`: sent as `Authorization: token <token>` with every download (a private repository).
    Letters, digits, `-` and `_` only, else ignored.
  - `remote_version` (or `version`): a version the client already knows; the Hub then stores it
    as the latest version and downloads nothing.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "current_version": "1.0.4",
    "remote_version": "1.0.4",
    "message": "Hub binaries and WebUI are up to date (v1.0.4).",
    "has_update": false,
    "update_available": false
  }
  ```

### 2.3 Apply Update
#### `POST /api/update-apply`
Executes `/data/harmony/update.sh` in the background and restarts services.
- **Request Body** (optional): `{"repo": "...", "token": "..."}`, the same fields and checks as
  `update-check`. `repo` chooses the server the new firmware is downloaded from, so like every
  route that changes the Hub this one is for admins once authentication is on (11.5). With
  authentication off (the default after install) any client on the LAN may call it.
- **Signed releases only**: the update source (the official `ota` branch, or `repo`) must publish `manifest.json` and `manifest.json.sig`, the Ed25519 signature of the manifest by a release key built into the Hub firmware (`payload/source/ota_keys.h`). `update.sh` checks the signature with `hub_util ota-verify`, takes the version from the signed manifest, and installs a file only when its SHA-256 matches the manifest. Without a valid signature nothing is changed, whatever server `repo` names. Downloads use `hub_fetch`, which checks the server certificate against the CA bundle (`/data/harmony/cacert.pem`); a Hub without `hub_fetch` (the first update from an older release) or without the bundle falls back to BusyBox `wget`, which checks no certificate. The signature check does not depend on TLS either way. When the certificate check fails, a file the signature covers (the manifest, its signature, the package and the files checked against the manifest) is fetched again without it and without the token of a private source, and the signature decides; the version check (`VERSION` files, the GitHub release list) is not fetched that way, since nothing signs it, and fails instead.
- **Services restart, no reboot**: once every file is installed, `update.sh` restarts the Hub services with `init.sh restart`, as `build/deploy_hub.py` does by default. An update has no kernel modules. Waiting for the next boot: a new `rcS.local`, the running SSH server (it keeps its old binary) and the USB port mode chosen at boot. `update.sh` removes what a reboot used to clear and the new daemon would reuse: the icons extracted from the old `icons.tar.gz` (`/tmp/www/assets/icons`) and a remote image kept for a resume (`/tmp/remote_ui.ota.gz`, `/tmp/remote_manifest.json*`). Without an executable `init.sh` it reboots the Hub (`safe_reboot.sh`) instead.
- **Not during an activity switch**: the update ends with a services restart, which must not cut an activity switch in the middle. While a switch runs, a request is queued behind it, or the steps after the switch point are still being sent, the request is refused with **`409 Conflict`** and starts nothing:
  ```json
  {"ok": false, "status": "activity_switch", "error": "An activity switch is running: start the update again when it has finished",
   "busy": true, "sending": true, "queued": null}
  ```
  Send it again when `GET /api/activity` shows `busy` and `sending` false and `queued` null. A switch that starts while the update downloads holds the restart: `update.sh` waits until `GET /api/activity` shows no switch twice in a row, 1 s apart (at most 10 minutes, then it restarts anyway, the new files being installed already). A query that fails counts as unknown: `update.sh` keeps asking, and restarts only when `hub_daemon` is not running or has not answered for 60 s. Both limits are real time from `/proc/uptime` (a query can take 3 s; a clock set by SNTP during the wait does not end it). Not covered: the fallback without an executable `update.sh` (`init.sh restart` 1 s after the request, so only the 409 at the request protects it); the first update to this version, which the old `update.sh` runs (the wait starts with the update after it); a switch started in the second between the last idle answer and the restart; `build/deploy_hub.py` and `POST /api/system-action` `reboot`, which do not look at a running switch.
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "status": "applying",
    "message": "Software update triggered. Hub services downloading and applying in background."
  }
  ```
- **Response `409 Conflict`**: `{"ok": false, "status": "busy", "error": "A software update is already running"}` while an update started earlier is still running or the services restart after it (`update_state` `running` or `restarting`, 2.1). `update.sh apply` also runs only once at a time.

---

## 3. Inventory & Device Management

Device, command and activity names (and a device's manufacturer and model) never contain `<`, `>` or control characters: the Hub drops them when it loads or saves the configuration, so a name cannot become markup in a client that shows it without escaping. Every other character is kept.

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
        "manufacturer": "LG",
        "model": "OLED65C2",
        "type": "Television",
        "controlPort": 7,
        "interDeviceDelay": 500,
        "interKeyDelay": 100,
        "holdInterKeyDelay": 200,
        "defaultInterDeviceDelay": 500,
        "powerOnDelay": 1500,
        "isPowerAlwaysOn": false,
        "powerOnSequence": [],
        "powerOffSequence": [],
        "commands": [
          {"name": "PowerToggle", "keyCode": "0000 006d ...", "raw": "0000 006d ...", "port": 7, "repeats": 3}
        ]
      }
    ]
  }
  ```
  The object is `devices.json` as the Hub stores it. Every device has the fields above, and `icon`
  when one was set. By kind of device:
  - infrared (every `type` that is not Bluetooth or MQTT): `controlPort` (the IR port mask, 0 to 7),
    `pressMinRepeats` when set (3.2), and `repeats` in each command. A command has `port` (1 to 7) only when it has its own Emitter
    Port, which a send uses instead of `controlPort`. `"commandPorts": 1` at the top marks a file
    whose command ports were set that way: in a file without it, every command's `port` is a copy
    of its device's port that no send used, and the Hub drops those once at load;
  - Bluetooth (`type` `BT`, `BTHID` or a type that contains `Bluetooth`): no `controlPort`,
    commands without `port` and `repeats`; instead `bdaddr`, `btAutoReconnect`,
    `btReconnectDurationSec`, `btReconnectDelaySec`, and `btTriggerDeviceId` and
    `btTriggerCommandName` when set. A device of another kind that was linked to a Bluetooth host
    (`POST /api/bt-link-device`, 6.5) has `bdaddr` too;
  - MQTT (`type` `MQTT`, or `mqttEnabled` true, see 8.1 "MQTT-type devices"): no `controlPort`,
    commands without `port` and `repeats`; instead `"mqttEnabled": true`, `mqttTopic` and
    `mqttPulseMs`. A device never has `controlPort` and `mqttEnabled` together;
  - HTTP and TCP (`type` exactly `HTTP` or `TCP`, see 3.9): no `controlPort`, commands without
    `port` and `repeats`; instead `netHost`, `netPort`, `netTls`, `netTlsVerify`, `netTimeoutMs`,
    and for HTTP `httpHeaders`, `httpAuthType`, `httpUser`. `httpPassword` and `httpToken` are
    always sent as `""`, with `"hasHttpPassword": true` / `"hasHttpToken": true` when one is
    stored. Commands carry `keyCode` `HTTP:<name>` with `httpMethod`, `httpPath`, `httpBody`,
    `httpContentType`, or `TCP:<name>` with `tcpPayload` and `tcpFormat`.

  A command carries its code as `keyCode` and as `raw`, and `"isMacro": true` when it is a macro.
- **Response `404`**: `{"ok": false, "error": "DeviceList not found"}` when the list cannot be loaded.

#### `GET /api/devices`
Returns `devices.json` as stored (`{"devices": [...]}`), `404` when it cannot be read. The
credentials of HTTP devices are hidden as in `GET /api/inventory`.

### 3.2 Save Device
#### `POST /api/device-save` or `POST /ir/device`
Creates or updates a device definition. A field that is left out keeps its stored value; a field
with another name than those below is ignored without an error. Also read: `icon`, `powerOnDelay`,
`isPowerAlwaysOn`, `hideOnRemote` (true leaves the device out of the Elite's device pages and
activity cards at the next config push; its commands still work from activity screen buttons
and keys), `hideInDevicesTab` (true leaves the device out of the Devices tab of both web UIs;
the remote and the device editor still list it), `powerOnSequence`, `powerOffSequence`, and for a Bluetooth device `bdaddr`
(older name `btHost`), `btAutoReconnect` (default on), `btReconnectDurationSec`,
`btReconnectDelaySec`, `btTriggerDeviceId` and `btTriggerCommandName`. The MQTT pulse length is
`mqttPulseMs` (milliseconds, default 1000); only `POST /api/device-mqtt-test` also takes `mqttPulse`.
Other aliases of this route: `POST /api/new-device`, `/ir/new-device`, `/ir/device-mqtt`,
`/ir/device-power`.

Without `deviceId` (also accepted as `id`) the Hub creates a device and gives it the lowest free
number as id; the answer carries it. A new device without `controlPort` gets `7`, without `type`
gets `"Generic"`, and without `name` is named after its id. `powerOnDelay` is 0 to 120000 ms
(default 1500; a larger value is lowered to 120000).

`"mqttEnabled": true` (or `type` `MQTT`) makes the device an MQTT-type device (8.1): it has no
`controlPort`, its commands are stored without `port` and `repeats`, and they go out as MQTT pulses
on `mqttTopic` (default `{base_topic}/button/{device}/{command}`). Leave the MQTT fields out for an
infrared or Bluetooth device.

`"plugin": "<id>"` (or `type` `plugin:<id>`) makes the device a plugin device (docs/PLUGINS.md): no
`controlPort`, its commands go to the plugin, and `config` (an object, or that object as a JSON
string) holds the values of the plugin's `config_fields`. The id must be a safe plugin id (letters,
digits, `-`, `_`), or the save is refused with `400`. A device's `type` text never makes it a
plugin device, even when it matches a plugin's name. A top-level key is copied into `config` only
for a plugin device and only when the plugin's `config_fields` declare it. The plugin choice comes
before the MQTT and Bluetooth guesses from `type` and `manufacturer`. A save without `plugin` keeps
the stored one; `"plugin": ""` makes the device a non-plugin device again. A command of a plugin
device may carry `irKeyCode` (an infrared code, as an IR device's `keyCode`) and `irWhen` (the key of
a boolean `config` field, letters, digits, `_` or `-`, at most 63): while `config[irWhen]` is on
(`true`, a number other than 0, `"true"` or `"1"`) the command is sent as infrared, as a command of
an IR device (its `port` 1 to 7, else all blasters; `repeats`, 3 when missing), instead of going to
the plugin (docs/PLUGINS.md). An invalid `irKeyCode` or `irWhen` is left out. In a `commands` list,
each of the two keys a command leaves out keeps the value the device's command of that name had
(so a save that changes only `irKeyCode` keeps `irWhen`), `null` or `""` removes it, and a command the device did not have gets those of the plugin manifest's
`default_commands` entry of the same name. A command with an `irKeyCode` keeps `port` and
`repeats`; other plugin commands store neither.

`type` `HTTP` or `TCP` makes the device an HTTP or TCP device (3.9). Its fields: `netHost` (host
name or IP address: letters, digits, `.`, `-`, `_`, `:`), `netPort` (1 to 65535; an HTTP device
without one uses 80, or 443 with TLS), `netTls` (bool), `netTlsVerify` (`verify`, the default,
`pin` or `insecure`), `netTimeoutMs` (500 to 15000, default 3000), and for HTTP `httpHeaders`
(at most 8 `{"name", "value"}`; not `Host`, `Connection`, `Content-Length`,
`Transfer-Encoding`), `httpAuthType` (`none`, `basic` or `bearer`), `httpUser`, `httpPassword`,
`httpToken`. An empty or missing `httpPassword` / `httpToken` keeps the stored one, but only while
`netHost`, `netPort` and `netTls` stay the same: a save that changes one of them drops the stored
secret (send it again). Only the credentials of the chosen `httpAuthType` are kept. Header values
and `httpPath` (with its query) are not secret: `GET` returns them as stored, so put a key in
`httpToken` or `httpPassword`, not in a header or the path. `netHost` cannot be the Hub itself
(`localhost`, `127.x`, `::1`) or start with `-`. With `netTlsVerify` `verify` the device must
speak TLS 1.2; `pin` and `insecure` also accept TLS 1.0 and 1.1. A host, port, timeout, verify mode, auth type
or header that cannot be used (a header name with a space, a value with CR or LF) is refused with
`400`. The HTTP/TCP choice comes before the MQTT and Bluetooth guesses (only a plugin wins over
it). The command fields are saved with `POST /api/command-save` (3.5) or a `commands` list.
- **Request Body**:
  ```json
  {
    "deviceId": "70321456",
    "name": "LG OLED TV",
    "type": "Television",
    "manufacturer": "LG",
    "model": "OLED65C2",
    "controlPort": 7,
    "interDeviceDelay": 500,
    "interKeyDelay": 100,
    "holdInterKeyDelay": 200
  }
  ```
- `interDeviceDelay` (wait before a command to this device when the previous one went to another
  device), `interKeyDelay` (between two different commands to it) and `holdInterKeyDelay` (between
  repeats of the same command) are milliseconds from 0 to 10000. The sender waits them out before
  the command, so a save with a value outside that range is refused with `400` and nothing is
  changed; a larger value already in `devices.json` is lowered to 10000 when the file is loaded.
  The three waits are for commands a program sends (API, MQTT, activity sequences); a key pressed
  on a remote is sent without them.
- `pressMinRepeats` (infrared devices): how many frames a press sends after the first, as the
  Logitech account's `PressMinRepeats` (5.1, "Frames of a press"). 0 to 20 (a number or numeric
  text; a larger value is lowered to 20). `null`, `""` or a negative number removes it, and the
  Hub then uses 3, as a stock Hub does for an account without the value; left out, the stored
  value is kept. A device without it in `devices.json` gets none when the file is loaded (no
  default is written); a value that is not a count is dropped there.
- **Response `200 OK`**: `{"ok": true, "deviceId": "70321456", "id": "70321456"}` (both carry the device's id, the new one for a device the Hub just created)
- **Response `400`**: `{"ok": false}` when a timing field (the three delays, `powerOnDelay`, `btReconnectDurationSec`, `btReconnectDelaySec`) is not a number (or numeric string) or a
  delay is out of range, or `pressMinRepeats` is text that is not a number.
- **Response `500`**: `{"ok": false, "error": "..."}` when `devices.json` could not be written (flash
  full); the stored devices are kept.

### 3.3 Delete Device
#### `POST /api/device-delete` or `POST /ir/delete-device`
Deletes a device that nothing uses any more. References are not removed: while an activity (its
`startSequence`, `stopSequence`, `deviceIds` or any other step), another device's
`btTriggerDeviceId`, or a key binding of a remote map (`remote_slot1_map.json` to
`remote_slot6_map.json`, `bt_remote_map.json`) names the device, the delete is refused and nothing
changes. Not counted, so a device never becomes impossible to delete: the map's own page for the
device (`devices/<id>`, removed from the maps together with the device, so a new device given the
same id starts without it), map sections of activities and pages of devices that no longer exist,
and the map of a touchscreen slot that has no remote: a slot counts when `hub_rf` reports it active
(`GET /api/rf/status`, asked for at most 1.5 s), or, not active, when its
`remote_slot<N>_version.json` still names a remote (`remote_id` or `skin`; an unpair clears both);
when `hub_rf` does not answer, when that file exists. Any other slot does not count. Deleting an activity
(`POST /api/activity-delete`) removes its sections (`activities/<id>`) from the three maps; a device
delete removes its page and pages of devices deleted earlier. Both only once the configuration in
memory no longer has the activity or device. The maps are written compact (as the editors save
them, so a large map stays under the 200000 bytes `GET /api/rf-mapping` reads), to a temporary
file compared with the map once more right before the rename: an editor save in the meantime is
read again instead of being overwritten (one in the last instant before the rename can still be
lost: the editors take no lock). The web remote layouts (`web_remote_layouts.json`) are not
scanned: the web remote drops buttons of a device that no longer exists by itself.
- **Request Body**: `{"deviceId": "70321456"}`. `DELETE` is accepted as well as `POST`, `id` as
  well as `deviceId` (a string or a number), and the body may also be form fields
  (`deviceId=70321456`). Without a body the id comes from the query (`?deviceId=` or `?id=`).
- **Response `200 OK`**: `{"ok": true}` (`{"ok": false}` for an unknown device)
- **Response `409 Conflict`**: the device is in use; `users` lists what uses it: activities (with
  `name` and the `fields` that name the device), devices whose Bluetooth trigger names it, and per
  remote map the key bindings (`keys`: paths, at most 32, `count`: the total) and `groups`: the
  bindings counted per section, with its name (`type` `activity`, `device_page` for another device's
  page, `no_activity` for the default page `-1`/`default`, `other`). A screen button counts as a key
  of its activity. Both web UIs show this as text, e.g. "Activities: Shield TV, Xbox (device list,
  start steps)", "Remote slot 1: 46 keys in TV (DTT)":
  ```json
  {
    "ok": false,
    "error": "Device is in use: remove it from these activities, devices and remote keys first",
    "deviceId": "1",
    "users": [
      {"type": "activity", "id": "10", "name": "Watch TV", "fields": ["startSequence", "stopSequence"]},
      {"type": "device", "id": "5", "name": "Receiver", "field": "btTriggerDeviceId"},
      {"type": "remote_map", "slot": 1, "count": 2, "keys": ["activities/10/VolumeUp", "activities/10/VolumeDown"],
       "groups": [{"type": "activity", "id": "10", "name": "Watch TV", "count": 2}]},
      {"type": "bt_remote_map", "count": 1, "keys": ["activities/10/OK"],
       "groups": [{"type": "activity", "id": "10", "name": "Watch TV", "count": 1}]}
    ]
  }
  ```

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
        "keyCode": "0000 006d 0022 0002 0155 00aa ...",
        "raw": "0000 006d 0022 0002 0155 00aa ...",
        "port": 7,
        "repeats": 3
      }
    ]
  }
  ```
  The commands are those of 3.1 (`port` and `repeats` only for an infrared device). `id` is
  accepted for `deviceId`. An unknown device, or no device id, answers
  `{"ok": true, "commands": []}`.

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
  - `deviceId` (also `id`) and `name` are required. `raw` (also `code`) is the code; it is stored
    as the command's `keyCode` and `raw`.
  - `oldName`: the present name of a command to rename (to `name`).
  - `script`: a Bluetooth macro; it is stored as `BT:SCRIPT:<script>` with `"isMacro": true` and
    replaces `raw`.
  - `repeats` (1 to 20, higher is 20) and `port` are numbers or, from a form post, numeric text.
    `repeats` left out keeps the stored value (3 for a new command). It is stored and kept for
    clients, but it does not set how many frames a press sends: the device's `pressMinRepeats`
    does (3.2, 5.1), as on a stock Hub, where the trailing `:3` of a keycode is not read either.
  - `port`: the command's own IR port mask, 1 to 7, used by a send instead of the device's
    `controlPort`, also when that is `0` (No Blasters). `0` (Default (Device Setting)) removes it, so the device's port applies; left
    out, the stored one is kept. For a Bluetooth or MQTT device, and for a Bluetooth (`HID:`,
    `G:HID`) or MQTT (`G:MQTT`) code, `port` and `repeats` are not stored. 3.6 treats both fields
    the same way.
  - `irKeyCode` and `irWhen` (plugin devices only, 3.4): the infrared alternative of the command.
    A string sets one (an invalid one is left out), `null` or `""` removes it, left out the stored
    one is kept; a new command without them gets those of the plugin manifest's `default_commands`
    entry of the same name. A plugin command with an `irKeyCode` stores `port` and `repeats` like
    an IR command; other plugin commands store neither. 3.6 treats both fields the same way.
- **Response `200 OK`**: `{"ok": true}`; `{"ok": false}` (also `200`) when the device does not
  exist or `deviceId` or `name` is missing.
- **Response `500`**: `{"ok": false, "error": "..."}` when `devices.json` could not be written (flash
  full); the stored devices are kept.

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
- **Response `200 OK`**: `{"ok": true, "count": 2}` (`count`: the commands stored; an entry
  without `name` or without `raw` (or `code`) is skipped). `{"ok": false, "count": 0}` (also
  `200`) when the device does not exist or `commands` is not a list.
- **Response `500`**: `{"ok": false, "count": 0, "error": "..."}` when `devices.json` could not be
  written (flash full); nothing was stored.

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
  `name` is accepted for `command`, `id` for `deviceId`.
- **Response `200 OK`**: `{"ok": true}`; `{"ok": false}` (also `200`) when the device or the
  command does not exist.
- **Response `500`**: `{"ok": false, "error": "..."}` when `devices.json` could not be written (flash
  full); the stored devices are kept.

### 3.8 Device MQTT Test
Publishes the MQTT pulse of a device once, the way a command to an MQTT-type device does (8.1,
"MQTT-type devices"): `"1"` on the resolved topic, then `"0"` after the pulse.

#### `POST /api/device-mqtt-test`
- **Request Body**: `{"deviceId": "3", "device": "Plug", "command": "PowerToggle", "topic": "{base_topic}/button/{device}/{command}", "pulseMs": 1000}`.
  `topic` (also `mqttTopic`) overrides the device's `mqttTopic`. `pulseMs` (also `mqttPulse`)
  defaults to 1000: the device's stored `mqttPulseMs` is not used by this route. `device` is the
  text put in place of `{device}`; it defaults to the device id (`deviceId`, also `id`), not the
  device's name. `command` defaults to `PowerToggle`.
- **Response `200 OK`**: `{"ok": true, "pulseMs": 1000, "topic": "harmony/hub/button/Plug/PowerToggle", "message": "MQTT test publish sent"}`.
- **Response `503`**: `{"ok": false, "error": "MQTT broker not connected"}`: MQTT is disabled or the broker is not connected.

### 3.9 HTTP and TCP Devices
A command to an HTTP device is one request to `netHost:netPort` (HTTPS with `netTls`); a command
to a TCP device opens a connection, sends `tcpPayload` and closes it. Both run on one worker
thread in the order they come: the key path never waits for the device. A press and each repeat
of a held key send the command; a release sends nothing. In `httpPath` (percent-encoded),
`httpBody` and `tcpPayload`, `{device}` and `{command}` are replaced with the device and command
name. `tcpFormat` `text` decodes `\r`, `\n`, `\t`, `\\` and `\xHH`; `hex` sends the hex digits as
bytes (spaces and `:` between them are ignored). An HTTP request has `Connection: close`, the
device's headers, `Authorization: Basic ...` or `Bearer ...`, and a `Content-Type`
(`httpContentType`, else `application/json` for a body starting with `{` or `[`, else
`text/plain`). Redirects are not followed.

TLS (BearSSL, TLS 1.0 to 1.2) checks the certificate by `netTlsVerify`:
- `verify`: it must chain to a CA in `/data/harmony/cacert.pem` and name `netHost` (a certificate
  for an IP address is not supported: use `pin` for one);
- `pin`: the first key the device shows is kept in `config/tls_pins.json` (by `host:port`), and
  a connection with another key is refused until the pin is forgotten (below);
- `insecure`: any certificate (encryption only).

A clock that is not set yet (before the build date) is taken as the build date for the
certificate dates.

#### `POST /api/device-net-test`
Runs a command of an HTTP or TCP device now and waits for it (up to about 4 times
`netTimeoutMs`). Without `command`: the connection and TLS handshake only.
- **Request Body**: `{"id": "50", "command": "Power"}` (`deviceId` is accepted as well).
- **Response `200 OK`**: `{"ok": true, "status": 200, "response": "{\"done\":true}", "tlsFingerprint": "9f86...", "pinned": true}`.
  `ok` is false with an `error` when the request failed or answered 400 or more. `response` is
  the start of the answer (512 bytes at most; for TCP the first data the device sent back).
  `tlsFingerprint` is the SHA-256 of the server's raw public key (the key bytes of the
  certificate, not the SubjectPublicKeyInfo, so not what `openssl x509 -pubkey` hashes to);
  `pinned` is set when this run pinned it. A command from a remote key is sent once per press (a
  held key sends no repeats), and one still queued after 2 s, or for a device that timed out in
  the last 10 s, is dropped (`dropped:` in the log). Only a live key is dropped: a person's press
  that arrives late is wrong, while a command of an automation must arrive. Live keys are the
  hard keys, the screen buttons and the device pages of a touchscreen remote (hub_rf), the keys
  of a Bluetooth remote, the remote views of both web UIs, and any `ir-send` or `send_command`
  with `"live": true` (5.1, 10.3). Activity steps and every other `ir-send`, MQTT and WebSocket
  send (Home Assistant, scripts, the Logitech protocol) wait in the queue and go out in order. A
  send answers `ok` when it is queued, also one that is dropped later; a lone `hold` or `repeat`
  answers `ok` and sends nothing. The queue holds 16 sends. When it is full, a send is refused:
  `ir-send` answers `503` `"send queue full"`, an activity step is lost and logged once (`[!] Orch:
  activity '<id>' step '<step>' lost`), and a live key takes the place of the oldest live key
  that is over 1 s old (late for a key pressed now). The host is resolved once per send; a host
  with any address on the Hub itself (loopback) is refused, and the connection goes to the
  addresses that were checked.
- **Response `400`**: not an HTTP or TCP device. **`404`**: no device with that id, or no
  command of that name.

#### `POST /api/device-net-pin-forget`
- **Request Body**: `{"id": "50"}`.
- **Response `200 OK`**: `{"ok": true, "removed": true}` (`removed` false when no key was
  pinned for the device's host and port).

---

## 4. Activities & Orchestration

### 4.1 List Activities
#### `GET /api/activities`
Returns all configured activities, active activity ID, and state enum. The list is in Display
Order (`order`, lowest first; equal orders keep their place). The Hub keeps `activities.json` in
that order (written back once at load when it was not), so the remotes and MQTT list the
activities the same way; an activity without `order` gets one after the highest (99 at most).
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
        "deviceIds": ["70321456"],
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
    "queued": null,
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
- While a transition runs, the request is queued and starts when that transition has completed:
  the reply is `{"success": true, "state": "queued"}`. One request is queued at a time, the latest
  replaces an earlier one, and a request for the running transition's own target drops the queue.
  `GET /api/activity` shows the queued id in `queued` (`null` when none), and so does the
  `activity_state` event (10.8), sent whenever the queue changes.
- `id` is accepted for `activity`. The body must be JSON.
- **With `"dry_run": true`** nothing is sent and nothing changes: the answer is the plan of the
  switch instead of `success` and `state`. `type` is the step type as a number (`0` a delay, `2` a
  device command, `4` the point where the activity counts as switched), `delayMs` the time the
  step takes, `switchedDurationMs` the time until that point, `backgroundSteps` the steps of a
  sequence that follow its `Switched` step and `trueEndMs` the time until the last of them:
  ```json
  {
    "target": "123456",
    "switchedDurationMs": 700,
    "trueEndMs": 700,
    "steps": [
      {"description": "Starting: PowerOn", "type": 2, "deviceId": "70321456", "command": "PowerOn", "delayMs": 250},
      {"description": "Starting: Delay 200ms", "type": 0, "delayMs": 200},
      {"description": "Activity switched", "type": 4, "stepType": "Switched", "delayMs": 0}
    ],
    "backgroundSteps": []
  }
  ```
  A dry run is not queued: while a transition runs it answers `409` (`"Interlock busy"`).
- **Error `400`**: `{"ok": false, "error": "Missing activity parameter"}` without `activity`.
- **Error `500`**: unknown activity (nothing is queued for it either), or the transition could not
  be built: `{"ok": false, "error": "Could not build the transition"}`. Errors of this route and of
  4.4 have this `ok` / `error` form; only the successes use `success`.

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
- While a transition runs, the power off is queued like a start (4.3):
  `{"success": true, "state": "queued"}`.
- With `"dry_run": true` the answer is the plan, as in 4.3 (`"target": "-1"`); while a transition
  runs a dry run answers `409` (`"busy"`).
- **Error `500`**: `{"ok": false, "error": "Could not build the transition"}`.

### 4.5 Reset Interlock / Cancel Activity
#### `POST /api/activity-cancel` or `POST /api/interlock-reset` or `POST /api/activity-reset`
Forcibly clears stuck orchestrator step queues: the steps not sent yet and the queued request (4.3)
are dropped, and an infrared send in progress is stopped. The Hub stays in the activity it was in:
a cancelled start goes back to the activity before it (running, or idle when there was none); only
a cancelled power off ends as PowerOff (`-1`, idle).
- **Response `200 OK`**: `{"success": true, "message": "Activity cancelled / interlock reset"}`

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
  - `id` (also `activityId`): the activity to change. Without it a new activity is created with
    the lowest free number from 1 to 99 as id. The answer does not carry the new id: read
    `GET /api/activities` again.
  - Defaults of a new activity: `name` `"Custom Activity"`, `type` `"Custom"`, `icon` `"Default"`,
    `order` after the highest one (99 at most), so it is listed last.
  - `order` left out (or empty) keeps the stored Display Order of an existing activity.
  - `deviceIds` (also `devices`): the devices of the activity, as a list or as text separated by
    commas. When left out, the list is built from the devices named in the two sequences.
  - Steps are stored in one form, each with `type`, `command` and `deviceId`. Types: `Command` (a
    device command, with an optional `delay` after it), `BTCommand` (a Bluetooth command: also any
    command that starts with `HID:` or `BT:`), `Delay` (`delay` in ms; 1000 when left out) and
    `Switched` (the point from which the activity counts as switched, see 4.2 `busy`; the steps
    after it are sent in the background). `delay` is 0 to 120000 ms (a larger value is lowered to
    120000). A step without a command and without a delay is dropped.
  - A sequence may also be given as text, one step per line: `Type|deviceId|command|delay`.
- **Response `200 OK`**: `{"ok": true, "message": "Activity saved successfully"}`
- **Response `400`**: `{"ok": false, "message": "Failed to save activity"}` when the body is empty.
- **Response `500`**: `{"ok": false, "error": "..."}` when `activities.json` could not be written
  (flash full); the stored activities are kept.

### 4.7 Delete Activity
#### `POST /api/activity-delete`
Deletes an activity by ID.
- **Request Body**: `{"id": "123456"}`
- **Response `200 OK`**: `{"ok": true}`; `{"ok": false}` (also `200`) when `id` is missing, no
  activity has that id, or `activities.json` could not be written.
- **Error `409 Conflict`**: the activity is running (its power off is built from its definition):
  power off or start another activity first. If the activity file loses the running activity in
  another way (a restore, a reload), the Hub reports PowerOff (`-1`, idle).

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
- **Response `200 OK`**: `{"ok": true, "executed": 2}`. The steps run before the answer is sent.
  `sequence` is accepted for `steps`, also as text lines (4.6). Each delay is cut to 3000 ms. A
  command step that also has a `delay` counts twice in `executed`.
- **Error `400`**: more than 64 steps (`"Too many steps (limit 64)"`); nothing is run.

### 4.9 Activity Progress Webhook
#### `POST /api/activity-progress`
Internal step progress notification: sends an `activity_progress` event (10.9) to the WebSocket
clients and MQTT. The body is a JSON object; only these fields are taken: `activity` (or
`activityId`), `state` (name or number, or `stateCode`), `step` (or `currentStep`), `total` (or
`totalSteps`), `desc` (or `stepDesc`). Anything else in the body is not sent.
- **Response `200 OK`**: `{"ok": true}`
- **Error `400`**: the body is not a JSON object.

### 4.10 Reload / Sync Config
#### `POST /api/sync` or `POST /api/reload`
Forces daemon to reload JSON configs from flash, and marks the remotes' configuration as waiting
for a push (`sync_pending` in the RF status, 7.1). The push itself is `/api/rf/push-config` (7.4).
- **Response `200 OK`**: `{"ok": true, "message": "Configuration reloaded and synchronized"}`

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
  With `dry_run`, nothing is sent and `preview` holds the steps that would be sent (the plan
  object of 4.3); a dry run does not need the activity to be running.
- **Error `409 Conflict`**: `Interlock busy`, or `Activity is not running`.
- **Error `500`**: `Could not build the activity's commands`.
- Errors of both Help routes have the form `{"ok": false, "error": "..."}`.

#### `POST /api/activity/help/answer`
Answers the question for one device of the last Help. `"on": false` sends that device's power toggle; `"on": true` sends nothing. Each device takes one answer.
- **Request Body**: `{"device": "4", "on": false}`
- **Response `200 OK`**: `{"success": true, "sent": true}`
- **Error `404 Not Found`**: the device is not waiting for a Help answer.
- **Error `409 Conflict`**: `The Help question is no longer valid`: the Hub has left the activity
  of that Help, or the Help is older than 300 s.

---

## 5. Hardware & IR Control

### 5.1 Send IR Command
#### `POST /api/ir-send` or `POST /ir/send`
Transmits an IR command either by device ID and one command name, or raw Pronto hex. `command`
(also accepted as `name`) must be one string: a list answers `400`. The reply also carries
`deviceId` and `command`; a failed send answers `500` with `"error": "IR transmit failed"`, and a
command of an HTTP or TCP device that finds their send queue full `503` `"send queue full"`.
- **Request by Device Command**:
  ```json
  {
    "deviceId": "70321456",
    "command": "PowerToggle"
  }
  ```
- **Value (plugin commands)**: `"value": 42` (an integer, or a string holding one; `1.9`,
  `"abc"` or `true` answers `400` "value must be an integer") reaches the plugin as the
  datagram's `value`. A plugin command with `"range": [min, max]` refuses a value outside it with
  `400` and `"error": "value out of range"`, and needs one: without `value` it answers `400`
  "value required". Senders that have no value (remote keys, activity steps) cannot use such a
  command; the WebSocket and MQTT `send_command` take `"value"` for a single command. Other
  command types ignore the value and their `range`. `POST /api/command-save` and
  `/api/commands-batch` take `"range": [min, max]`, remove it with `null` or `[]`, and answer
  `400` for any other range. In `POST /api/device-save`, a command without a `range` key keeps
  the range the device's command of that name had, `null` or `[]` removes it (a removed range
  stays removed), and a command the device did not have gets the one of the plugin manifest's
  `default_commands` entry of the same name.
- **Lists, repeats, delays and holds are not taken by this route.** `num_repeats`, `delay_secs`
  and `hold_secs` are ignored here. They work with the `send_command` action of the WebSocket
  (10.3) and MQTT (8.1), which takes this form:
  ```json
  {
    "deviceId": "70321456",
    "command": ["VolumeUp", "VolumeDown"],
    "num_repeats": 2,
    "delay_secs": 0.25,
    "hold_secs": 0.0
  }
  ```
  - `command` / `commands`: Single command name or array of command names (up to 32).
  - `num_repeats`: Number of times to cycle through the command list (1-50, default: 1).
  - `delay_secs`: Delay in seconds between successive commands (default: 0.25s for multi-commands, 0s for single).
  - `hold_secs`: Button press hold duration in seconds before release (0.0 to 10.0s).
- **Held key (`"action"` with a device command)**: `press`, `hold` or `release`; `tap`, the
  default, is one full keystroke. For an infrared command a `press` starts the code and returns
  at once: the Hub sends the code and then repeats it in the code's own repeat format (its
  repeat sequence, or the whole frame when it has none) with no pause, as the device's own
  remote does, until the `release`. A `hold` sends nothing itself: it tells the Hub that the key
  is still down. With no `hold` and no `release` for 600 ms the Hub stops repeating by itself
  (a lost release cannot leave a key repeating), so a client that holds a key longer sends a
  `hold` at least every 300 ms, as the remote does. The repeating always ends after a whole
  frame, and the start of the code and the repeat parts of a press (below) always go out, also
  when the release comes sooner or the holds stop. A `tap`, each command of a list and each step
  of an activity is one press (below). Another infrared command ends a held key (it waits for the
  frames that must still go out). A `hold` or `release` for a key that
  is not held does nothing. For a Bluetooth command `press` is key down, `release` key up and
  `hold` nothing. The `hold_secs` of `send_command` (10.3, MQTT) sends the holds itself.
- **Frames of a press**: a press of a device's infrared command (a `tap`, a step, a command of a
  list, and the start of a held key) sends the code's start part, then N repeat parts, as a
  stock Hub does (its `irmanager.lua` and `hal`). N is the device's `pressMinRepeats` (3.2), 3
  when the device has none. The command's `repeats` and the keycode's trailing `:3` do not count.

  | Code | Frames of a press | N = 0 | N = 1 | N = 3 |
  |---|---|---|---|---|
  | empty start group, `G:<protocol>:()(code)():3` (most protocols; also Pronto with a repeat sequence only) | the frame max(N, 1) times | 1 | 1 | 3 |
  | start group, `G:Toshiba 32 Bit:(code)(Repeat)():3` (also Pronto with a once and a repeat sequence) | the frame, then N repeat parts (for NEC types the short repeat code) | frame | frame + 1 | frame + 3 |
  | Sony (`G:Sony <n> Bit:...`) | the frame max(N, 3) times: Sony receivers act only on a repeated frame | 3 | 3 | 3 |
  | learned code (`F<carrier>P...S...`) | once, whatever N (it holds every frame of the learned press, 5.2a) | 1 | 1 | 1 |

  A held key sends at least these frames and then repeats until the release. The routes that
  send a code as given send it once: a code in `pronto`, `code` or `raw` of this route (below;
  also a `G:` keycode), `POST /api/pronto-blast` (5.2), `POST /api/ir-test-learned` (5.2a),
  `POST /api/ir-raw` (5.4), and an activity step that names a raw or Pronto code instead of a
  command.
- **Request by Raw Pronto**:
  ```json
  {
    "pronto": "0000 006d 0022 0002 0155 00aa ...",
    "port": 7,
    "repeats": 3
  }
  ```
  - `port`: Port bitmask (`1` = Front Hub blaster, `2` = Jack 1, `4` = Jack 2, `7` = All emitters). Default 7.
  - `repeats`: Repeat count (default: 1).
  - `code` or `raw` is accepted for `pronto`. A Harmony code (`G:<protocol>:...`, or the raw timing
    form `F<carrier>P...S...` of 5.2a) given here is sent as that keycode.
- **`live`** (optional, default `false`): `true` marks a person's press (a remote view, a
  Bluetooth remote). It is sent like a key from a remote: without the gaps between commands
  (`interKeyDelay`, `interDeviceDelay`), and for an HTTP or TCP device it is dropped when it
  waited too long in the send queue, and may take the place of a live key over 1 s old when the
  queue is full (3.9). Leave it out for automations: their commands wait their turn and go out.
- **Other names**: `device_id`, `device` or `id` for `deviceId`; for `action`, `down` = `press`,
  `repeat` = `hold`, `up` = `release`.
- **Duplicate filter**: a second `tap` or `press` of the same command of the same device that
  arrives within 90 ms of the first is not sent, and is still answered `"ok": true`.
- **Response `200 OK`**: `{"ok": true, "deviceId": "70321456", "command": "PowerToggle"}` (`deviceId`
  and `command` as far as the request had them; a raw Pronto request gets `{"ok": true}`)
- **Error `400`**: `{"ok": false, "error": "Missing deviceId/command or pronto code"}` when the
  request has neither a device command nor a code.
- **Error `500`**: `{"ok": false, "deviceId": "70321456", "command": "PowerToggle", "error": "IR transmit failed"}`
  when the send failed: an unknown device or command, a code the encoder does not understand, or
  a transmit error (the text is the same for a Bluetooth or MQTT device).
- **Error `503`**: `{"ok": false, "deviceId": "50", "command": "Power", "error": "send queue full"}`
  when the command of an HTTP or TCP device finds their send queue (16 sends) full (3.9).

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
  The body must be JSON. `ports` defaults to 7 (all emitters), `repeats` to 3.
- **Response `200 OK`**: `{"success": true}`
- **Error `400`**: `{"ok": false, "error": "Missing pronto parameter"}`.
- **Error `500`**: `{"ok": false, "error": "IR send failed"}` (a code that is not Pronto, or a
  transmit error).

### 5.2a IR Learning (capture)
#### `POST /api/capture`
Listens on the Hub's IR receiver until a signal arrives or the timeout ends, then returns the whole press as a Harmony raw timing string (`F<carrier hex>P<pulse us hex>S<space us hex>...`, the carrier measured, each space between frames as measured, the last space the gap to the next frame). As the stock Hub's capture does, it records until 200 ms without light, at most 1023 mark/space pairs (2 s at most), and keeps every frame of the press; the string can be stored as a command's `raw` (`POST /ir/command`), and a tap of that command sends it once, so every frame goes out again (3.5). Learn with one short tap: a key held for a second becomes a code a second long. The sensor is short-range and often misses the start of a press: hold the original remote 5 to 8 cm from the Hub. A first frame shorter than the next one is dropped (the sensor cut it), and so is a last frame without quiet after it (cut by the limit).
- **Request Body** (optional): `{"timeout": 10}` in seconds, 1 to 30 (default 10). `?timeout=` works too, but is read only when the request has no body.
- **Response `200 OK`**:
  ```json
  {"ok": true, "raw": "F9470P...S...", "keycode": "F9470P...S...", "mode": "raw", "carrier_hz": 38000, "pulses": 26, "duration_us": 24000, "frames": 1}
  ```
  `pulses` is the number of timing intervals in `raw`, marks and spaces together (`P` and `S` entries); `duration_us` is their sum; `frames` the number of frames (a space of 20 ms or more ends one).
- **Learning while the capture mode (5.5) is on**: works. The receiver thread that serves the mode records the learned press for this request, and no `ir_received` event is sent for that press; the mode stays reported on (`ir_capture` true, no `ir_capture` event). A press that was already going on when the request came is not learned: the thread first waits until the line is quiet. With the mode off, the thread is started for the learn and stopped after it.
- **Hub IR sends during a learn** (activity steps, remote keys, `ir-send`, `ir-raw`) go out at once; they do not wait for the learn. The receiver sees the Hub's own blasters, so what it reads during a send and 100 ms after it is dropped, and a press that overlaps a send is not kept: the learn goes on waiting for the next press until its timeout. If that happened and nothing else was pressed, the `408` message says so.
- **One learn at a time**: a second request while one waits is refused with `409`.
- **Errors**: `408` no signal before the timeout, or only a flash that is not a code, `409` another capture is running, `503` no IR receiver, or the receiver stopped delivering data (the capture gives up after 2 s without data instead of hanging; a reboot clears a stuck receiver), or the Hub is shutting down, `500` other failures.

#### `POST /api/ir-test-learned`
Sends a captured code once through the IR ports of a device (its `controlPort`, default all).
- **Request Body**: `{"deviceId": "3", "keycode": "F9470P...S..."}` (`raw` or `code` are accepted for `keycode`).
- **Response `200 OK`**: `{"ok": true, "port": 7}`
- **Errors**: `400` missing code or a code the IR encoder does not understand, `500` transmit failure.

### 5.3 Button Press & MQTT Dispatch
#### `POST /api/button-press` or `POST /api/mqtt-publish`
`/api/mqtt-publish` is an older name of the same route: same handler, same role (11.5). Despite
its name it publishes nothing by itself; MQTT gets the `button` event like every other sink.
Reports a physical key. Every call gives a `button` event on the WebSocket (10.13) and on MQTT
(`<base>/remote/<slot>/button`, 8.1) for `action` `press` (the default) and `release`; `hold`
repeats give nothing (they are sent to the device by `hub_rf` itself). `hub_rf` sends physical
keys on its own local key channel (below) and posts here only when `hub_daemon` has none (an
older `hub_daemon`): `{"button": <key name>, "action", "source": "remote_rf", "slot"}` plus
`deviceId` and `command` when the key was mapped to a device command; other callers may keep
posting the key as `command`. Only a request with a `slot` gives an event with a slot: the
Bluetooth remote's keys (`source` `"bt_remote"`, posted by `hub_btstack`) and other calls without
one go to `<base>/button`.

**Key channel from `hub_rf`.** One UNIX datagram per press, hold and release on
`/tmp/hub_daemon_keys.sock`, read in order by one thread of `hub_daemon` (no HTTP worker, no
main loop): `{"seq", "ts", "slot", "button", "action", "deviceId", "command"}`, `ts` being
`CLOCK_MONOTONIC` in ms when `hub_rf` received the key. `hub_daemon` sends the device command as
`POST /api/ir-send` with that `action` does and gives the same button event as this route. A
press or hold older than 1000 ms is skipped (logged), a release always runs. A key from this
channel is sent at once: the device's `interDeviceDelay`, `interKeyDelay` and
`holdInterKeyDelay` apply to commands from every other source (this API, MQTT, activity
sequences), not to a key the user just pressed on a remote. Not an API for other callers: format
in `payload/source/hub_key_channel.h`.
- **Request Body**:
  ```json
  {
    "deviceId": "70321456",
    "command": "VolumeUp"
  }
  ```
  The body must be JSON. `pulseMs` and `topic`, fields of the old layer, are accepted and ignored.
- **Response `200 OK`**: `{"ok": true, "button": "VolumeUp"}` (the MQTT pulse topics of the old
  layer are gone, see 8.1).
- **Error `400`**: `{"ok": false, "error": "Missing command parameter"}` without `button` and
  `command`.

---

### 5.4 Raw Infrared
#### `POST /api/ir-raw`
Sends a raw infrared frame given as carrier and microsecond timings (what an IR receiver or
Home Assistant's `infrared` platform produces). Capability flag: `ir_raw`.
- **Request Body** (`Content-Type: application/json`, up to 2048 timings):
  ```json
  {"frequency": 38000, "timings": [9000, -4500, 560, -560, 560, -1690], "port": 1, "repeats": 1}
  ```
  - `frequency`: carrier in Hz, 20000 to 60000. Default 38000; `0` also means the default (the
    blaster has no unmodulated output).
  - `timings`: microseconds, 2 to 2048 values, each 1 to 65535 in absolute value. Either signed
    (positive mark, negative space, alternating) or all positive alternating mark/space starting
    with a mark. Mark/space pairs ending with the gap after the frame are the usual form; an odd
    count (ending with a mark) is accepted and the Hub adds 40 ms of silence after the last mark
    (not counted in `duration_ms`).
  - `port`: bitmask as `/api/ir-send` (`1` front blaster, `2` jack 1, `4` jack 2, `7` all). Default 7.
  - `repeats`: 1 to 10, default 1. Written into the code; the Hub's IR player sends the frame once.
- **Response `200 OK`**: `{"ok": true, "count": 6, "frequency": 38000, "duration_ms": 17}`
  (`frequency` = the carrier sent, `duration_ms` = sum of the timings: the frame that went out).
- **Error `400`**: `{"ok": false, "error": "<field>: ..."}` naming the offending field.
- **Error `409`**: `{"ok": false, "error": "busy"}` while another IR transmission holds the I2S
  blaster (the same lock the orchestrator and `/api/ir-send` take).
- **Error `500`**: `{"ok": false, "error": "IR transmit failed"}`.
- Nothing of the frame is logged.

### 5.5 Infrared Capture Mode
#### `POST /api/ir-capture`
Turns the capture mode on or off. While it is on, a thread keeps the Hub's IR receiver open and
pushes one `ir_received` event (10.13b) per button press, to every WebSocket client and on MQTT
`<base>/ir/received` (8.1). It is what Home Assistant's infrared receiver uses to learn codes. An
event carries one frame of the press (the first complete one), not the whole press that
`POST /api/capture` (5.2a) keeps.
Capability flag: `ir_capture`. Admin only, like `POST /api/capture`.
- **Request Body**: `{"enabled": true}` or `{"enabled": false}`.
- **Response `200 OK`**: `{"ok": true, "ir_capture": true}`.
- **Error `400`**: `enabled` missing or not a boolean.
- **Error `503`**: no IR receiver, or it could not be set up.

Behaviour:
- One event per press: the first complete frame is decoded (5.2a keeps every frame), then nothing until the
  receiver has been dark for 150 ms, so a held button gives one event.
- Sends keep working while the mode is on (activity steps, `ir-send`, `ir-raw`): the receiver sees
  the Hub's own blasters, so what it reads during a send and 100 ms after it is dropped, and a
  press in that window is lost. A send also reprograms the shared I2S clock; the receiver sets
  its own clock again before it reads on.
- The mode does not refuse a learn (`POST /api/capture`, 5.2a): during a learn the receiver
  records the learned press for that request and sends no `ir_received` event for it; the mode
  stays reported on and pushes no `ir_capture` event. Presses before and after the learn are
  events as usual. Turning the mode on or off during a learn answers at once: off stops the
  receiver when the learn ends, on keeps the receiver the learn started.
- The sensor is short-range: the remote about 5 to 8 cm above the Hub's upper side.
- If the receiver stops delivering data (seen once on a Hub), the mode reports it in
  `GET /api/status` (`ir_capture_error`) and reopens the receiver every 5 s; a reboot clears it.
- The mode is off at every start and is not stored.
- Every change of the state (on, off, `ir_capture_error` set or cleared) is pushed as an
  `ir_capture` event (10.13c) to WebSocket clients, and the retained MQTT `<base>/hub` is
  published again with it, whoever made the change (REST, WebSocket, MQTT).

## 6. Bluetooth Stack & HID

### 6.1 Bluetooth Status
#### `GET /api/bt-status`
Returns `hub_btstack`'s status (`/tmp/hub_btstack_status.json`) with four fields `hub_daemon` adds:
`ok`, `running` (the `hub_btstack` process is alive), `connected` (any host connected) and
`pairing` (pairing mode on). `hosts` lists every known host (paired, or only named in
`devices.json`) with its address (`addr`), name, `connected`, `paired` (whether a link key is
stored for it) and `cid`; `host` is the host keys go to. `target` and `remote` are the
Bluetooth LE remote, not a host; `state` is that link's state. `hub_btstack` writes the file in
`update_status()` through `bt_status_json.c`; the tests and fixtures use the same writer
(`tests/bt_status_sample.c`).
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
  - `deviceId` (read from `targetDev` first, when the body has it): the device the new host is
    linked to. Required to turn pairing mode on.
  - `name` (default `Harmony Keyboard`): the name the Hub shows to the host.
  - Turning pairing mode on disconnects the connected hosts (their link keys are kept). It ends by
    itself after 180 s, or with `"enable": false`.
- **Response `200 OK`**: `{"ok": true, "pairing": true}`
- **Error `400`**: `"enable": true` without a target device
  (`"Target device is required for Bluetooth pairing"`).

### 6.3 Connect Host
#### `POST /api/bt-connect`
Connects to a paired Bluetooth host MAC address. Without `bdaddr`: the default host (the host
keys go to: the one of the current activity's Bluetooth device, else the pinned host, which is the
host paired last).
- **Request Body**: `{"bdaddr": "AA:BB:CC:DD:EE:FF"}`
- **Response `200 OK`**: `{"ok": true}`
- **Errors**: `400` when `bdaddr` is given and is not `XX:XX:XX:XX:XX:XX` (the same holds for
  `POST /api/bt-disconnect`, `POST /api/bt-link-device` and `POST /api/remote-pair`).

### 6.4 Disconnect Host
#### `POST /api/bt-disconnect`
Disconnects active Bluetooth host link. Without `bdaddr`: every connected host.
- **Request Body**: `{"bdaddr": "AA:BB:CC:DD:EE:FF"}`
- **Response `200 OK`**: `{"ok": true}`

### 6.5 Link Device to BT Address
#### `POST /api/bt-link-device`
Associates a logical device ID with a paired Bluetooth MAC address. An empty or missing `bdaddr` removes the device's link. `id` is accepted for `deviceId`.
- **Request Body**:
  ```json
  {
    "deviceId": "70321456",
    "bdaddr": "AA:BB:CC:DD:EE:FF"
  }
  ```
- **Response `200 OK`**: `{"ok": true}`
- **Errors**: `400` without `deviceId` or with a `bdaddr` that is not an address; `404` when there
  is no device with that id; `500` when `devices.json` could not be saved. (Older builds answered
  `200`: `{"ok": false}` without a device id or for an unknown device, `{"ok": true}` after a failed
  save.)

`hub_daemon` is the only writer of `devices.json`. When a new pairing links a device, `hub_btstack`
sends this request on loopback; when `hub_daemon` does not answer, or answers anything but `200`,
`400` or `404` (restarting, busy, the save failed), `hub_btstack` keeps the link and sends it again
after 1 s, then with a pause that doubles up to 60 s, until it is saved. It never writes the file
itself. Only a `404` or `400` drops the link, and three `200 {"ok": false}` answers (an older
`hub_daemon`'s "no such device"). A newer link of the same device waits for the answer to the older
one (or its 3 s timeout), so that the older address is not applied after it.

### 6.5a Forget Host
#### `POST /api/bt-forget-host`
Removes a paired Bluetooth host for good (a TV or box that was sold, or a device that paired by
mistake). Admin only.
- **Request Body**: `{"addr": "AA:BB:CC:DD:EE:FF"}` (`bdaddr` is accepted too)
- **Response `200 OK`**:
  ```json
  {"ok": true, "addr": "AA:BB:CC:DD:EE:FF", "devices_cleared": 1, "default_host_cleared": true}
  ```
- **Errors**: `400` when `addr` is not `XX:XX:XX:XX:XX:XX` or is `00:00:00:00:00:00`; `403` for a
  user account; `503` when `hub_btstack` is not running (Bluetooth off): nothing is changed, since
  the link key lives there; `500` when `devices.json` could not be saved, or when `hub_btstack` did
  not take the command (the devices are cleared but the link key is kept: send the request again).

In order: every device linked to the host loses its `bdaddr` (or legacy `btHost`) in
`devices.json` (`devices_cleared` counts them), `bthid_host_target` is removed when it names the
host (`default_host_cleared`), then the running `hub_btstack` gets `host_forget <addr>` and, right
after the answer, drops the host's link key, closes its HID channels and its link, removes it from
the host list, stops reconnecting to it (also when a connection to it was being set up), drops a
key kept for it and a device link waiting for it, and removes `bthid_host_target` too if a
connection wrote it again in between.

The devices that were linked to the host have no host of their own any more: their keys go to the
default host (`bthid_host_target`, the last host paired) when there is one, and that can be another
TV or box. Link them to another host (`POST /api/bt-link-device`) or pair them again. If the forgotten
host was itself the default host, their keys are dropped until a host connects; that host then
becomes the default.

Afterwards the host can only come back through a new pairing (pairing mode); outside pairing mode
its pairing requests are refused. Remove the Hub ("Harmony Keyboard") from the host's own Bluetooth
list too: the host keeps its key and keeps trying to connect, and a later pairing can fail until it
is removed there.

### 6.6 Bluetooth Remote (B25) Status & Pairing
#### `GET /api/remote-status` or `GET /api/remote-pair-status`
Returns status of connected Bluetooth BLE remote (e.g. Homatics B25).

> [!NOTE]
> **Sleeping BLE Remotes & First Button Press**:
> When a BLE remote (such as the Homatics B25) enters low-power sleep mode, the first button press wakes up the remote's radio rather than transmitting a keycode. On the Homatics B25, this initial wake press drops the command and flashes the red indicator light. The second button press will register and execute normally once the BLE remote finishes reconnecting to the Harmony Hub.

- **Response `200 OK`**: `hub_btstack`'s status object (the one of 6.1: `state`, `target`, `remote`,
  `host`, `hosts`, ...; only `{"state": "idle"}` when there is no status file) with these fields
  added: `ok`, `running` (the `hub_btstack` process is alive), `bdaddr` (the pinned remote, when
  there is one) and, during and after a scan, `scanning` and `devices`, the remotes the scan found:
  ```json
  {
    "state": "scanning",
    "ok": true,
    "running": true,
    "scanning": true,
    "devices": [
      {"addr": "11:22:33:44:55:66", "bdaddr": "11:22:33:44:55:66", "name": "B25", "rssi": -58, "pairing": true}
    ]
  }
  ```

#### `POST /api/remote-scan`
Initiates Bluetooth scan for nearby BLE remotes. The scan lasts 8 s; read its results with
`GET /api/remote-status` (`scanning`, `devices`).
- **Response `200 OK`**: `{"ok": true, "state": "scanning", "message": "Bluetooth remote scan started"}`

#### `POST /api/remote-scan-stop`
Stops a running BLE remote scan. Reply `{"ok": true, "state": "idle", "message": "Bluetooth remote scan stopped"}`.

#### `POST /api/remote-pair`
Pairs with target BLE remote MAC address.
- **Request Body**: `{"bdaddr": "11:22:33:44:55:66"}`
- **Response `200 OK`**: `{"ok": true, "message": "Pair command issued to BTstack"}`. The answer is
  the same without `bdaddr`; nothing is done then.

The Hub connects only to the remote paired this way (the scan lists the remotes in range, the pair
request pins one). With no remote pinned it does not scan and does not connect to any BLE device on
its own; older builds bonded with any discoverable BLE HID device in range.

The pair request also opens a pairing window of 120 s for that address. A new LE pairing (bonding)
is accepted only inside it; outside it the Hub declines every pairing request, also from a device
that advertises with the pinned remote's address. When the pinned remote reconnects without a key
for this Hub (it was reset or paired elsewhere), the stored bond is kept and the link is closed:
pair the remote again with this request. Remote buttons count only on an encrypted link.

#### `POST /api/remote-unpair`
Forgets the pinned BLE remote: removes its address from `bt_remote_map.json`, and `hub_btstack`
disconnects it, deletes its target file, its LE bond and its stored HOGD record, then stays idle.
The Classic Bluetooth hosts (link keys, `devices.json`) are not touched.
- **Response `200 OK`**: `{"ok": true, "state": "standby", "map_cleared": true, "message": "Bluetooth remote forgotten"}` (`map_cleared` false when the map named no remote)

### 6.7 Remote Key Mappings
#### `GET /api/remote-mapping`
Returns physical key bindings for BLE remote: the stored `bt_remote_map.json`. Without a map file
the answer is `{"ok": true, "remote": {"name": "Homatics B25", "bdaddr": ""}, "activities": {}}`.

#### `POST /api/remote-mapping-save`
Saves BLE remote button mapping JSON. The body is stored as the file itself, so it must be one JSON object of less than 262144 bytes (256 KB, what the readers load): `400` otherwise, `413` when larger; the stored mapping is then kept.
- **Response `200 OK`**: `{"ok": true}`; `500` when the file could not be written.

### 6.8 Bluetooth Mouse
#### `POST /api/bt-mouse` or `POST /api/v1/bt/mouse`
Sends one mouse report to a Bluetooth host. Open to the `user` role (11.5).
- **Request Body**: `{"dx": 10, "dy": -5, "buttons": 0, "wheel": 0}` for a move, or
  `{"action": "click", "button": "left"}`.
  - `action`: `click`, `down` or `up` act on `button` (letters only, default `left`); `wheel`
    scrolls by `wheel`; anything else, or no `action`, is a move by `dx`, `dy` with the `buttons`
    bit mask and `wheel`.
  - `target`: the host's Bluetooth address (optional; the default host otherwise).
  - The WebSocket action `bt_mouse` (10.7a) does the same, but names this field `mouseAction`.
- **Response `200 OK`**: `{"ok": true}`; `{"ok": false}` when `hub_btstack` did not take the
  command (not running).
- **Error `400`**: `target` is not a Bluetooth address, or `button` is not a plain name.

---

## 7. RF Subsystem & Physical Remotes (`/api/rf/*`, touchscreen remotes, Companion, Smart Control)

Requests starting with `/api/rf/*` are proxied to the native RF daemon `hub_rf` running on port 8092.
`hub_rf` answers every `/api/rf/...` request it does not know (another path, or another method than
the one listed here) with the RF status of 7.1 and `200`, not `404`, and does nothing else. Use the
method and path given in this section: a `POST /api/rf/sync`, for example, only returns the status.

Other `hub_rf` routes:
- `GET` or `POST /api/rf/pull-config?slot={1..6}`: asks the remote to send its settings back
  (`RF_MSG_CONFIG_PULL_REQ`). `{"status":"pull_requested","slot":1,"ok":true}`; `400` for a
  Companion slot, `409` during an RF OTA.
- `GET` or `POST /api/rf/log-level?level={0..3}`: sets `hub_rf`'s log level (3 = debug) until it
  restarts; without `level` it only reports it. `{"log_level":1,"ok":true}`.
- `GET` or `POST /api/rf/push-icon?id=<id>&type=<type>&slot={0..6}`: sends a custom icon to the
  remote (slot `0` or none: every paired remote). `id` and `type` are 1 to 19 characters of
  `A-Z a-z 0-9 _ -` (`400` otherwise; `type` defaults to `activity`). The transfer runs in the
  background: the answer is `202` `{"status":"icon_push_started","id":"<id>","slot":0,"ok":true}`
  at once, and `hub_rf` logs how many remotes took the icon. `409` during an RF OTA.

`409` `{"error":"RF OTA in progress","ok":false}` is the answer during an RF firmware update (7.5)
for pair, unpair, pull-config, sleep, send, push-icon, push-config, wake, and the `POST` of
remote-config and remote-direct-buttons. `GET /api/rf/sync` answers `200`
`{"status":"ota_in_progress","ok":true}` instead and sends nothing.

A request to `hub_rf` may be at most 4095 bytes, headers included: a longer one is answered `413`
(`"Request body too large"`), one whose body did not arrive whole `400`
(`"Request body incomplete"`). This bounds the `POST` of remote-config and remote-direct-buttons.

### 7.1 RF Subsystem Status
#### `GET /api/rf` or `GET /api/rf/status`
Returns CC2544 radio state, paired remote slots, model identification, screen capabilities, the pending configuration sync and the RF update state.
- **Response `200 OK`**:
  ```json
  {
    "running": true,
    "ok": true,
    "fw_version": "v1.00",
    "paired": true,
    "rf_address": "A1B2C3D4",
    "hub_rf_address": "A1B2C3D4",
    "pairing_active": false,
    "sync_pending": false,
    "sync_pending_slot": 0,
    "sync_pending_diff": "Hub configuration changed. Push settings to sync.",
    "hub_config_version": 34,
    "ota": { "in_progress": false, "slot": 0, "status": 0, "sent_bytes": 0, "total_bytes": 0,
             "sent_chunks": 0, "total_chunks": 0, "error": "" },
    "slots": [
      {
        "slot": 1,
        "name": "Harmony Ultimate (Slot 1)",
        "model": "ultimate",
        "model_name": "Harmony Ultimate",
        "skin": 100,
        "equad_id": 16417,
        "has_screen": true,
        "supports_ota": true,
        "active": true,
        "in_sync": true,
        "address": "12345678",
        "rf_address": "12345678",
        "sw_version": "2.4.2",
        "remote_id": "20140101120000-000002",
        "config_version": 34,
        "battery": { "percent": 87, "charging": false, "voltage_mv": 3980, "reported_at": 1762387200, "restored": false },
        "link": { "frames_rx": 1520, "acked_sent": 412, "acked_ok": 398, "retries": 14, "naks": 9,
                  "key_events": 640, "key_events_lost": 3, "key_lost_press": 1, "key_lost_release": 0, "key_lost_other": 2,
                  "last_seen_s": 2 }
      },
      {
        "slot": 2,
        "name": "Harmony Companion (Slot 2)",
        "model": "companion",
        "model_name": "Harmony Companion",
        "skin": null,
        "equad_id": 16422,
        "has_screen": false,
        "supports_ota": false,
        "active": true,
        "in_sync": true,
        "address": "87654321",
        "rf_address": "87654321",
        "sw_version": "N/A",
        "config_version": 0,
        "battery": { "percent": 0, "charging": false, "voltage_mv": 0, "reported_at": null, "restored": false },
        "link": { "frames_rx": 310, "acked_sent": 0, "acked_ok": 0, "retries": 0, "naks": 0,
                  "key_events": 0, "key_events_lost": 0, "key_lost_press": 0, "key_lost_release": 0, "key_lost_other": 0,
                  "last_seen_s": 40 }
      }
    ]
  }
  ```
- Top level: `fw_version` is the firmware of the Hub's CC2544 radio; `rf_address` and
  `hub_rf_address` are both the Hub's own RF address (`"None"` while not known, like a slot's
  `address` and `rf_address` without a remote); `paired`: at least one remote is paired;
  `pairing_active`: the pairing window (7.2) is open. There is no `state` or `connected` field
  here: those are only in the answer `hub_daemon` gives when `hub_rf` does not run (last point).
- `sync_pending`: the Hub's configuration changed and was not pushed to the remotes yet
  (`/api/rf/push-config`, 7.4); `sync_pending_slot` (`0` = every remote) and `sync_pending_diff`
  say for which slot and why. `hub_config_version` is the version of the Hub's configuration; a
  slot is `in_sync` when its remote reported the same number as `config_version` (always `true`
  for a Companion or Smart Control, which takes no configuration).
- `ota`: the state of the RF firmware update, the same object as `GET /api/rf/ota-status` (7.5).
- The Hub has 6 remote slots (the CC2544 pipe table). Every slot is
  listed, in slot order, with every field; a slot without a remote has `"active": false`. `sw_version`: the remote software's version as it reported it (`"unknown"`
  before its first report, `"N/A"` for a remote without a screen).
- `slots[].link`: RF traffic with the remote since `hub_rf` started: protocol frames received, frames sent that wait for an ACK (resends included) and how many were ACKed, resends, sends the Hub's CC2544 refused (NAK), and seconds since the last frame from the remote (`-1` before the first).
- `slots[].link.key_*`: key frames of a touchscreen remote since `hub_rf` started. A key frame is sent once, with no acknowledgement and no resend; a remote that numbers its key frames (`docs/HARMONY_RF_PROTOCOL.md`, `RF_MSG_BUTTON_PRESS`) lets the Hub count the ones that did not arrive. `key_events`: key frames received that carry a number. `key_events_lost`: numbers missing between them. The lost ones are split by what the frames around the gap show: `key_lost_press` (a hold or release arrived for a key whose press was not seen: the key did nothing or started late), `key_lost_release` (a key frame arrived while the key before it was still down: that key stayed down), `key_lost_other` (holds, or nothing to tell them by); the three add up to `key_events_lost`. All stay `0` for a remote that does not number its key frames (older remote software, Companion, Smart Control). Each gap is also one line in the Hub log: `[!] RF key events lost: slot 1, 2 missing before #57 (release DirectionRight)`. Like the other link counters they are read on request: a change does not send an `rf_status` event.

- `slots[].model`: the product, a stable machine id: `ultimate`, `ultimate_one`, `ultimate_home`,
  `elite`, `950`, `touch` (a touchscreen remote with a skin id the stock table does not list),
  `companion`, `smart_control`, or `unknown`. A touchscreen remote (EquadID `0x4021`, the
  "Juniper" family) is identified by the skin id it reports and is `unknown` until it does
  (older harmony-ng remotes do not report one); Smart Control and Companion are identified by
  their EquadID. Never guessed from traffic.
- `slots[].model_name`: the product name as the stock remote UI shows it ("Harmony Ultimate",
  "Harmony Ultimate One", "Harmony Ultimate Home", "Harmony Elite", "Harmony 950",
  "Harmony Touch", "Harmony Companion", "Harmony Smart Control"), `null` while unknown.
  `slots[].name` is `"<model_name> (Slot N)"`; while the model is unknown,
  `"<the CC2544's slot name> (Slot N)"`, or `"Harmony Remote (Slot N)"` when the radio has no name
  for the slot.
- `slots[].battery`: the remote's last battery report: `percent`, `charging`, `voltage_mv` (one
  cell), `reported_at` (Unix time of that report; `null` before the first one, and while the Hub's
  clock was not set yet, until it is) and `restored` (`true` when `hub_rf` read the value back from
  `/tmp` at its start and the remote has not reported since). The last report per slot
  is kept in RAM (`/tmp/hub_rf_battery_slot<N>.json`, never on flash), so a restart of `hub_rf`
  shows it at once; after a reboot the block stays at its placeholder (`percent` 0, `voltage_mv` 0,
  `reported_at` `null`) until the remote reports, which it does right after its link to the Hub
  comes up, every 5 minutes while awake, on a charging change and, with its screen on, when the
  percentage moved by 2 points or more (at most once a minute). The age of a value is now minus
  `reported_at`. A new report is a change (one `rf_status` event per report); a voltage change
  alone is not.
- `slots[].skin`: the skin id the remote reported (an integer, `null` when not reported).
- `slots[].equad_id`: the EquadID the Hub's CC2544 reports for the slot (an integer: 16417 =
  `0x4021` touchscreen remotes, 16414 = `0x401E` Smart Control, 16422 / 16423 = `0x4026` /
  `0x4027` Companion; `null` when not known). See `HARMONY_RF_PROTOCOL.md` 4.3.
  The same slot objects are in the WebSocket `rf_status` event and snapshot (10.12, 10.14) and
  in the MQTT topic `<base>/rf` (8.1).
- `slots[].remote_id`: the remote's factory unit id (its `mfg` partition, `RF_MSG_UNIT_ID`),
  present once the remote reported it since pairing. The identifier for the remote's device in
  integrations; the slot number is only an attribute. Cleared on unpair.
- When `hub_rf` does not run, `hub_daemon` answers every `/api/rf/...` request itself (any path,
  any method, a pair or a wake request too) with `200` and this object, so check `running` before
  trusting an answer:
  ```json
  {"ok": true, "running": false, "state": "stopped", "connected": false, "pairing_active": false,
   "paired": false, "rf_address": "None", "hub_rf_address": "None",
   "slots": [{"slot": 1, "active": false, "address": "None", "rf_address": "None"},
             ... slots 2 to 6 the same]}
  ```

#### `POST /api/rf/status-changed` (internal)
`hub_rf` posts the object above to this endpoint whenever it changed (once a second at most; the
`link` counters and `battery.voltage_mv` are not changes). The daemon turns it into an `rf_status`
WebSocket event (10.12). Accepted from loopback only (`403` otherwise); the body must be the status
object (`400` otherwise). Reply: `{"ok": true, "changed": true|false}`.

### 7.2 RF Pairing
#### `GET /api/rf/pair?slot={1..6}` or `POST /api/rf/pair`
Opens the Hub's pairing window for Harmony touchscreen remotes (Elite, Ultimate), Harmony Companion, or Harmony Smart Control remotes. The window closes by itself after 30 s (or with `stop-pair`).
- **`slot` 1 to 6**: the remote that pairs goes to that slot. The CC2544 puts a new remote on a
  radio pipe of its own choice; `hub_rf` then gives that pipe the chosen slot, and the slot that
  pipe had takes the chosen slot's empty pipe. Maps, settings, the API and the logs use the slot;
  only the radio frames name the pipe. The table is kept in `/data/harmony/rf_slot_map.txt` (the
  pipe of slots 1 to 6, `1 2 3 4 5 6` until a pairing changes it; a broken file counts as that).
  The pipe-table reply after the pairing decides: the pipe that holds a remote now and held none
  when the window opened goes to the slot. A second request while the window is open changes the
  slot; a cancel or a timeout drops the choice. The choice holds for 45 s, or 15 s after the
  radio reports the pairing. A Hub that goes back to an older firmware ignores
  `rf_slot_map.txt`: such a remote then shows up in the slot of its pipe. The slot table is part
  of the backup bundle (`rf_slot_map`).
- **Slots 3 to 6 after the update to six slots**: the Hub now reads the whole pipe table, so a
  remote paired by an earlier owner, or a replaced Companion, shows up there. Unpair what you do
  not know. A config push goes to a touchscreen remote (in any slot) only once it has sent a
  frame since `hub_rf` started; one that has not stays pending and gets the push with its first
  frame.
- **Without `slot`** (or `0` / `all`): the remote takes the slot of the pipe the radio picks. The
  Hub's pairing button pairs into the lowest slot without a remote.
- **Response `200 OK`**: `{"status": "pairing_started"}` (also when the window is already open)
- **Error `409`**: during an RF OTA, or when the slot holds a remote (`"The slot holds a remote: unpair it first"`).

#### `POST /api/rf/wake?slot={1..6}`
Turns the remote's screen on over RF (`RF_MSG_WAKE`), as a key press would; with the screen already on it restarts the idle timeout. Returns `202` `{"status":"waking","slot":1,"ok":true}` at once and tries for about 10 s in the background; `hub_rf` logs whether the remote acknowledged it. One wake runs at a time (`202` `already_waking` while one runs); `409` during an RF OTA, `400` for a Companion slot. Remotes without this message do not answer. A remote in deep standby (suspended to RAM on battery) does not receive it either: it reports again at its next hourly alarm or key press. An RF OTA start (`/api/rf/ota-start`) sends the same request first (4 attempts). Over USB/SSH the same is `touch /tmp/trigger_wake` on the remote.

#### `GET /api/rf/stop-pair` or `POST /api/rf/stop-pair`
Stops active pairing mode.
- **Response `200 OK`**: `{"status": "pairing_stopped"}`

#### `GET /api/rf/unpair?slot={1..6}` or `POST /api/rf/unpair`
Unpairs target remote slot; without a slot (or with `slot=0` / `slot=all`) every remote.
- **Response `200 OK`**: `{"status": "unpaired", "ok": true}`
- **Error `409`**: during an RF OTA.

The RF routes read `slot` from the query, or from a JSON body (`"slot": 1`) when the query has
none. A value other than `1` to `6` is answered with `400`; `0` and `all` ("every remote") are
accepted by pair, unpair, sleep and push-icon only. Routes match the path exactly
(`/api/rf/pairing-state` is not `/api/rf/pair`). For `POST /api/rf/remote-config`
a `"slot"` of `1` to `6` in the body wins over the query.

### 7.3 Raw RF Frame Transmit
#### `GET /api/rf/send?hex={hex_stream}`
Transmits raw RF bytes through CC2544 SPI driver (up to 64 bytes, read two hex digits at a time
until the first pair that is not hex). `GET` only, and `hex` must be the first query parameter:
any other form gets the status object of 7.1.
- **Response `200 OK`**: `{"status": "sent"}`, also when not one byte could be read and nothing
  was sent; there is no `ok` field. `409` `{"error":"RF OTA in progress","ok":false}` during an RF OTA.

### 7.4 RF Remote Configuration & OTA Push
#### `GET /api/rf/remote-config?slot={1..6}`
Returns brightness, display timeout, tilt gyro, touch sensitivity, night mode, and nav button configuration for the touchscreen remote: the stored `/data/harmony/remote_slot<N>_config.json` (every setting under its camelCase name and its snake_case name, as the `POST` stores it), or the defaults when nothing was saved yet. For a Companion or Smart Control slot the answer is `{"slot": 2, "model": "companion", "has_screen": false, "supports_ota": false, "in_sync": true}`.

#### `POST /api/rf/remote-config?slot={1..6}`
Saves remote preferences and transmits push packet to the remote over RF. The body replaces the
whole stored configuration: a setting left out is not kept, it is set to its default (below). Send
every setting each time.
- **Request Body**:
  ```json
  {
    "brightness": 3,
    "timeout": 120,
    "touchEnabled": true,
    "capButtonsEnabled": true,
    "gyroEnabled": true,
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
  - Values used for a setting that is left out (and, where a range is given, for a value outside
    it): `brightness` 2, `timeout` 10 (seconds; a given value is kept within 5 to 600),
    `touchEnabled` false, `capButtonsEnabled` true, `gyroEnabled` true,
    `btnBacklight` 2 (0 to 2), `nightStart` 22 and `nightEnd` 7 (hours, 0 to 23), `navToggleKey`
    139, `navToggleMode` 1, `confirmActivity` false, `touchSensitivity` 2 (1 to 3), `usbEth` false.
  - More settings: `noScreen`, `debugLogging` (older name `powerDebug`), `language` (`en`, `de`,
    `fr`, `nl`, `es`, `it`, or the number 0 to 5), `theme` (`classic`, `dark`, `light`,
    `colourful`), `layout` (`theme`, `list`, `grid`, `carousel`) and `physicalNavEnabled` (true
    forces `layout` `list`). All false, `en` and not set when left out.
  - `remoteMode` is ignored: `0` is always stored and sent.
  - Each setting is also read under its snake_case name (`touch`, `gyro`, `nav_key`, `nav_mode`,
    `confirm_act`, `touch_sensitivity`, `btn_backlight`, `night_start`, `night_end`,
    `usbeth`, `cap_buttons`, `no_screen`, `debug_logging`, `language_id`, `physical_nav`).
- **Response `200 OK`**: `{"status": "saved_and_pushed", "ok": true}`. The answer is the same when
  the remote did not acknowledge the push (asleep or out of range): the settings are stored, the
  change is recorded as pending (`sync_pending` in the status, 7.1) and reaches the remote with
  the next configuration push.
- **Errors**: `400` for a Companion or Smart Control slot, `409` during an RF OTA, `500` when the
  settings could not be stored on the Hub.

#### `GET /api/rf/push-config` or `POST /api/rf/push-config`
Increments config version and broadcasts instant over-the-air sync packet to remote.
- **Response `200 OK`**: `{"status": "push_started"}`

#### `GET /api/rf/sync`
Forces remote activity display synchronization with hub current state.
- **Response `200 OK`**: `{"status": "synced"}`

#### `GET /api/rf/sleep?slot={0..6}` or `POST /api/rf/sleep?slot={0..6}`
Puts paired touchscreen remotes to sleep (immediate Suspend-to-RAM on battery).
- **Query Parameter**: `slot` (optional: `1` to `6` for that remote, `0` or omitted for all active remotes).
- **Response `200 OK`**: `{"status": "sleep_dispatched", "slot": 1, "sent": 1, "ok": true}` (`slot` is `0` for "all"; `sent` counts the remotes the request was sent to; a Companion or Smart Control slot is skipped). `409` during an RF OTA.
- **MQTT equivalent**: Publish to `<base>/remote/<n>/sleep`, `<n>` 1 to 6 (`<base>/remote/sleep` is slot 1). One slot per message: MQTT and the WebSocket action `rf_sleep` (10.7b) have no "all remotes" form.
- **Home Assistant**: Auto-discovered as `button.<hub>_remote_<n>_sleep`, one per slot.

### 7.5 Direct Buttons & Firmware OTA (Touchscreen Remotes)
#### `GET /api/rf/remote-direct-buttons?slot={1..6}`
Returns the stored direct-buttons file of the slot (`/data/harmony/remote_slot<N>_direct_buttons.json`), or `{"page1":[],"page2":[],"buttons":{}}` when there is none.

#### `POST /api/rf/remote-direct-buttons?slot={1..6}`
Stores the request body as that file and answers `{"status":"saved","ok":true}` (`500` when the
file cannot be written, `409` during an RF OTA). Kept for old clients only:
the remote's direct mode was removed (every remote runs in activity mode, `remoteMode` is always
`0`), so nothing reads the file and nothing is pushed to the remote.

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

#### `POST /api/rf/ota-start?slot={1..6}`
Starts background remote firmware update (touchscreen remotes only).
- **Storage & Download Policy**:
  The Hub does not store remote OTA images on flash storage. If `/tmp/remote_ui.ota.gz` is not already present in RAM, the Hub downloads `manifest.json`, `manifest.json.sig` and the remote OTA image on-demand from the update repository into `/tmp`, and sends the image only when the manifest carries a valid release signature and the image matches its SHA-256 (see `POST /api/update-apply`). An image staged by hand without a manifest is sent as is.
  Developers or offline hubs can stage a local image using `python build/deploy_hub.py <IP> --stage-remote-ota`, which uploads the local binary to `/tmp/remote_ui.ota.gz` in RAM without internet access or flash writes.
  *Note*: Authentication tokens are not forwarded during background OTA fetches; private repositories requiring tokens should stage the remote firmware image using `--stage-remote-ota`.
- **Query Parameter**: `slot` (`1` or `2`, defaults to `1`).
- **Responses**:
  - `200 OK`: `{"status":"started","slot":1,"ok":true}` — OTA worker thread started.
  - `400 Bad Request`: `{"status":"unsupported","slot":1,"error":"Companion remotes do not support OTA","ok":false}` — slot holds a Companion remote (no screen/firmware OTA).
  - `409 Conflict`: `{"status":"already_in_progress","slot":1,"error":"RF OTA already in progress on slot 1","ok":false}` — an update or download is already running.
  - `409 Conflict`: `{"status":"config_push_running","error":"A config push is running: start the update when it ends","ok":false}` — a config push is running. An update the remote asks for itself, or one started during a push, waits up to 60 s for the push before its transfer starts.
- **Status reporting (`GET /api/rf/ota-status`)**:
  - `status: 0`: Idle.
  - `status: 3`: Downloading and verifying OTA image into `/tmp` (RF sync and ACKs remain unblocked).
  - `status: 1`: RF transmission in progress (`in_progress: true`).
  - `status: 2`: Successfully transferred; remote is verifying and installing.
  - `status: -1`: Failed. Specific error conditions include:
    - `"Failed to download OTA image to /tmp"`: update source unreachable or image missing.
    - `"Remote OTA version mismatch against Hub"`: manifest version does not match the Hub's version.
    - `"Remote OTA SHA-256 checksum mismatch"`: downloaded package failed integrity check against manifest.
    - `"Remote rejected OTA: Low battery (<40%)"`: remote battery is below 40%.
    - `"Chunk N ACK timeout (can resume)"` / `"Chunk N: no progress for 20 s (can resume)"`: RF communication stalled. The staged file in `/tmp` is preserved so the update can be resumed.
    - `"Remote failed to ACK OTA_START (timeout - wake remote)"`: the remote did not answer the start (asleep or out of range).
    - `"Remote OTA manifest missing or invalid"` / `"Remote OTA manifest not signed by a release key"`: the downloaded image has no usable signed manifest.
    - `"Invalid OTA file size: N"`, `"OTA_END ACK timeout"`, `"Remote rejected OTA: SHA-256 integrity check failed"`, `"Remote rejected OTA: Error code 0xNN"`.
  - While `status` is `3`, `error` holds the text `"Preparing OTA update..."`: it is not an error.
- **Staged image age**: an image in `/tmp/remote_ui.ota.gz` older than 2 hours is deleted when an update starts, and the published one is downloaded instead.

### 7.6 Physical Remote Key Mappings (Touchscreen Remotes, Companion, Smart Control)
#### `GET /api/rf-mapping?slot={1..6}`
Returns physical button and screen mappings for the remote in the slot (1 to 6).
- **Query Parameter**: `slot` (`1` or `2`, defaults to `1`).
- **File Persistence**: Slot N (1 to 6) uses `/data/harmony/remote_slot<N>_map.json`. Only `POST /api/rf-mapping-copy` falls back to the old name `/data/harmony/elite_remote_map.json` when slot 1's file is missing.
- **Remote Model Awareness**: Mappings apply to any paired remote in that slot: a Harmony touchscreen remote (Elite, Ultimate), Harmony Companion, or Harmony Smart Control.
- **Which mapping a key uses**: the running activity's entry under `activities`. A key that activity does not map, or any key while nothing runs, uses the `"-1"` entry, then `"default"`, then the map's only activity when the map has exactly one. With several activities and neither `"-1"` nor `"default"`, the key is not sent. The "only activity" counts the non-empty entries in the map. A `"-1"` or `"default"` entry, or a key binding, that is not an object (`null` in a hand-edited map) binds nothing and ends the search. This also covers `activity_start` bindings: a Companion or Smart Control activity key bound in one activity's entry starts nothing from Off or from the other activities; put keys meant for every activity and for Off under `"-1"`. `PowerOffActivity` is handled before the map and always works. A key pressed while the remote browses a device uses that device's entry under `devices`, then the device command with the button's name.

#### `POST /api/rf-mapping-save?slot={1..6}`
Saves button assignments for the specified slot. The body must be one JSON object of less than 262144 bytes (256 KB): `400` otherwise, `413` when larger; the stored mapping is then kept.
- **Query Parameter**: `slot` (`1` or `2`, defaults to `1`).

#### `POST /api/rf-mapping-copy?from={1..6}&to={1..6}`
Copies entire button mapping configuration from one remote slot to another.
- **Query Parameters**: `from` (source slot, 1 to 6, default 1), `to` (destination slot, 1 to 6, default 2). Both may also be given in a JSON body (`{"from": 1, "to": 2}`), which wins over the query.
- A source slot without a mapping file is copied as the empty mapping `{"activities":{},"devices":{}}`. The destination slot is marked as waiting for a push (`sync_pending`, 7.1).
- **Response `200 OK`**:
  ```json
  {
    "ok": true,
    "from": 1,
    "to": 2
  }
  ```
- **Response `500`**: the destination file could not be written.

#### `POST /api/ui-remote-layout`
Saves virtual remote keypad layout for WebUI and Android App. The body must be one JSON object of less than 262144 bytes (256 KB): `400` otherwise, `413` when larger; the stored layouts are then kept. The body is stored as sent in `/data/harmony/web_remote_layouts.json`. `GET /api/ui-remote-layout` returns the stored file, or `{"ok": true, "layouts": {}}` when there is none.

Answers of the mapping routes of this section: `GET /api/rf-mapping` returns the stored file as it
is, or `{"ok": true, "mappings": []}` when the slot has none. `POST /api/rf-mapping-save` and
`POST /api/ui-remote-layout` answer `{"ok": true}`, `500` when the file could not be written. In
the body of `rf-mapping-save` a top-level `"slot"` of `2` to `6` wins over the query; the save marks the slot
as waiting for a push (`sync_pending`, 7.1).

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

**The broker is trusted as admin.** Commands that arrive over MQTT run with the admin role, including
`update_apply`, and the connection is plain TCP (user name and password in clear text). Anyone who
can publish to the broker, or read the credentials on the way to it, controls the Hub. Use a broker
account and ACLs that only the Hub and trusted clients have, on a trusted network. The global topic
`harmony/ir/pronto` sends any Pronto code it receives.

#### `POST /api/mqtt-save` or `POST /mqtt`
- **Request Body** (every field optional; a field left out keeps its stored value):
  ```json
  {
    "enabled": true,
    "host": "192.168.1.50",
    "port": 1883,
    "username": "homeassistant",
    "password": "password",
    "baseTopic": "harmony/hub",
    "clientId": "harmony-local-mqtt",
    "discoveryPrefix": "homeassistant",
    "haDiscovery": true
  }
  ```
  - `enabled`: when neither the request nor the stored file has it, the save turns MQTT on.
  - `port`: a value of 0 or less is stored as 1883.
  - `password`: a typed password replaces the stored one; an absent or empty one keeps it. A
    password is stored only while a user name is set (sent with it or already stored): saving an
    empty `username` also removes the stored password.
  - `clientId` (default `harmony-local-mqtt`) is also the base of the discovery identifier.
  `haDiscovery` (default true) publishes the Home Assistant discovery entities under
  `discoveryPrefix`; turning it off clears them. The fields of the old layer with no v2 equivalent
  (`name`, `deviceName`, `keepAlive`, `btRemoteEvents`/`Topic`, `remoteEvents`/`Topic`,
  `eliteRemoteEvents`/`Topic`, `slotNRemoteEvents`/`Topic`, `pollSeconds`, `mqttDevices`,
  `devices`) are ignored and removed from the stored file: button events are always published on
  `<base>/remote/<slot>/button`.
- **Response `200 OK`**: `{"ok": true, "message": "MQTT settings saved"}`; the client reconnects.

#### `GET /api/mqtt-status`
`{"ok": true, "connected": true, "broker": "...", "port": 1883, "baseTopic": "harmony/hub", "v2": true, "config": {...}}`. `config` is the stored MQTT settings without the password (`has_password` tells whether one is stored; saving with an empty password keeps it).

#### Topics
`<base>` is `baseTopic` (default `harmony/hub`). Payloads are JSON objects with `type`. The event
topics (`activity_state`, `progress`, `bt`, `rf`, `remote/<slot>/button`, `ir/received`) carry `seq`
and `ts` as on the WebSocket (10.8 to 10.13); `hub` and `config` carry `ts` only;
`inventory/...` and `cmd/response` carry neither; `status` is plain text. A packet larger than
8 KiB sent to the Hub (for example a long `ir/raw` body) is dropped without a reply.

| Topic | Retained | QoS | Payload |
| --- | --- | --- | --- |
| `<base>/status` | yes | 0 | `online` / `offline` (last will); both are sent with QoS 0 |
| `<base>/hub` | yes | 1 | `{"type":"hub", ...}`: the `hub` object of the snapshot (10.14) plus `ip`, `device_count`, `command_count`, `ha_discovery` and `discovery_prefix` (whether and where this Hub publishes the discovery entities described below, so a client on MQTT alone can tell the layers apart) and `ts`; published on connect, on `<base>/sync`, on a config change, again when the clock is set or jumps (so `boot_time` and `ts` are real times in the retained copy), and when the IR capture mode turns on or off or its `ir_capture_error` changes |
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
| `<base>/ir/received` | no | 0 | `ir_received` (10.13b), while the IR capture mode (5.5) is on |

`inventory_version` is the first 8 hex digits of the SHA-256 over the device and activity files. A
device chunk is republished only when its own content changed, a removed device gets an empty
retained payload on `inventory/<deviceId>` (the `inventory/<deviceId>/<n>` parts of a device that
was split are not cleared, nor surplus parts when a device shrinks), and the index is published
last. At most 64 devices are published; a device that is split has 120 command names per part.
Retained topics are republished on every
(re)connect from the bus's last payloads. While the broker is down events are dropped (nothing is
queued) and the retained state is resent on reconnect. The client uses a keepalive of 30 s and a
clean session; after a lost or refused connection it tries again after 5 s, then with a pause that
doubles up to 60 s.

The old state topics `<base>/state`, `<base>/activity`, `<base>/current_activity`,
`<base>/activity_id`, `<base>/activity/status` and the remote pulse topics (`remotes/slotN/Cmd`,
the per-remote custom topics) are gone: read `activity_state`, `hub`, `inventory/index` and
`remote/<slot>/button` instead.

#### Inbound topics of the first MQTT layer (kept)
These are aliases of the command channel for existing automations and for the discovery buttons:

| Topic | Payload | Does |
| --- | --- | --- |
| `<base>/activity/set` | an activity name, an activity id, `PowerOff` or `-1` | `start_activity` / `stop_activity` |
| `<base>/send_command` | `{"device": "<name>"` or `"deviceId": "<id>", "command": "<cmd>"` or `"commands": [...]`, optional `repeats`, `delay` (s) or `delay_ms`, `hold` (s) or `hold_ms`, `action`}` (no `value`: use `<base>/cmd` with the `send_command` action, 10.3, for a plugin command that takes one) | the commands in order, `repeats` times, `delay` apart (0.25 s default for more than one) |
| `<base>/command/<device>/<command>` | any | one command to the device (id or name); a payload that is a plain integer is the command's `value` (plugin commands) |
| `<base>/sync` | any | republishes every retained topic and the discovery entities |
| `<base>/remote/<n>/sleep`, `<base>/remote/sleep` (slot 1) | any | `rf_sleep` |
| `harmony/ir/pronto` | a Pronto hex code | sends it on every IR port once |

They answer nothing on `cmd/response`; use `<base>/cmd` for acknowledged commands.

`<base>/send_command` also reads `device_id` for `deviceId`, `dev` for `device`, `cmd` for
`command`, `num_repeats` or `numRepeats` for `repeats`, `delay_secs` or `delaySecs` for `delay`,
`delayMs` for `delay_ms`, `hold_secs` or `holdSecs` for `hold`, and `holdMs` for `hold_ms`.
`action` is `press`, `hold` or `release` (a tap otherwise); `hold` without a hold time holds the
key for 0.5 s. The limits of 10.3 apply (32 commands, 50 repeats, 60 s delay, 10 s hold, 16 jobs,
300 s per job). `<base>/remote/<n>/sleep` takes slot 1 to 6 only: there is no "all remotes" topic. Home Assistant
discovery announces the sleep button of every slot.

#### MQTT-type devices
A device of type `MQTT` (or with `mqttEnabled`) is driven through the broker instead of IR or
Bluetooth: a command to it (an activity sequence, `POST /api/ir-send`, the remote, the command
channel) publishes `"1"` on its topic, then `"0"` after `mqttPulseMs` (default 1000), and raises the
`button` event (10.13) as well. The topic comes from the device's `mqttTopic` template
(default `{base_topic}/button/{device}/{command}`; placeholders `{base_topic}`, `{baseTopic}`,
`{base}` or `{root}` for the base topic, `{device}`, `{device_id}`, `{deviceId}` or `{deviceName}`
for the device, all four replaced by the device's name, not its id (the id only when the device has
no name), and `{command}` or `{button}`;
a template starting with `/` is appended to the base topic). `POST /api/device-save` stores `mqttTopic` and `mqttPulseMs`;
`POST /api/device-mqtt-test` (3.8) publishes one pulse.

#### Home Assistant discovery
With `haDiscovery` on, the Hub publishes retained discovery configs under
`<discoveryPrefix>/<component>/<ident>_<object>/config` (`ident` = the client id in lower case,
every character other than letters, digits, `-` and `_` replaced by `_`) on
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
`id` is an opaque client string (up to 64 characters), echoed in the reply (a longer one is echoed
whole here; only WebSocket replies cut it to 64); a request without `id`
is executed and not answered. Actions and fields mirror the WebSocket actions (section 10):
`ping`, `get_state`, `get_snapshot` (the reply carries the snapshot), `start_activity` (`activity`),
`stop_activity`, `cancel_activity`, `cancel_commands`, `send_command` (`deviceId`, `command`, `status` = `press`,
`hold`, `release` or omitted for a tap; `value`, an integer, for a single plugin command: one with a range needs
it and answers `400` "value required" without; a list or `num_repeats` with such a command is refused the
same way), `bt_key` (`key`, `keyAction`, `target`), `bt_text`,
`bt_mouse`, `rf_wake` (`slot`), `rf_sleep` (`slot`), `sync`, `ir_raw` (fields of 5.4),
`ir_capture` (`enabled`, 5.5; the reply carries `enabled`),
`update_check`, `update_apply`, `activity_help`, `activity_help_answer` (10.6a, 10.6b).

Reply on `<base>/cmd/response`:
```json
{"type": "cmd_response", "id": "c7d1", "action": "start_activity", "ok": true, "state": "starting"}
{"type": "cmd_response", "id": "c7d1", "action": "start_activity", "ok": true, "state": "queued"}
{"type": "cmd_response", "id": "c7d2", "action": "get_state", "ok": true, "activity_state": {...}}
```
Codes and errors equal the REST ones (400 bad input, 404 not found, 409 busy for the update check,
503 a service is not running); `start_activity` and `stop_activity` during a transition are queued
(`state` `queued`), as in REST 4.3. An ack carries the fields of the REST reply as well (`count` and `duration_ms` for
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
Tests the connection to an AP by switching to it: the Hub writes the network to `/etc/wpa_supplicant.conf` and `/etc/wpa_supplicant.ath0.conf`, reconfigures wpa_supplicant and waits up to 12 s for the connection. When it fails, the previous file is put back and applied again, so the Hub returns to the network it was on. When it succeeds, the tested network stays saved and the Hub stays on it: a successful test is a switch. The answer comes when the test has ended.
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
- **Response `400 Bad Request`**: `{"ok": false, "error": "Incorrect Wi-Fi password"}`. The Hub reports a wrong password when the key handshake starts and fails twice, or never completes. Other texts: `"Wi-Fi network not found (check the name; 2.4 GHz networks only)"` (the network was never seen), `"Connection timed out (AP unreachable or handshake failed)"`, `"Another Wi-Fi test is running, try again in a few seconds"`, and the reason when the SSID or the password cannot be used (see `POST /api/wifi-save`).

#### `POST /api/wifi-save` or `POST /wifi`
Saves Wi-Fi credentials to `/etc/wpa_supplicant.conf` and `/etc/wpa_supplicant.ath0.conf`.
`apply` defaults to `save`: the settings are written but not applied, and the answer's `message` is then `"Wi-Fi settings saved"`. When `apply` is `reconfigure`, `apply`, `1` or `reboot`, the Hub first tries the network the way `POST /api/wifi-test` does (the files are rewritten, and put back when the connection fails), unless `force` is true, which skips the test. If the test connection fails, the switch is rejected and existing settings are preserved. `reboot` restarts the Hub after the save; the other three reconfigure wpa_supplicant.
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
- **Response `400 Bad Request`**: `{"ok": false, "error": "Wi-Fi test connection failed: Connection timed out (AP unreachable or handshake failed). Settings not changed."}`
- **Response `500`**: the two files could not be written.
- **Values**: the SSID and the password are written as typed (wpa_supplicant reads a quoted value without escapes). Refused with `400` and the reason: an SSID that is empty or longer than 32 bytes, a password outside 8 to 63 characters (unless the network is open, or the password is the 64 hex digit raw key, which is written unquoted), and a line break or another control character in either.

#### Wi-Fi over USB (no HTTP)
A computer on the Hub's USB port can read and set the Wi-Fi network with the stock USB HID file
protocol (`/sys/wifi/connect`), answered by `hub_usbwifi`: `tools/set_wifi_usb.py`, or
`tools/harmony_hid.py` `provision_wifi()` / `get_wifi_status()`. It tests the network the way
`POST /api/wifi-save` with `apply` does (same code, same rules for the values) and shares its
one-at-a-time lock: a web UI test while a USB attempt runs gets `"Another Wi-Fi test is running,
try again in a few seconds"`, and the other way round. Protocol and limits:
[ETHERNET_AND_NETWORK_SETUP.md](ETHERNET_AND_NETWORK_SETUP.md) section 5. It is on by default and
answers any computer on the port without a login; `usb_wifi_setup: false` in
`POST /api/ethernet-save` (8.3) turns it off.

### 8.3 USB Ethernet Configuration
#### `POST /api/ethernet-save` or `POST /ethernet`
Configures USB Ethernet interface mode (DHCP or static IP). Every save rewrites the whole
`ethernet.conf`; nothing is merged with the stored file. A field left out gets its default, not
its stored value: `eth_enabled` and `eth_fallback_wifi` become on, `eth_mode` `dhcp`,
`eth_netmask` `255.255.255.0`, `eth_ip`, `eth_gateway` and `eth_dns` empty (in `dhcp` mode an
address that is not a valid IPv4 address is stored empty too; in `static` mode it is refused, see
`400` below). Send every field each time. The one exception is `usb_wifi_setup`: left out, it
keeps its stored value (the web UIs send it only after reading the stored value, so an older client cannot turn it back on). The settings are
applied (`network_manager.sh apply`) only when `apply` is `apply`, `1`, `reconfigure` or
`restart`; without it they take effect at the next network restart.
- **Request Body**:
  ```json
  {
    "eth_enabled": true,
    "eth_fallback_wifi": true,
    "eth_mode": "dhcp",
    "eth_ip": "192.168.1.100",
    "eth_netmask": "255.255.255.0",
    "eth_gateway": "192.168.1.1",
    "eth_dns": "192.168.1.1",
    "apply": "apply"
  }
  ```
- **Response `200 OK`**: `{"ok": true, "message": "Ethernet configuration saved"}`
- **Response `400 Bad Request`** (static mode only): an invalid address, an address a host cannot use (0.x, 127.x, 224.x and above), a netmask that is not a mask, the network or broadcast address of the subnet, a gateway outside the address's subnet or equal to the address, or a DNS entry that is not an IPv4 address (`eth_dns` may list several, separated by spaces or commas). Nothing is saved. A static address is used on Wi-Fi as well as on Ethernet.
- `eth_enabled` (default `true`) off: the Hub does not start Ethernet (a standard Hub keeps its USB port in device mode). `eth_fallback_wifi` (default `true`): a static address moves to Wi-Fi when the Ethernet link is down.
- `usb_wifi_setup` (default `true`, stored as `USB_WIFI_SETUP`; left out = unchanged): Wi-Fi setup over USB ("Wi-Fi over USB" above). `false` refuses every USB request at once (status read included); the next start of the Hub services or boot then starts no `hub_usbwifi`, and the boot no longer waits for a computer on the USB port, so Ethernet over USB starts as it did before this setting existed. Turning it back on takes effect at the next start of the Hub services or boot. With it off and the Hub on no Wi-Fi network, the way back is an Ethernet adapter (when Ethernet over USB is on) or a factory reset.

---

## 9. Backup, Import & Export

### 9.1 Export Configuration
#### `GET /export/{target}`
Exports complete hub configuration bundle or individual configuration elements.
- Targets (any other name answers `404 "Export target not found"`; `hub_id` is only a section of
  `bundle`, not a target of its own, and `GET /export/hub_id` answers that `404`). A single target
  is sent as a file download; when its file does not exist the answer is still `200`, with a
  placeholder (`[]`, `{}`, an empty mapping, or a comment line for the text files):
  - `bundle` (Full JSON snapshot of devices, activities, settings, MQTT, network, mappings, keys:
    one object with `version` (1), `timestamp`, one member per target below whose file exists,
    under the target's name (`devices`, `activities`, `settings` and `mqtt` are always there, empty
    when the file is missing), `hub_id` when the Hub has that file, and `btstack_keys`, the
    Bluetooth link keys as base64. The header `X-Checksum-CRC32` carries the CRC32 of the body)
  - `devices`
  - `activities`
  - `settings`
  - `mqtt`
  - `network` (also `ethernet`)
  - `wifi`
  - `bluetooth`
  - `bt_remote_map` (also `remote-mapping`)
  - `remote_slot1_map` to `remote_slot6_map`
  - `rf_slot_map` (text: which radio pipe each slot is, six different numbers from 1 to 6, such
    as `1 4 3 2 5 6`; `1 2 3 4 5 6` when the Hub has no table file. A bundle leaves out a file
    that is no such table, which hub_rf ignores as well). `hub_rf` reads it at its start: after
    an import that changes it, hub_daemon asks hub_rf to restart (`POST /api/rf/restart`), which
    it does once no config push, update or pairing runs; `init.sh` starts it again within
    seconds, and the remotes are off the air for that time. Before it ends, hub_rf moves each
    remote's version file, battery and settings file (`remote_slot<N>_config.json`) to the slot
    its radio pipe now is, so the new hub_rf shows every remote with its id and version in its
    new slot and the next config push sends each remote its own settings. An import whose table equals the one
    in use does not restart hub_rf. When hub_rf is not running the table applies at its next
    start. It names the pipes of this Hub's radio: on another Hub, or after remotes were paired
    again, it puts the maps on other remotes.
    `POST /api/rf/restart` (answer `202 {"ok":true,"status":"restart_scheduled"}`) is a route of
    hub_rf's loopback API, and hub_daemon's `/api/rf/` proxy on port 8089 passes it on too: there
    it needs an administrator (any client while authentication is off), as the unpair route does.
  - `users` (the web UI accounts)
  - `ui_remote_layout` (also `ui-remote-layout`)
  - `auth`
- Only one export or import runs at a time: another one meanwhile is answered `409`.

### 9.2 Import Configuration
#### `POST /import`
Imports and atomically applies configuration files or full bundle (`target` `bundle`, or `all`
as the new web UI calls it). An import of the accounts that turns authentication on also answers
`auth_enabled` and `to_check`, the accounts and access tokens to check (11.1).
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
- `target` is `bundle` or one of the single export targets of 9.1 (`payload` = that file's
  content). In a bundle the Wi-Fi (`wifi`) and Ethernet (`network`) sections are skipped unless
  the request has `"restore_network": true` (top level, next to `target`), so a restore does not
  cut the Hub off a network it is reached on. A bundle may be sent as the `payload` object, as
  `payload` text, or as the top-level object itself (the bundle's members next to `target`).
- A bundle's `btstack_keys` (base64, as exported) is restored as the Bluetooth link key file; it is
  refused, with the whole import, when it is not valid base64 or 200000 bytes or more.
- After a bundle import the Hub reloads its configuration, marks the remotes' configuration as
  waiting for a push and tells `hub_btstack` to reload its map. An import of a bundle, of `auth`
  or of `users` ends every session.
- Every section is checked before anything is written: `devices` must be an object with a `devices`
  array, `activities` an object with an `activities` array, and `settings`, `mqtt`, `bluetooth`,
  `bt_remote_map`, `remote_slot1_map` to `remote_slot6_map`, `ui_remote_layout` and `users` JSON objects
  (sent as objects, or as strings that parse completely). `rf_slot_map` must be text with six
  different numbers from 1 to 6 (written as `a b c d e f`). `network`, `wifi`, `auth` and `hub_id` are
  text files and are written as sent. An empty `devices` or `activities` list in a bundle (what an
  export holds when the file could not be read) is skipped.
- The new files are written under temporary names first and then renamed into place.
- **Response `200 OK`**: `{"ok": true, "message": "Restored successfully"}`
- **Response `400`**: a section is not valid; nothing was written:
  `{"ok": false, "error": "Import refused, nothing was changed: 'devices' is not valid"}`.
  An unknown `target`, or a single target without `payload`, answers
  `{"ok": false, "message": "Import failed"}`.
- **Response `500`**: a file could not be written.

---

## 10. WebSocket Protocol (`/ws` or Port 8089)

Connect via WebSocket to `ws://<hub_ip>:8089/ws`. In normal operation nothing listens on port 80. The upgrade is recognised by its `Upgrade: websocket` header, on any path. The Hub serves at most 16 WebSocket clients, ports 8089 and 8088 together: a further upgrade is answered `503`.

A client frame may arrive in several pieces; the Hub acts on it once it is complete. A frame longer than 4095 bytes (65535 on port 8088) closes the connection, and only complete frames count as traffic for the keepalive (10.15).

**Replies.** A reply carries the request's `msgId` when it is a number other than 0, and the request's `id` when it is a text (cut to 64 characters), so `id` works as a request id as on MQTT (8.1). An `ack` always has `ok`; a failed one also has `code` (the status the REST route would give) and `error`:
```json
{"type": "ack", "action": "bt_key", "msgId": 7, "ok": false, "code": 503, "error": "Bluetooth stack not running"}
```
An action the Hub does not know is answered `{"type": "ack", "action": "<action>", "ok": false, "code": 400, "error": "unknown action"}`.

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
  "state": 2,
  "stateName": "running",
  "busy": false,
  "sending": false,
  "queued": null
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
Multiple commands or repeats can be queued as an array:
```json
{
  "action": "send_command",
  "deviceId": "70321456",
  "command": ["VolumeUp", "VolumeDown"],
  "num_repeats": 2,
  "delay_secs": 0.2,
  "hold_secs": 0.0,
  "msgId": 3
}
```
- `deviceId` (also `device_id`, `device` or `dev`): the device id or its name.
- `command` (also `commands`): one name or a list of up to 32 (names after the 32nd are dropped
  without an error).
- `num_repeats` (also `repeat` or `repeats`): times through the list, 1 to 50, default 1.
- `delay_secs` (also `delay`): seconds between commands, at most 60; default 0.25 for a list or
  repeats, and two sends of a job are never less than 0.1 s apart. `hold_secs` (also `hold`):
  seconds a key is held before its release, at most 10.
- `cmdAction` (also `status`, `action_type` or `btnAction`): `press` (or `down`), `hold` (or
  `repeat`), `release` (or `up`); anything else or nothing is a tap. In a list or with repeats,
  `press` and `hold` without `hold_secs` are sent as taps.
- `ir_send` is another name of this action.
- A single command with no repeat or hold is sent at once, unless a job is still running.
- `live` (optional): `true` sends such a single command as a person's press, as `live` of
  `POST /api/ir-send` (5.1). A list, repeats, holds and a command queued behind a job are never
  live.

**Response**:
```json
{
  "type": "ack",
  "action": "send_command",
  "ok": true,
  "msgId": 3
}
```
A list, repeats or a hold run as a job of the command sequencer, one job after the other (at most
16 jobs, the running one included). The ack of a job carries `"state": "started"`, or `"queued"` when another job runs
first. A job may last at most 300 s (commands x repeats x (gap + hold)); a longer one is refused
with code 400. Errors: 400 without `deviceId` or `command`, 404 unknown device or command, 409
queue full, 500 `IR transmit failed` for a single command that could not be sent.

#### 10.4 Start Activity
```json
{
  "action": "start_activity",
  "activity": "123456",
  "msgId": 4
}
```
**Response**:
```json
{
  "type": "ack",
  "action": "start_activity",
  "ok": true,
  "msgId": 4,
  "state": "starting"
}
```
`activity` names the activity (`id` is taken only when there is no `activity` and it names a
configured activity; as the request id it is then also echoed in the reply). `state` is `queued`
while another transition runs (REST 4.3). Errors: `400` `activity required`, `500`
`could not build the transition` for an unknown activity.

#### 10.5 Stop Activity / Power Off
```json
{
  "action": "stop_activity",
  "msgId": 5
}
```
`power_off` is another name of this action.
**Response**:
```json
{
  "type": "ack",
  "action": "stop_activity",
  "ok": true,
  "msgId": 5,
  "state": "stopping"
}
```
`state` is `queued` while another transition runs.

#### 10.6 Cancel Activity / Interlock Reset
```json
{
  "action": "cancel_activity",
  "msgId": 6
}
```
`reset_interlock` is another name of this action.
**Response**:
```json
{
  "type": "ack",
  "action": "cancel_activity",
  "ok": true,
  "commandsCancelled": 0,
  "msgId": 6
}
```
`cancel_activity` also drops the command jobs of 10.3 (`commandsCancelled` = how many; a held
command is released first). `cancel_commands` drops the jobs only, with the same reply; it needs
the rights of `send_command`.

#### 10.6a Firmware Update Check and Install
`update_check` runs the check of `POST /api/update-check` (optional `repo`, `token`,
`remote_version`, as there) and answers with the fields of `GET /api/update-status` read after the
check, plus the check's `message`. The check runs `update.sh check` or downloads the published
version, which takes seconds: the action runs it on a thread and the reply comes when it ends
(other actions are answered meanwhile). One check at a time; another request meanwhile gets
`409`. `update_apply` starts `POST /api/update-apply` (optional `repo`, `token`) and answers at
once with its `status` (`applying`) and `message`, or `409` while an update is already running;
the Hub restarts its services when the update is installed. Both are admin actions, like their REST endpoints.
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
`update_apply` during an activity switch is refused like the REST request (2.3), with the same
fields:
```json
{"type": "ack", "action": "update_apply", "msgId": 42, "ok": false, "code": 409,
 "error": "An activity switch is running: start the update again when it has finished",
 "status": "activity_switch", "busy": true, "sending": true, "queued": null}
```

#### 10.6b Activity Help
`activity_help` is `POST /api/activity/help`: re-sends the running activity's commands except
power toggles and lists the devices with a power toggle to ask about (`activity`, defaults to the
running one; `dry_run` returns the steps as `preview` and sends nothing). `activity_help_answer`
is `POST /api/activity/help/answer` (`device`, `on`): for `on: false` the device's power toggle is
sent; only devices the last Help listed, one answer each (`404` otherwise). A Help on an activity
that is not running gets `409`, and so does an answer that comes after the Hub left that activity
or more than 300 s after the Help.
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

#### 10.6c IR Capture Mode
`ir_capture` is `POST /api/ir-capture` (5.5): `enabled` turns the capture mode on or off; signals
then arrive as `ir_received` (10.13b).
```json
{"action": "ir_capture", "enabled": true, "msgId": 61}
```
**Response**:
```json
{"type": "ack", "action": "ir_capture", "ok": true, "msgId": 61, "enabled": true}
```
A refused request (`503`, as in 5.5) carries `enabled` too, the state after the request,
next to `code` and `error`.

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
- `keyAction`: `"tap"` (default), `"down"` (or `"press"`), `"up"` (or `"release"`), or
  `"release_all"` (releases every key, as `key` `"release_all"` does).
- `key`: Key name or `"release_all"`; at most 64 characters, no control characters (`400`).
- `target` (also `bdaddr`): Optional Bluetooth host MAC address, `XX:XX:XX:XX:XX:XX` (`400`
  otherwise; the same for `bt_text` and `bt_mouse`). `bt_text` takes at most 240 characters, and
  the `bt_mouse` `button` is a name of at most 16 characters without control characters.
**Response**:
```json
{
  "type": "ack",
  "action": "bt_key",
  "ok": true,
  "msgId": 7
}
```
`503` when `hub_btstack` is not running.

#### 10.7a Bluetooth Text and Mouse
```json
{"action": "bt_text", "text": "hello", "target": "AA:BB:CC:DD:EE:FF", "msgId": 71}
{"action": "bt_mouse", "dx": 10, "dy": -5, "buttons": 0, "wheel": 0, "msgId": 72}
{"action": "bt_mouse", "mouseAction": "click", "button": "left", "msgId": 73}
```
- `bt_text`: types `text` (line breaks become spaces). `target` (also `bdaddr`) as for `bt_key`.
- `bt_mouse`: without `mouseAction`, moves by `dx`, `dy` with the `buttons` bit mask and `wheel`;
  `mouseAction` `click`, `down` or `up` acts on `button` (default `left`), `wheel` scrolls by
  `wheel`. `target` as for `bt_key`. The REST route `POST /api/bt-mouse` (6.8) names this field
  `action`.
- **Response**: `{"type": "ack", "action": "bt_text", "ok": true, "msgId": 71}`; `400` without
  `text`, `503` when `hub_btstack` is not running.

#### 10.7b RF Remote Wake and Sleep
```json
{"action": "rf_wake", "slot": 1, "msgId": 81}
{"action": "rf_sleep", "slot": 2, "msgId": 82}
```
`POST /api/rf/wake` and `POST /api/rf/sleep` (7.2, 7.4) for slot `1` (default) or `2` (`400`
otherwise). `503` when `hub_rf` is not running; another refusal of `hub_rf` keeps its code
(`409` during an RF OTA).

#### 10.7c Sync
`{"action": "sync", "msgId": 83}` reloads the configuration from flash and marks the remotes'
configuration as waiting for a push, as `POST /api/sync` (4.10). The ack carries no other field.

#### 10.7d Raw Infrared
`{"action": "ir_raw", "frequency": 38000, "timings": [9000, -4500, 560, -560], "port": 1, "repeats": 1, "msgId": 84}`
takes the fields of `POST /api/ir-raw` (5.4). The ack carries `count` and `duration_ms`; an error
carries the code and `error` of the REST reply (`400` naming the field, `409` busy). A client frame
can be at most 4095 bytes, so send a long timing list with `POST /api/ir-raw` instead.

### Server-to-Client Messages (pushed events)

Every state change the Hub knows about goes through its event bus and is pushed to every
WebSocket client as one text frame. Each pushed message carries `type`, `seq` (sequence number,
from 1 per daemon run, increasing across all types) and `ts` (Unix seconds). Nothing is replayed to
a client that connects later: it fetches the current state over REST (or the `snapshot`, see
10.14). A client that fails a send is dropped.

#### 10.8 Activity State
Sent on every change of the orchestrator state, of the current activity or of the queued request
(4.3), including cancel and dry runs, and once at daemon start. Same shape as the `get_state` reply
and `GET /api/activity`. `queued` is the id of the request waiting behind the running transition,
`null` when none: a start or power off asked while `busy` is true sets it, the next request replaces
it, and it goes back to `null` when the queued request starts (the following event is its `starting`
or `stopping`) or is dropped.
```json
{
  "type": "activity_state",
  "seq": 17,
  "ts": 1762387200,
  "activity": "123456",
  "state": 1,
  "stateName": "starting",
  "busy": true,
  "sending": true,
  "queued": "-1"
}
```
- `state`: `0` idle, `1` starting, `2` running, `3` stopping; `stateName` is the same as a word.
- `activity` is `"-1"` when idle; while starting it is still the previous activity.

#### 10.9 Activity Progress
One frame per executed step of an activity switch (unchanged shape, now carrying `seq` and `ts`).
A step whose command could not be sent (unknown device -5, unknown command -6, IR or Bluetooth
send failed) carries `"error": <code>`; the `Transition completed` frame carries `failedSteps`,
the number of such steps. A new request no longer drops the commands that follow the Switched
marker of the transition before it: they are sent first, as steps named `Tail of the last switch:`.
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
Sent when a configuration file was written or replaced on disk (inotify watch on the config
folders, read once a second):
`devices`, `activities`, `settings` or `remote_map` (either slot's button map). Reload the
affected inventory over REST. One event names every file seen in that second. A file that is
written again with the same content, or only touched, gives an event too: the event does not mean
that the content differs.
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
`POST /api/button-press` from other sources give the same event with `source` `"api"` (the
Bluetooth remote's keys with `"bt_remote"`), and so does every command sent to an MQTT-type device
(8.1). `slot` is present only for a key of an RF remote.
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

#### 10.13b IR Received
One learned infrared signal while the IR capture mode (5.5) is on: `timings` are signed
microseconds (positive mark, negative space, at most 2048 values) of one frame, ending with the
gap to the next frame (40 ms when no second frame was seen); `modulation` is the measured carrier
in Hz, or `null` when there were too few carrier pulses to measure it. The same timings can be
sent back through `POST /api/ir-raw` (5.4). Not kept for the snapshot.
```json
{"type": "ir_received", "seq": 57, "ts": 1762387257, "modulation": 37700,
 "timings": [4537, -4518, 505, -505, 505, -1513, "...", 505, -55833]}
```

#### 10.13c IR Capture
The IR capture mode (5.5) turned on or off, or its receiver problem appeared, changed or cleared.
Same fields as `GET /api/status`: `ir_capture`, and `ir_capture_error` only while there is a
problem. The current state is also in the snapshot's `hub` object (10.14); this event is not kept
separately for the snapshot. On MQTT the change shows as a new retained `<base>/hub`.
```json
{"type": "ir_capture", "seq": 58, "ts": 1762387260, "ir_capture": true}
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
refetch over REST. The frame can exceed 4 KB, and outbound frames have no 64 KB limit (a frame
above 65535 bytes uses the 64-bit length form of RFC 6455).
```json
{
  "type": "snapshot",
  "msgId": 8,
  "hub": {"hub_name": "Living Room Hub", "hub_id": "20140101120000-000001", "version": "2.2.0",
          "api_version": 2, "capabilities": ["events", "snapshot", "ws_auth", "ws_keepalive", "ir_raw", "ir_capture", "mdns", "legacy", "mqtt_v2", "rf_wake"],
          "legacy_enabled": false, "mdns_enabled": true, "boot_time": 1762352379, "ir_capture": false,
          "auth_enabled": false, "uptime_sec": 34821},
  "activity_state": {"type": "activity_state", "activity": "-1", "state": 0, "stateName": "idle", "busy": false, "sending": false, "queued": null, "seq": 3, "ts": 1762387100},
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

The Hub never waits for a slow client: what the connection does not take at once is queued for it.
A client that has not taken its queue within 5 s, or whose queue would pass 256 KB, is closed.

---

## 11. Authentication & Security Architecture

Harmony Hub supports a multi-tier authentication model:

**Authentication is off after install.** It is on only once an account file
(`/data/harmony/config/users.json`, or the older `webui_auth.conf`) enables it, and the installers
create none. Until then every client on the LAN is admin on every route of port 8089, the
WebSocket included. To turn it on:
- web UI, System page, "User Accounts & Access Control": enter the admin user name and password,
  tick "Require authentication" and save;
- or `POST /api/system-action` with
  `{"action": "auth", "authUsername": "admin", "authPassword": "<password>", "authEnabled": true}`
  (refused with `400` while no admin account exists and no password is given).

Requests from the Hub itself (`127.0.0.1`, `::1`) are always admin. MQTT commands always run as
admin (8.1), and port 8088 (1.12) is never authenticated.

### 11.1 Accounts, Basic authentication and rules
- **Basic** (`Authorization: Basic`) is accepted on every route, under the same failed-login limit as
  `POST /api/auth/token`: after 3 wrong passwords from one address, that address is locked for 2 s,
  then 4, 8, 16, up to 30 s, for both. Digest authentication is not offered.
- Passwords are stored as SHA-256 of `user:Harmony Hub:password`; names must be shorter than 32 bytes
  (no `:` or line breaks), passwords at most 128 bytes.
- The account list always keeps at least one admin: `POST /api/users` answers `400` instead of
  demoting or replacing the last admin (no default account is ever created).
- Deleting an account, changing its role or password ends its sessions; deleting it also deletes the
  personal access tokens it created. Turning authentication on or off ends every session.
- Personal access tokens can be created only while authentication is on (`409` otherwise), carry the
  creating account as `owner`, end with it, and never have more rights than it.
- **Turning authentication on lists the accounts and access tokens to check.** Every request that
  turns authentication from off to on answers with every account except the one the request names,
  and every personal access token (no hashes): `POST /api/users` with `"auth_enabled": true` (the
  account it saves is not listed), `POST /api/system-action` with `"action": "auth"` and
  `authEnabled` (the admin it sets is not listed), an import (`POST /import`) of a users file with
  authentication on (every account listed); `hub_user.sh set-admin`, `add-user`, `delete` and
  `enable-auth` over SSH print the same list when they turn it on. This happens at every switch from
  off to on:
  ```json
  {"ok": true, "auth_enabled": true,
   "to_check": {"users": [{"username": "guest", "role": "admin"}],
                "tokens": [{"id": "tok_1a2b3c4d", "name": "Old script", "owner": "guest", "role": "admin", "created_at": 1729940000}]}}
  ```
  A token whose owner account is gone carries `"owner_missing": true`: it is already refused (a token
  ends with its account) and is removed from the file when an account of that name is made again.
  When the token file cannot be read at that moment, `to_check` also has `"tokens_unreadable": true`
  (its tokens are not listed; both web UIs say so).
  Both web UIs show the list right after the switch, with Delete per item (`DELETE /api/users`,
  `DELETE /api/auth/tokens`; deleting an account also deletes its tokens; one already gone counts as
  deleted) and Keep all; the account logged in is never offered. The switch ends every session, so
  the list is kept until an admin is logged in: the legacy UI asks for the login and shows it then,
  and its Users page can open the last list of the session again; the new web UI (whose only way to
  turn authentication on is a restore) keeps it for the browser session, shows the names with a
  hint to log in on the Users page while nobody is logged in, and the full list once an admin is.
  Nothing is stored on the Hub, kept or blocked: accounts and tokens that are not deleted keep
  working as before.

  **Limit:** while authentication is off anybody on the network is admin. This list is a check at
  the moment authentication is turned on, not a protection during the off period: what somebody did
  while it was off (an account or token made and used, a setting changed) is not undone, and only
  accounts and tokens can be listed. Keep authentication on.
- An operator (role `user`) may create its own tokens (always role `user`), logged in with its account
  (a session or Basic, not with a token: `403`), and may delete the tokens of its own account or the
  token it calls with (any other id answers `404`). An operator's `GET /api/auth/tokens` lists only
  the tokens of its own account when it logs in with that account (session or Basic). With a token,
  the token acts for its owner's account (list and delete that account's tokens, so an operator's app
  at the limit can free a slot) only when the token's role is `user` and its owner is an operator
  account; any other token with role `user` (for example one an admin made for Home Assistant) sees
  and deletes only itself. An admin sees every token; the list shows each token's `owner`.
- An operator account may create or delete at most 30 tokens a minute (each change rewrites the token
  file on flash): one more answers `429` with the seconds to wait.
- The token file is changed under one lock: a request that cannot get it within 3 s answers `503`
  ("try again").
- A token created by an admin with a token (for example from the Android app) belongs to the same
  account as the calling token and ends with it.
- At most 64 tokens on the Hub and 10 per operator account: one more answers `409` with the reason.
  If `config/tokens.json` exists but cannot be read or parsed (cut, or larger than 64 KB), creating,
  deleting and listing tokens answer `500` with a text that says so, and the file is left as
  it is (every token is refused until then). To start again, an admin logged in with an account (not
  with a token) sends `POST /api/auth/tokens-reset` with `{"confirm": true}`: the file is moved to
  `config/tokens.json.bad` and an empty list starts; every token (Home Assistant, the Android app)
  then has to be created again. A readable file is never reset (`409`: delete tokens one by one).
  `tokens.json.bad` stays on `/data` (the next reset replaces it); delete it over SSH once you have
  looked at it. While another request holds the token list, these routes answer `503` ("busy, try
  again"), never the text about an unreadable file.
  Creating an account whose name a deleted account had (`POST /api/users`, the admin name in
  `POST /api/system-action` `"action": "auth"`, an imported users file, or `hub_user.sh add-user` /
  `set-admin`) removes that account's tokens left in the file first; while the file cannot be changed
  the name cannot be used (`500`, or `503` while busy; an import says so in a `warning`).
  `hub_user.sh delete` removes the account's tokens too. The `"action": "auth"` rename refuses a name
  another account has (`409`).
- A new password ends the account's sessions, not its tokens (Home Assistant and the Android app keep
  working). To lock out a lost phone, delete its token (named `Android app (<phone>, <date>)`).
- If `config/users.json` exists but cannot be read (cut, empty, broken) and `webui_auth.conf` has no
  account either, every request that needs a login (all API routes except `POST /api/auth/token`;
  the web UI files stay open) is refused (`401`), except from loopback, until the file is fixed.
- `GET /api/mqtt-status` never returns the stored broker password (`config.has_password` instead).

```bash
curl -u admin:secret123 http://192.168.1.1:8089/api/status
```

### 11.2 OAuth2 Bearer Session Tokens (RFC 6749 / RFC 6750)
Recommended for SPAs, WebUI, and Mobile Apps. The login is a plain JSON request with `username`
and `password`, not an RFC 6749 token endpoint (there are no grant types); the token it returns is
used as an RFC 6750 Bearer token. A session lasts 24 hours (`expires_in`). The Hub keeps at most
32 sessions: one more replaces the session used least recently. A wrong name or password answers
`401` `{"ok": false, "error": "Invalid username or password"}`; while the address is locked after
failed logins (11.1) the answer is `429` with `retry_after`, the seconds to wait. While
authentication is off the login gives an admin session for any name and password.

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

The Android app uses one too: when you log in, it creates a token named
`Android app (<phone model>, <date>)` for your account and keeps only that token, never the
password. Logging out (or deleting the Hub profile) deletes the token on the Hub when the Hub can
be reached.

```bash
# 1. Generate new Personal Access Token (an operator gets role user, see 11.1)
curl -X POST -H "Authorization: Bearer hht_..." \
  -H "Content-Type: application/json" \
  -d '{"name":"Home Assistant","role":"admin"}' \
  http://192.168.1.1:8089/api/auth/tokens

# Response (201 Created; the id is tok_ + 8 hex digits, the token pat_ + 32 hex digits):
# {"ok":true,"id":"tok_6a1d8f3c","name":"Home Assistant","role":"admin","token":"pat_3f8a9b..."}

# 2. Authenticate Home Assistant REST sensor / command
curl -H "Authorization: Bearer pat_3f8a9b..." http://192.168.1.1:8089/api/current_activity

# 3. List active tokens
curl -H "Authorization: Bearer hht_..." http://192.168.1.1:8089/api/auth/tokens

# 4. Revoke a token
curl -X DELETE -H "Authorization: Bearer hht_..." "http://192.168.1.1:8089/api/auth/tokens?id=tok_6a1d8f3c"
```
The token itself is shown only in the answer to its creation: the Hub stores a hash. `name`
defaults to `Personal Access Token` (at most 63 characters); `role` is `admin` unless it is
`"user"`, and always `user` for a token created by a `user` account.

### 11.4 WebSocket Token Authentication
When authentication is enabled, the WebSocket upgrade (`GET /ws`) must carry a token: either
`Authorization: Bearer <token>` or, since browsers cannot set headers on a WebSocket, `?token=<token>`
in the request line:
`ws://192.168.1.1:8089/ws?token=pat_3f8a9b...`

- Personal access tokens (`pat_...`, 11.3) and session tokens (`hht_...`, 11.2) are both accepted.
  Basic credentials in an `Authorization` header are accepted on the upgrade as on every route.
- Missing, unknown or revoked token: `401 Unauthorized` with the usual challenge; the connection is
  closed, never upgraded. Callers on the Hub itself (`127.0.0.1`, `::1`: `hub_rf`, `hub_btstack`)
  need no token, as for every request. With authentication disabled nothing changes.
- The role of the token is kept for the connection. A `user` role receives every pushed message
  and may send `ping`, `get_state` and `get_snapshot`; its command actions follow the same
  permission table as the REST equivalents (11.5) (`send_command` = `/api/ir-send`, `start_activity` =
  `/api/activity`, `stop_activity` = `/api/poweroff`, `cancel_activity` = `/api/activity-cancel`,
  `bt_key`, `bt_text`, `bt_mouse` = their endpoints). A refused action is answered with
  `{"type": "ack", "action": ..., "ok": false, "code": 403, "error": "Forbidden: Admin access required"}`
  (with the request's `msgId`).
- Both web UIs pass their session token as `?token=` when they open the WebSocket.
- Capability flag: `ws_auth`.

### 11.5 Roles per route
With authentication on, each request is checked before its route runs:
- **Open without credentials**: `POST /api/auth/token` (login), `GET /`, `GET /health`, and the web
  UI's static files (every `GET` or `HEAD` outside `/api/`, `/ws` and `/export/`, `/assets/...`
  included). Everything else under `/api/`, `/ir/`, `/bt/`, `/export/`, and `/import`, `/wifi`,
  `/ethernet`, `/system`, `/mqtt` needs an account or a token (`401` otherwise). The open routes
  keep answering while the account file cannot be read (11.1): only the others are refused then.
- **`user` role**: only the routes below; any other route, method or alias answers `403`
  `{"ok":false,"error":"Forbidden: Admin access required"}`. The check compares the exact path, so
  an alias that is not listed (for example `/api/interlock-reset`, `/api/activity-reset`,
  `/ir/commands-batch`) needs the admin role even when its twin is listed.

  | Method | Paths |
  | --- | --- |
  | `GET`, `HEAD` | `/api/status`, `/api/auth/me`, `/api/activities`, `/api/activity`, `/api/current_activity`, `/api/devices`, `/api/inventory`, `/api/icons`, `/api/icons/...`, `/api/icons/archive`, `/api/custom-icons`, `/api/ui-remote-layout`, `/assets/...` |
  | `POST` | `/api/auth/logout`, `/api/activity`, `/api/activity-start`, `/api/activity-stop`, `/api/poweroff`, `/api/activity-cancel`, `/api/ir-send`, `/ir/send`, `/api/ir-raw`, `/api/button-press`, `/api/mqtt-publish`, `/api/bt-key`, `/api/bt-text`, `/api/bt-mouse`, `/api/v1/bt/mouse` |

  `/api/bt-key` and `/api/bt-text` are not REST routes: the two entries only decide whether a
  `user` may send the WebSocket `bt_key` and `bt_text` actions.

- **`admin` role**: every route.
- **WebSocket**: a `user` token may send `ping`, `get_state`, `get_snapshot`, and the actions whose
  REST twin is in the `POST` row: `send_command`/`ir_send` (`/api/ir-send`), `start_activity`
  (`/api/activity`), `stop_activity`/`power_off` (`/api/poweroff`),
  `cancel_activity`/`reset_interlock` (`/api/activity-cancel`), `bt_key`, `bt_text`, `bt_mouse`,
  `ir_raw`. `ir_capture`, `rf_wake`, `rf_sleep`, `sync`, `update_check`, `update_apply`,
  `activity_help` and `activity_help_answer` are admin only.
- A personal access token never has more rights than the account that created it (11.3).

### 11.6 Account and token routes
- `POST /api/auth/token` `{"username", "password"}`: login, returns a session token (11.2). Open
  without credentials; wrong passwords count for the failed-login limit (11.1).
- `GET /api/auth/me`: `{"ok", "auth_enabled", "authenticated", "username", "role"}` of the caller.
- `POST /api/auth/logout`: ends the caller's session.
- `GET /api/auth/tokens`, `POST /api/auth/tokens` `{"name", "role"}`, `DELETE /api/auth/tokens?id=<id>`:
  list, create and revoke personal access tokens (11.3). The list is
  `{"ok": true, "tokens": [{"id", "name", "role", "created_at", "last_used_at"}]}` (times as Unix
  seconds, never the token itself). The delete answers `400` without an id and `404` for an
  unknown one.
- `GET /api/users`: `{"ok", "auth_enabled", "users": [{"username", "role"}]}`.
- `POST /api/users` `{"username", "password", "role": "admin"|"user", "auth_enabled"}`: creates an
  account (a password is required) or changes one (a missing password keeps it); `auth_enabled`,
  when given, turns authentication on or off. `role` is `user` for a new account when left out.
  The Hub keeps at most 16 accounts. Reply `{"ok": true, "message": "User account saved"}`. `400`
  when no admin would be left, for an empty or unusable name, a password over 128 bytes, a new
  account without a password, or a 17th account.
- `POST /api/system-action` `{"action": "auth", "authEnabled": true, "authUsername": "admin", "authPassword": "<password>"}`
  (1.10): sets the admin account and turns authentication on or off in one request. A request
  without `authEnabled` keeps the present state. With an admin account stored, `authUsername`
  renames it (the password must be sent with it) and `authPassword` changes its password; without
  one, the request creates it (the name defaults to `admin`). Reply
  `{"ok": true, "auth_enabled": true, "message": "Authentication settings updated"}`. `400`: a name
  of 32 bytes or more or with `:` or a line break, a password over 128 bytes, a rename without the
  password, or turning authentication on while no admin account exists and no password is given.
  `500` when the accounts could not be stored.
- `DELETE /api/users?username=<name>` (also `POST /api/users/delete` `{"username"}`): `404` for an
  unknown name, `400` for the only admin.
