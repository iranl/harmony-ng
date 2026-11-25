# HarmonyNG Plugin Architecture & Developer Guide

HarmonyNG features a lightweight, decentralized **Plugin System & Community Marketplace**. This allows adding community-maintained device integrations (such as **Roku ECP**, **PlayStation 5 Bluetooth Wake**, and **Android TV / Google TV**).

Official plugin repository: [**https://github.com/iranl/harmony-ng-plugins**](https://github.com/iranl/harmony-ng-plugins)

---

## Table of Contents

1. [Architecture & Design Principles](#architecture--design-principles)
2. [User Guide](#user-guide)
   - [Accessing the Marketplace](#accessing-the-marketplace)
   - [Installing & Managing Plugins](#installing--managing-plugins)
   - [Adding Community Repositories](#adding-community-repositories)
   - [Creating a Plugin Device in Device Studio](#creating-a-plugin-device-in-device-studio)
3. [Developer Guide](#developer-guide)
   - [Plugin Package Structure](#plugin-package-structure)
   - [Manifest Specification (`manifest.json`)](#manifest-specification-manifestjson)
   - [Process Lifecycle & Supervisor](#process-lifecycle--supervisor)
   - [UNIX Domain Datagram IPC Protocol](#unix-domain-datagram-ipc-protocol)
   - [Pairing Protocol](#pairing-protocol)
   - [Bluetooth Helpers (`BLEADV:`, `adv`, `bt_page`)](#bluetooth-helpers-bleadv-adv-bt_page)
   - [Action Lifecycle & Debounce Rules](#action-lifecycle--debounce-rules)
   - [Target Architecture & Cross-Compilation](#target-architecture--cross-compilation)
   - [Packaging & Testing](#packaging--testing)
   - [Publishing to a Repository](#publishing--distribution)
   - [Security](#security)
4. [Reference Plugins](#reference-plugins)

---

## Architecture & Design Principles

### 1. Minimal Flash Space & Low Overhead
The Harmony Hub (Atheros AR9344 SoC, MIPS 24Kc) operates with limited flash and RAM. To prevent memory exhaustion and binary bloat:
- Core `hub_daemon` remains a micro-daemon (< 650 KB).
- Plugins run as independent, standalone processes (lightweight shell scripts, micro-daemons in C/C++, or small interpreters).
- No statically linked runtimes (Python, Node.js, or heavy frameworks).

### 2. Zero Flash Wear During Operation
The Hub uses JFFS2 on SPI NOR flash, which is shared with the CC2544 RF transceiver. Writes to NOR flash are slow and cause flash wear.
- All plugin runtime artifacts (UNIX sockets, logs, status files) live exclusively in **RAM tmpfs** under `/tmp/plugins/`.

### 3. Non-Blocking, Isolated IPC
- Commands are dispatched from `hub_daemon` to plugins via **UNIX domain datagram sockets** (`AF_UNIX`, `SOCK_DGRAM`) at `/tmp/plugins/<plugin_id>.sock`.
- The send never waits (`MSG_DONTWAIT`). If a plugin is unresponsive or stopped, the core daemon continues immediately without locking the activity orchestrator or remote sync.

### 4. Process Supervisor & Crash Recovery
- `hub_daemon` starts each plugin itself (`fork()` and `exec` of the manifest's `entrypoint`) and keeps its pid. No pid file decides what runs or what is killed.
- `hub_daemon` checks enabled plugins every 10 seconds via a background watchdog tick, which reaps an exited plugin (`waitpid`) and logs its exit status.
- If an enabled plugin crashes, it is automatically restarted. A plugin that keeps dying is restarted with a growing gap (20 s, 40 s, … up to 5 minutes); the gap resets once it stays up for a watchdog period. `GET /api/plugins` reports the count as `restarts`.
- A plugin stopped on request (stop, disable, uninstall) is not restarted by the watchdog; `GET /api/plugins` reports it as `stopped`. Start, restart or enable clears it.
- When plugins are stopped or restarted, `hub_daemon` follows strict **Rule 1 termination discipline**: `SIGTERM` to the plugin's process group (the plugin and any helper it started), up to 1 second for the plugin to exit, then `SIGKILL` to the plugin itself (not the group). A helper of the group keeps its `SIGTERM` and is not signalled again while it runs its `EXIT` trap; it gets `SIGKILL` only if it is still there 8 seconds later. A shutdown signals all plugins at once, so it takes at most 2 seconds in total.
- One start, stop, restart, enable or disable per plugin at a time: a second one while the first runs answers HTTP 409 and changes nothing.
- Plugin processes `hub_daemon` does not track (helpers of a plugin that died, a plugin left by a `hub_daemon` that was killed) are found by the `HARMONY_PLUGIN_ID` and `HARMONY_SOCKET` in their environment. Before the plugin starts again, and on stop and uninstall, they get one `SIGTERM`, and `SIGKILL` 8 seconds later if they are still there; nothing waits for them (a watchdog restart does not hold the main loop). At `hub_daemon` start they get the same `SIGTERM`, and the plugins start without waiting for them. `hub_daemon` itself and the processes that started it are never taken. `init.sh stop` stops `hub_daemon` with `SIGTERM` (which stops its plugins), gives plugin processes still left up to 5 seconds to exit, then sends `SIGTERM`, and `SIGKILL` 1 second later.
- An update interrupted by a power cut (between the swap of the old and the new folder) is put back in place when `hub_daemon` starts.

---

## User Guide

### Accessing the Marketplace
Open the HarmonyNG Web UI or Android Companion App:
1. Tap the **Menu Drawer** in the upper left.
2. Select **Plugins & Marketplace** (or tap the **Plugins & Marketplace** card on the Settings Overview screen).

### Installing & Managing Plugins
The Plugins view has three tabs:
- **Installed**: Shows all installed plugin daemons, their running state, PID, version, and controls:
  - **Enable / Disable Toggle**: Disabling a plugin prevents it from starting on boot.
  - **Start / Stop / Restart**: Instantly control the live process.
  - **Uninstall**: Stops the daemon and removes its folder, its state in `/data/harmony/plugin-data/<id>` (pairing keys) and its log. Refused while a device uses the plugin.
- **Marketplace**: Browse community plugins from configured repositories.
  - Click **Install** for 1-click installation.
  - If a newer version is released, an **Update** button appears automatically.
  - **Dual-Path Resilience**: The installer attempts direct download on the Hub. If the Hub cannot download the archive (HTTP 502: no route to the host), the WebUI downloads it in the browser and uploads it with the catalog's SHA-256, which the Hub checks. This works only where the host allows cross-origin reads (`raw.githubusercontent.com` does, GitHub release assets do not). A refusal (checksum, authentication, flash space) or a timeout is never retried from the browser.
- **Upload Archive**: Click **Upload Archive (.tar.gz)** in the top bar to install a custom plugin `.tar.gz` package directly. An upload without a SHA-256 is a dev install. The archive may be at most 1 MB.
- **Authentication**: a plugin runs as root on the Hub. Install, pull and repository changes are refused (HTTP 403) while authentication is off. All plugin routes, `GET /api/plugins` included, need the admin role.
- **Space**: the Hub checks the unpacked size against the free space of `/data` (256 KB stay free for the configuration) and of `/tmp` (RAM) before it unpacks. The new version is copied next to the installed one and swapped in only when every file was written: a failed install leaves the installed version as it was. One install runs at a time (HTTP 409 for a second one).

### Adding Community Repositories
1. Go to the **Repositories** tab in the Plugins view.
2. Enter the URL of a community `repository.json` catalog (e.g. `https://raw.githubusercontent.com/.../repository.json`).
3. Tap **+ Add Repo**. The Marketplace catalog updates immediately.

### Creating a Plugin Device in Device Studio
Once a plugin is installed (e.g. `roku-ecp`, `android-tv`, or `ps5-wake`):
1. Navigate to **Device Studio** -> **Add Device**.
2. Open the **Device Type** dropdown: installed plugins appear under the **Community Plugins** group (e.g. `Roku Player / TV (Plugin)`).
3. The UI automatically displays the custom configuration fields defined by the plugin (e.g. *IP Address*, *Port*, *Bluetooth MAC*).
4. Tap **Create Device**. Default commands are automatically generated and linked to the plugin.
   The device is stored with `"plugin": "<id>"` (and type `plugin:<id>` or its own type text). Only that field, or a type `plugin:<id>`, makes a device a plugin device: a free-text type that happens to match a plugin's `device_type` or `display_name` does not.
5. The device can now be assigned to any Activity, physical button, or Home Assistant automation.

---

## Developer Guide

### Plugin Package Structure
A plugin distribution is a compressed archive (`.tar.gz` or `.tgz`) containing:

```
my-plugin/
├── manifest.json          # Plugin metadata, device definitions, configuration schema
└── bin/                   # Binaries, scripts, or assets
    └── my_daemon          # The entrypoint: started and stopped by hub_daemon
```

### Manifest Specification (`manifest.json`)
The `manifest.json` file defines the plugin identity and how it appears in the WebUI and Device Studio:

```json
{
  "id": "roku-ecp",
  "name": "Roku ECP Controller",
  "version": "1.0.0",
  "author": "Harmony-NG Community",
  "description": "Control Roku streaming boxes and Roku TVs over local Wi-Fi via External Control Protocol (ECP)",
  "homepage": "https://github.com/iranl/harmony-ng-plugins",
  "entrypoint": "bin/roku_ecp",
  "device_definition": {
    "device_type": "roku",
    "display_name": "Roku Player / TV",
    "config_fields": [
      {
        "key": "ip_address",
        "label": "IP Address",
        "type": "string",
        "placeholder": "192.168.1.50"
      },
      {
        "key": "port",
        "label": "Port",
        "type": "number",
        "default": 8060
      }
    ],
    "default_commands": [
      { "name": "Home", "keyCode": "PLUGIN:roku-ecp:Home" },
      { "name": "Back", "keyCode": "PLUGIN:roku-ecp:Back" },
      { "name": "Select", "keyCode": "PLUGIN:roku-ecp:Select" },
      { "name": "DirectionUp", "keyCode": "PLUGIN:roku-ecp:Up" },
      { "name": "DirectionDown", "keyCode": "PLUGIN:roku-ecp:Down" },
      { "name": "DirectionLeft", "keyCode": "PLUGIN:roku-ecp:Left" },
      { "name": "DirectionRight", "keyCode": "PLUGIN:roku-ecp:Right" },
      { "name": "Play", "keyCode": "PLUGIN:roku-ecp:Play" },
      { "name": "PowerOff", "keyCode": "PLUGIN:roku-ecp:PowerOff" }
    ]
  }
}
```

#### Manifest Fields Reference

| Field | Type | Description |
| :--- | :--- | :--- |
| `id` | `string` | **Required.** Unique identifier (alphanumeric, hyphens, underscores; 1–63 chars). Must match directory name. |
| `name` | `string` | Human-readable plugin title. |
| `version` | `string` | **Required.** Semantic version string (e.g. `1.0.0`). |
| `author` | `string` | Author or organization name. |
| `description` | `string` | Short summary of plugin capabilities. |
| `homepage` | `string` | URL to source repository or documentation. |
| `entrypoint` | `string` | **Required.** Relative path of the binary or script the Hub runs (it must be in the archive). |
| `files` | `array` | *(Dev installs)* Relative paths the dev install downloads next to `manifest.json`. |
| `pairing` | `object` | *(Optional)* Pairing: `supported`, `type` (`pin_display`, the default, `confirm` or `listen`), `requires_host` (default true; false for `listen`), `timeout_s` (default 15, at most 60) and the dialog texts (`title`, `button_label`, `description`, `instructions`, `host_label`, `prompt`, `pin_length`). See [Pairing Protocol](#pairing-protocol). |
| `device_definition` | `object` | *(Optional)* Schema for devices created using this plugin. `config_fields`, when present, is an array of objects, each with a string `key`. A `default_commands` entry may have `"range": [min, max]`: the integer values the command takes (see `value` below), and an infrared alternative, `irKeyCode` and `irWhen` (see [Infrared alternative of a command](#infrared-alternative-of-a-command-irkeycode-irwhen)). |
| `requires_libs` | `array` | *(Optional)* File names of the Hub's shared libraries the binary links, e.g. `["libbearssl.so.0"]` (see "Using TLS: the shared BearSSL" below). |

The Hub refuses an install whose manifest has no safe `id`, no `version`, no valid `entrypoint` (or one not in the archive), a malformed `config_fields`, a `default_commands` range that is not two integers with min <= max, a `default_commands` `irKeyCode` that is not a non-empty string or `irWhen` that is not a config key (letters, digits, `_` or `-`, at most 63), or a `requires_libs` entry that is not in `/data/harmony/lib` (a Hub on older firmware: update it first).

#### Config Field Types

| Type | Description | Rendered Input |
| :--- | :--- | :--- |
| `"string"` | Text value (e.g. IP address, host, MAC) | `<input type="text">` |
| `"number"` | Numeric integer value (e.g. port, timeout) | `<input type="number">` |
| `"password"` | Sensitive string (e.g. auth token, PIN) | `<input type="password">` |
| `"boolean"` | True / false toggle, saved as a JSON `true` / `false` (a new device gets `false` unless the field has a `default`) | `<input type="checkbox">` |

---

### Process Lifecycle & Supervisor

`hub_daemon` starts the `entrypoint` itself:

- in the plugin folder (`/data/harmony/plugins/<id>`), in its own session and process group;
- with the socket path as `argv[1]` and the environment below;
- stdin `/dev/null`, stdout and stderr to `/tmp/plugins/<id>.log` (written new at each start, appended to; `/tmp` is RAM, so `hub_daemon` empties every `*.log` of `/tmp/plugins` over 256 KB, checked every second, also while the plugin is not running);
- default signal handlers and no blocked signals (`hub_daemon` itself ignores `SIGPIPE`);
- none of `hub_daemon`'s sockets or files.

The entrypoint must **stay in the foreground** for as long as the plugin runs: do not fork into the background, do not write a pid file. When it exits, the next watchdog tick (every 10 s) sees it and restarts it (with the backoff above); an entrypoint that daemonizes is therefore started again at each backoff period, its earlier copy stopped each time. To stop it, `hub_daemon` sends `SIGTERM` to its process group, waits up to 1 second, then sends `SIGKILL` to the entrypoint; its helpers get `SIGKILL` 8 seconds after the `SIGTERM` if still there.

A restart, an update and a watchdog restart start the new instance without waiting for the old helpers: an old helper's `EXIT` trap can still run next to the new instance. An update or an uninstall removes the plugin folder while that trap runs, so a trap must not call files of the plugin folder, and must not remove a lock the new instance may have taken (create the lock with `set -C` and remove it only when it holds your own pid). At most 32 such processes are tracked at a time; more get `SIGKILL` at once.

Every process the plugin starts inherits `HARMONY_PLUGIN_ID` and `HARMONY_SOCKET` (keep them in the environment of your helpers). `hub_daemon` uses them to find plugin processes it does not track: helpers left when the plugin itself died, a copy that daemonized against the rule above, plugins of a `hub_daemon` that was killed. These get one `SIGTERM`, then `SIGKILL` if they are still there 8 seconds later: before the plugin starts again, on stop and uninstall, and at `hub_daemon` start (`init.sh stop` does the same). Keep a helper's cleanup (an `EXIT` trap) under 8 seconds. A program you start by hand in a plugin folder has neither variable and is left alone.

```c
int main(int argc, char **argv) {
    const char *sock_path = argc > 1 ? argv[1] : getenv("HARMONY_SOCKET");
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    /* unlink(sock_path); bind(fd, sock_path); then one poll() loop until SIGTERM */
}
```

#### Plugin Data
The plugin directory (`/data/harmony/plugins/<id>`) is replaced on every install, update and pull. Keep state that must survive an update (pairing, keys) in `/data/harmony/plugin-data/<id>/`.

#### Environment Variables Passed to Plugins

- `HARMONY_PLUGIN_ID`: The unique plugin ID (e.g. `roku-ecp`).
- `HARMONY_SOCKET`: The absolute path for the UNIX domain datagram socket (`/tmp/plugins/<id>.sock`).
- `HARMONY_PLUGIN_DATA`: A directory for state that must survive updates (`/data/harmony/plugin-data/<id>`, created before the start). An install keeps a plugin's disabled state.
- `PATH`: Extended with `/cache/bin:/data/harmony/bin:$PATH` to access modern BusyBox applets.

---

### UNIX Domain Datagram IPC Protocol

When a user triggers an action (via WebUI, touchscreen remote, Companion remote, or MQTT), `hub_daemon` formats a JSON payload and sends it as an `AF_UNIX` datagram (`SOCK_DGRAM`) to `/tmp/plugins/<id>.sock`.

#### Datagram JSON Schema

```json
{
  "plugin_id": "roku-ecp",
  "device_id": "1",
  "device_name": "Living Room Roku",
  "hub_name": "Harmony Living",
  "command": "Home",
  "action": "tap",
  "value": 0,
  "ts": 123456789,
  "config": {
    "ip_address": "192.168.1.50",
    "port": 8060
  }
}
```

#### Payload Fields

| Key | Type | Description |
| :--- | :--- | :--- |
| `plugin_id` | `string` | Target plugin ID receiving the command. |
| `device_id` | `string` | Hub internal device ID. |
| `device_name` | `string` | User-defined display name of the device. |
| `hub_name` | `string` | The Hub's name from its settings (`hub_name`, "Hub Name" in the web UI), or `Harmony-NG Hub` when none was set (also for the old stored default `Harmony Hub`, the stock Logitech name). A plugin that registers the Hub on its device under a name (a Samsung TV's list of allowed devices) uses this one, so the user sees the Hub as they named it. A rename reaches the next datagram, without a restart. Older firmware sends no `hub_name`: fall back to `Harmony-NG Hub`. |
| `command` | `string` | Command name to execute (e.g. `Home`, `PowerOff`, `VolumeUp`). |
| `action` | `string` | Action lifecycle verb: `"tap"`, `"press"`, `"repeat"`, or `"release"`. |
| `value` | `number` | Numeric argument of the command, 0 when none was given. Set by `POST /api/ir-send` (`"value"`) and by an MQTT `command/<device>/<command>` payload that is a plain integer. A command with `"range": [min, max]` only receives values in that range: the Hub refuses others. |
| `ts` | `number` | When `hub_daemon` queued the datagram: `CLOCK_MONOTONIC` in milliseconds (same clock in the plugin). A plugin may drop a `repeat` that waited too long. |
| `config` | `object` | Configuration dictionary defined by the user in Device Studio. A device without a `config` object sends its own fields here, except `commands`. |

#### Delivery
`hub_daemon` sends with `MSG_DONTWAIT` and never waits for a plugin: when the plugin's socket queue is full (a plugin stuck on a slow or unreachable device), the datagram is dropped and logged as `Plugin '<id>' busy (queue full)`. A plugin must keep its receive loop short and must not block on network I/O for long.

#### KeyCode Overrides
For custom keybindings, commands can specify a direct keycode override in the format:
```
PLUGIN:<plugin_id>:<subcommand>
```
When present, `hub_daemon` routes the command directly to `<plugin_id>` and sets the `command` field in the JSON payload to `<subcommand>` (the whole rest of the keyCode, any length: e.g. `PLUGIN:android-tv:app:https://www.netflix.com/title/80057281`).

#### Device fields
A plugin device in `devices.json` (and in `POST /api/device-save`) has `"plugin": "<id>"` and a `"config"` object with the values of the plugin's `config_fields`. A top-level key is copied into `config` only when the plugin's `config_fields` declare it; the UIs send `config` as one JSON field. A `plugin` that is not a safe id is refused (HTTP 400).

#### Infrared alternative of a command (`irKeyCode`, `irWhen`)
Some devices ignore some commands over their network API: Samsung TVs from 2018 on ignore the HDMI 1-4 keys. A command of a plugin device (in `default_commands` and in the device's `commands`) may name an infrared code the Hub sends instead:

| Field | Type | Description |
| :--- | :--- | :--- |
| `irKeyCode` | `string` | An infrared code, anything an IR device's command `keyCode` may hold: a Harmony keycode (`G:<protocol>:(...)(...)():N`), a learned raw code (`F...P...S...`) or Pronto hex (`0000 ...`). |
| `irWhen` | `string` | The `key` of a `"type": "boolean"` field of the plugin's `config_fields` (letters, digits, `_` or `-`, at most 63). |

While the device's `config[irWhen]` is on (JSON `true`, a number other than 0, or the text `"true"` or `"1"`), `hub_daemon` sends the command as infrared, exactly as a command of an IR device: the same encoder, press / hold / release handling (a held key repeats until the release), `repeats` (3 when the command has none) and blaster ports (the command's own `port` 1-7, else all blasters). The plugin gets nothing for that command. A command with a `range` needs no `value` while its infrared alternative is on: the infrared send takes none. When the field is off or missing, or the command has no `irKeyCode`, the command goes to the plugin as before. A command with an `irKeyCode` but no `irWhen` always goes to the plugin: the plugin decides which config field turns the infrared code on, and the user turns it on with the checkbox the device settings show for that field.

```json
"config_fields": [
  { "key": "host", "label": "TV address", "type": "string" },
  { "key": "hdmi_via_ir", "label": "Send the HDMI keys as infrared", "type": "boolean" }
],
"default_commands": [
  { "name": "HDMI 2", "keyCode": "PLUGIN:samsung-tv:KEY_HDMI2",
    "irKeyCode": "G:GoVideoO1 32 Bit:()(0xE0E07D82)():3", "irWhen": "hdmi_via_ir" }
]
```

The Hub keeps both fields through device saves, command saves and reloads (a save that sends a command back without them keeps the stored ones; `null` or `""` removes them) and copies them from the manifest when a device or a preset command is created from `default_commands`. Firmware older than this feature ignores the two fields: the command then goes to the plugin, so a plugin should still handle it (or report it as unsupported).

---

### Pairing Protocol

`POST /api/plugins/pair` (`{"id", "action": "start" | "pin", "host", "pin", "port"}`, `port` optional) starts the plugin if needed, removes `/tmp/plugins/<id>.status`, and sends one datagram:

```json
{ "plugin_id": "android-tv", "hub_name": "Harmony Living", "command": "pair", "action": "start_pair", "host": "192.168.1.60" }
```

`action` is `finish_pair` with `pin` for the second step. The plugin answers by writing `/tmp/plugins/<id>.status` (write a temporary file, then `rename()` it: the Hub polls the file every 100 ms for up to 15 s and must not read half of it):

```json
{ "ok": true, "action": "start_pair", "stage": "pin_required", "message": "Enter the code shown on the TV", "paired": false, "host": "192.168.1.60" }
```

The route returns this object, with HTTP 400 when `ok` is false. Keep pairing keys in `HARMONY_PLUGIN_DATA`.

The datagram also carries `timeout_s`, the manifest's `pairing.timeout_s` (default 15, at most 60): how long the Hub waits.

#### Pairing without a host (`"type": "listen"`)
A plugin that finds its device itself (it hears the device's own radio packets, for example with `adv_listen` below) declares `"pairing": {"supported": true, "type": "listen", "requires_host": false, "timeout_s": 30}`. Then:

- `POST /api/plugins/pair {"id", "action": "start"}` needs no `host`. The datagram has no `host` either. The route waits at most 1 s for a first status and answers `{"ok": true, "stage": "listening", "timeout_s": 30}` when there is none, or `ok: false`, `stage: error` at once when the plugin is not running or its socket takes no datagram. A start or PIN step of a `pin_display` pairing waits 15 s at most, whatever `timeout_s` says. The status file is read up to 4,096 bytes: a longer one reads as `waiting`.
- `{"id", "action": "status"}` returns the current status file and sends nothing (`stage: waiting` while there is none). The UIs poll it every second.
- The plugin ends with `stage: done` (or `ok: false` and `stage: error`). A `config` object in the status holds what it learned: the UIs fill the device's config fields with it, and the user saves the device. Only keys the manifest's `config_fields` declare, with string, number or bool values, are returned.

```json
{ "ok": true, "action": "start_pair", "stage": "done", "message": "Switch 2 found", "config": { "switch_mac": "98:E2:55:99:1F:97", "controller_mac": "11:22:33:44:55:66" } }
```

#### Pairing confirmed on the device (`"type": "confirm"`)
For a device that asks the user to accept the Hub on its own screen (a Samsung or LG TV) and then gives the plugin a token: `"pairing": {"supported": true, "type": "confirm", "timeout_s": 45}`. It runs as `listen` above (the route answers within 1 s, here with `stage: confirm_on_device`, and the UIs poll `status`), but a `host` is needed (the device's `host` config field) and no PIN is asked. Keep the token the device gives in `HARMONY_PLUGIN_DATA`, not in the status `config`: `config` is for values the user may see and edit.

---

### Bluetooth Helpers (`BLEADV:`, `adv`, `bt_page`)

- **`BLEADV:<hex>`** keyCode: a command whose keyCode is `BLEADV:` and up to 31 bytes of legacy advertising data (AD structures, hex) is broadcast by `hub_btstack`; `hub_daemon` checks the payload and sends `adv <hex>` to it. No plugin process is involved.
- **`adv <hex> [ms] [addr=AA:BB:CC:DD:EE:FF]`**: a text datagram a plugin may send to `/tmp/hub_btstack.sock`. Non-connectable, 100 ms interval (the shortest Bluetooth 4.0 allows), for `ms` (200 to 10000, default 1200). Broadcasts run one after another (8 queued at most). With `addr=` the packet goes out from that address, set as the LE random address while it is on air, then the Hub's public address is back; meanwhile the BLE remote does not connect (it does once the broadcast ends).
- **`adv_listen <plugin_id> <seconds> [hex_filter]`**: a passive LE scan for `seconds` (1 to 60; `0` stops early). Each advertising report whose data contains `hex_filter` (at most 16 bytes, anywhere in the data; none: every report) is sent to `/tmp/plugins/<plugin_id>.sock`:
  ```json
  { "plugin_id": "switch2-wake", "command": "adv_report", "action": "tap", "value": 0, "addr": "AA:BB:CC:DD:EE:FF", "addr_type": 0, "event_type": 3, "rssi": -60, "data": "0201061BFF5305..." }
  ```
  Repeats are not filtered: the plugin drops what it saw already. The scan runs at 50 % (interval 60 ms, window 30 ms) and pauses while a Bluetooth host is paged. One plugin listens at a time; a listen is refused while a remote scan or pairing runs, and a remote scan or pairing started by the user ends a listen. Meanwhile the BLE remote reconnect waits; when the paired remote advertises (a key pressed), the listen ends so it can connect, and a new listen is refused until it is connected (15 s at most). If it is not ready once in those 15 s (it cannot connect: bond lost, or another device sends from its address), its advertising does not end the next listen, until that listen ends (60 s at most, renewals included) or the remote is ready: such a remote ends every second listen, not every one.
- **Older firmware** answers neither: it ignores `adv_listen`, and sends an `adv ... addr=` from the Hub's own address. With `addr=` the address must be six hex pairs with `:` (not all zero); a broadcast from another address waits while the BLE remote connects or a remote pairing window is open (plain broadcasts queued behind it go first), and is dropped after 10 s of waiting.
- **`bt_page <target_mac> [spoof_mac]`**: a text datagram a plugin may send to `/tmp/hub_btstack.sock`. `bt_page` pages the target (optionally from a spoofed BD_ADDR, restored afterwards) and disconnects at once. One page runs at a time; a page with no answer ends after 10 s.
- **`/tmp/hub_btstack.lock`**: a helper that needs the radio alone (the `ps5-wake` cold wake restarts the CSR8811) creates this file; while it exists the `init.sh` supervisor does not restart `hub_btstack`. Remove it when done. A lock older than 120 s is taken as stale and removed.

---

### Action Lifecycle & Debounce Rules

- **Tap (`tap`)**: Standard momentary press (e.g. WebUI click or short physical button tap). Dispatched once.
- **Press (`press`)**: Triggered when a physical remote button is first depressed.
- **Repeat / Hold (`repeat`)**: Triggered periodically while a physical remote button is held down.
- **Release (`release`)**: Triggered when the button is released.
- **Lost release**: a release can be lost. A plugin that holds a key down on its device (press without release) must release it by itself when no `repeat` or `release` came for about 600 ms, the deadline `hub_daemon` uses for held IR keys.
- **One-shot commands** (launch an app, type text): act on `tap` or `press` only, and ignore `repeat` and `release`, or a remote button runs them several times.
- **Hardware Debounce**: `hub_daemon` enforces an automatic 90 ms debounce window. Duplicate identical `tap` or `press` pulses sent within 90 ms are dropped to prevent erratic double-fires.

---

### Target Architecture & Cross-Compilation

The Harmony Hub CPU is an **Atheros AR9344 (MIPS 24Kc, big-endian)** running **Linux 2.6.31**. Its stock C library is **uClibc 0.9.30**.

#### Cross-Compiler Toolchain
The MIPS Buildroot toolchain (`mips32--uclibc--stable`, uClibc-ng 1.0.24 / GCC 5.4.0) is found by `build/compile_all.sh` in `build/toolchains/`; it is not downloaded. Get it from the Buildroot toolchain archives (bootlin `mips32--uclibc--stable`).

```bash
# Add MIPS toolchain to PATH
export PATH="$(pwd)/build/toolchains/mips32--uclibc--stable/bin:$PATH"

# Compile a dynamically linked executable against the Hub's uClibc
mips-buildroot-linux-uclibc-gcc -Wall -Os -s -o my_daemon main.c -lm
```

#### Binary Guidelines
1. **Link dynamically** against the Hub's `libc.so.0` (and `libpthread.so.0` only if needed): a static binary is much larger, and `/data` has about 5 MB for everything. The Hub's own binaries in this repository are static because they must run before and without anything else; a plugin does not.
2. **Symbols missing from the stock uClibc 0.9.30**: the toolchain's headers are uClibc-ng 1.0.24, so some calls link but are not on the Hub. `<ctype.h>` functions (`isdigit`, `tolower`, ...) import `__ctype_b_loc` / `__ctype_tolower_loc`, which the Hub lacks: the plugin starts and dies at the first call. Write these checks by hand; `build_plugins.sh` refuses a binary that imports `__ctype_`. Link statically only when a symbol you need is missing and cannot be avoided.
3. **No futex on the Hub kernel**: a thread waiting on a condition variable, a semaphore, a contended mutex or `pthread_join` spins at 100 % CPU (#211). Use one thread and a `poll()` loop (socket, timers as poll timeouts); if a second thread is unavoidable, wake it through a pipe it `poll()`s.
4. **Prefer POSIX shell scripts** using BusyBox applets (`sed`, `awk`, `wget`, `nc`) when network protocols allow (such as HTTP REST APIs). A script entrypoint must also stay in the foreground (`exec` the last command).
5. **TLS: link the Hub's BearSSL**, do not bundle one (next section).

#### Using TLS: the shared BearSSL
The Hub firmware ships BearSSL once, as `/data/harmony/lib/libbearssl.so.0` (about 165 KB, needs
only `libc.so.0`). The firmware's own TLS client (`hub_fetch`, for updates and HTTPS devices) uses
it too. A plugin links it instead of a static copy of its own:

```bash
# BearSSL sources at the pinned commit, then the firmware build makes payload/lib/libbearssl.so.0
sh build/fetch_bearssl.sh
sh build/compile_all.sh bearssl

mips-buildroot-linux-uclibc-gcc -Wall -Os -s -I build/tmp/bearssl/inc -o bin/my_daemon main.c \
    -L payload/lib -l:libbearssl.so.0
```

and lists it in the manifest: `"requires_libs": ["libbearssl.so.0"]`. The Hub starts every
plugin with `LD_LIBRARY_PATH=/data/harmony/lib`: the stock `ld-uClibc` 0.9.30 ignores a
binary's rpath (`-Wl,-rpath` does not help). A plugin binary started by hand needs the same
variable. The `android-tv` plugin linked this way is 34 KB instead of 100 KB.

- **The commit is part of the ABI.** BearSSL contexts (`br_ssl_client_context`, ...) are structs
  the caller allocates, so their size and layout must match the library. The library is built
  from the commit pinned in `build/fetch_bearssl.sh`: compile against the headers of that commit.
  A new commit gets a new name (`libbearssl.so.1`), and the old one stays until the plugins that
  need it are rebuilt.
- **Certificates.** The Hub's CA bundle is `/data/harmony/cacert.pem` (PEM; strip CR before
  feeding it to `br_pem_decoder`, a copy from a Windows checkout has CRLF line ends). A device on
  the LAN with a self-signed certificate is better served by pinning its key on first use, as
  the `android-tv` plugin does.
- The library exports only the functions in `build/bearssl_exports.txt`: the TLS 1.2 client,
  certificate and PEM decoding, hashes, HMAC, HKDF, HMAC-DRBG, RSA and EC keys and signatures,
  raw AES/GCM/ChaCha20. A plugin that needs more asks for it in that list (a new name keeps the
  SONAME; removing one needs a new SONAME).

---

### Packaging & Testing

1. Create a clean staging directory with `manifest.json` and your binaries:
   ```bash
   mkdir -p /tmp/pkg_staging/bin
   cp manifest.json /tmp/pkg_staging/
   cp my_daemon /tmp/pkg_staging/bin/ && chmod +x /tmp/pkg_staging/bin/my_daemon
   tar -czf my-plugin.tar.gz -C /tmp/pkg_staging manifest.json bin
   ```

2. Test installation locally on the Hub:
   ```bash
   # Upload and test via API (authentication must be on: pass an admin token).
   # The upload is base64 inside JSON only, at most 1 MB of archive.
   base64 my-plugin.tar.gz | tr -d '\n' > /tmp/pkg.b64
   curl -X POST http://<hub-ip>:8089/api/plugins/install \
        -H "Authorization: Bearer <admin token>" \
        -H "Content-Type: application/json" \
        -d "{\"archive_base64\":\"$(cat /tmp/pkg.b64)\",\"dev\":true}"
   ```
   Pass `"sha256": "<64 hex>"` instead of `"dev": true` to have the Hub check the archive.

3. Run the firmware's plugin tests (`tests/suite.d/27-plugins.sh`: `test_plugin_mgr` and `test_plugin_device_save.py`).

---

### Publishing & Distribution

There are two ways to publish your plugin:

#### Option A: Submit a PR to Official `harmony-ng-plugins` (Recommended)
To make your plugin visible to all HarmonyNG users out-of-the-box:
1. Fork [**https://github.com/iranl/harmony-ng-plugins**](https://github.com/iranl/harmony-ng-plugins).
2. Add your plugin folder `<plugin_id>/` at the repository root and its build to `build_plugins.sh`.
3. Open a Pull Request: CI builds it for the Hub and runs the host tests. Once merged, the release workflow publishes `<plugin_id>-v<version>` and adds the plugin, with its SHA-256, to `repository.json`. Bump `version` in `manifest.json` to release a change.

#### Checksums and URLs
Downloads use `hub_fetch`, which checks the server certificate against the Hub's CA bundle (BusyBox `wget`, which checks none, only when `hub_fetch` or the bundle is missing). Every install that is not marked `dev` needs a SHA-256: the request's `sha256`, or the `sha256` of the marketplace entry for that `download_url`. Without one the install is refused, and an archive whose SHA-256 differs is refused. A `sha256` sent with a `dev` URL install is refused (the raw files cannot be checked). `build_plugins.sh` in `harmony-ng-plugins` writes `repository.json` with the checksum of each archive it builds: upload those exact archives. A pull installs the current marketplace entry of the plugin's id (checked the same way), from the repository it was installed from (two repositories may list the same id), or a dev plugin again from its dev URL. A plugin whose repository is not known (uploaded, or installed by an older version whose archive no catalog lists now) cannot be pulled: install it again from the marketplace.

URLs (repositories, downloads) must be `http://` or `https://` with plain characters only (letters, digits, `-._~:/?#@&+,=%`). File names in a manifest's `files` list must be relative paths of letters, digits, `.`, `_`, `-` and `/`, without `..`. An archive with an absolute path, a `..` entry, or a symbolic or hard link entry is refused.

#### Option B: Host Your Own Community Repository
To distribute your plugin independently, host a `repository.json` index on GitHub or any HTTP server:

```json
{
  "repo_name": "My Community Plugins",
  "repo_version": "1.0.0",
  "plugins": [
    {
      "id": "my-plugin",
      "name": "My Custom Driver",
      "version": "1.0.0",
      "author": "Your Name",
      "description": "Integration driver for my smart device",
      "homepage": "https://github.com/username/my-plugin",
      "download_url": "https://github.com/username/my-plugin/releases/download/v1.0.0/my-plugin.tar.gz",
      "sha256": "<64 hex characters: SHA-256 of the archive at download_url>",
      "device_definition": {
        "device_type": "my_device",
        "display_name": "My Device",
        "config_fields": [
          { "key": "host", "label": "Device IP", "type": "string" }
        ],
        "default_commands": [
          { "name": "PowerToggle", "keyCode": "PLUGIN:my-plugin:PowerToggle" }
        ]
      }
    }
  ]
}
```

Users can simply paste your repository URL into the **Repositories** tab in their WebUI or Android App to install your plugin.

---

### Security

- A plugin runs as root. Installing one runs its code as root, so the install, pull and repository routes need authentication to be on and the admin role.
- **What the authentication gate does not stop**: while authentication is off, any client on the LAN can turn it on with an account of its own (`POST /api/system-action`, action `auth`), sign in and install a plugin. The gate makes that visible (the owner is locked out of the Hub afterwards), it does not prevent it. Turn authentication on yourself before the Hub is on a network you do not control. Real protection against an untrusted LAN needs signed catalogs and archives (roadmap task 1.3).
- **What the SHA-256 protects against, and what not**: the checksum catches a damaged or cut download. It does not prove the archive is the published one: `repository.json`, which carries the checksums, comes over the same channel (`http://` is accepted, and an `https://` certificate is only as good as the CAs in the bundle). Someone who can change the traffic between the Hub and the repository can replace both. Signing `repository.json` in the release workflow and verifying it on the Hub (as firmware updates are verified) is planned (roadmap task 1.3); until then, add only repositories you trust, on a network you trust.

---

## Reference Plugins

Official reference implementations are hosted in the **harmony-ng-plugins** repository:
👉 [**https://github.com/iranl/harmony-ng-plugins**](https://github.com/iranl/harmony-ng-plugins)

- **`roku-ecp`**: Wi-Fi External Control Protocol for Roku streaming sticks and Roku TVs.
- **`ps5-wake`**: DualSense/DualShock Bluetooth wake packets for PlayStation 4 and PlayStation 5 consoles.
- **`android-tv`**: Local network key and media controls for Android TV and Google TV devices.
