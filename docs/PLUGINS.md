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
   - [Action Lifecycle & Debounce Rules](#action-lifecycle--debounce-rules)
   - [Target Architecture & Cross-Compilation](#target-architecture--cross-compilation)
   - [Packaging & Testing](#packaging--testing)
   - [Publishing to a Repository](#publishing-to-a-repository)
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
- All plugin runtime artifacts (UNIX sockets, PID files, logs) live exclusively in **RAM tmpfs** under `/tmp/plugins/`.

### 3. Non-Blocking, Isolated IPC
- Commands are dispatched from `hub_daemon` to plugins via **UNIX domain datagram sockets** (`AF_UNIX`, `SOCK_DGRAM`) at `/tmp/plugins/<plugin_id>.sock`.
- Sockets use a non-blocking timeout (200 ms). If a plugin is unresponsive or stopped, the core daemon continues immediately without locking the activity orchestrator or remote sync.

### 4. Process Supervisor & Crash Recovery
- `hub_daemon` checks enabled plugins every 10 seconds via a background watchdog tick.
- If an enabled plugin crashes, it is automatically restarted.
- When plugins are stopped or restarted, `hub_daemon` follows strict **Rule 1 termination discipline**: graceful `SIGTERM` first, sleep 1 second, followed by unconditional `SIGKILL` to ensure kernel locks and file descriptors are freed cleanly.

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
  - **Uninstall**: Stops the daemon and cleans up all associated files.
- **Marketplace**: Browse community plugins from configured repositories.
  - Click **Install** for 1-click installation.
  - If a newer version is released, an **Update** button appears automatically.
  - **Dual-Path Resilience**: The installer attempts direct download on the Hub. If the Hub cannot reach GitHub (e.g. air-gapped network or SSL certificate mismatch), the WebUI automatically downloads the archive via the browser and uploads it to the Hub as a base64 payload.
- **Upload Archive**: Click **Upload Archive (.tar.gz)** in the top bar to install a custom plugin `.tar.gz` package directly.

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
5. The device can now be assigned to any Activity, physical button, or Home Assistant automation.

---

## Developer Guide

### Plugin Package Structure
A plugin distribution is a compressed archive (`.tar.gz` or `.tgz`) containing:

```
my-plugin/
├── manifest.json          # Plugin metadata, device definitions, configuration schema
├── run.sh                 # Required lifecycle script (start, stop)
└── bin/                   # Optional compiled binaries, scripts, or assets
    └── my_daemon
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
  "entrypoint": "run.sh",
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
| `id` | `string` | Unique identifier (alphanumeric, hyphens, underscores; 1–63 chars). Must match directory name. |
| `name` | `string` | Human-readable plugin title. |
| `version` | `string` | Semantic version string (e.g. `1.0.0`). |
| `author` | `string` | Author or organization name. |
| `description` | `string` | Short summary of plugin capabilities. |
| `homepage` | `string` | URL to source repository or documentation. |
| `entrypoint` | `string` | Script or binary to execute (defaults to `run.sh`). |
| `device_definition` | `object` | *(Optional)* Schema for devices created using this plugin. |

#### Config Field Types

| Type | Description | Rendered Input |
| :--- | :--- | :--- |
| `"string"` | Text value (e.g. IP address, host, MAC) | `<input type="text">` |
| `"number"` | Numeric integer value (e.g. port, timeout) | `<input type="number">` |
| `"password"` | Sensitive string (e.g. auth token, PIN) | `<input type="password">` |
| `"boolean"` | True / false toggle | `<input type="checkbox">` |

---

### Process Lifecycle & Supervisor

Plugins must provide an executable `run.sh` script handling `start` and `stop` actions:

```bash
#!/bin/sh
# Example run.sh lifecycle script

PID_FILE="/tmp/plugins/${HARMONY_PLUGIN_ID}.pid"
SOCK_FILE="${HARMONY_SOCKET:-/tmp/plugins/${HARMONY_PLUGIN_ID}.sock}"

case "$1" in
  start)
    # Ensure previous instance is stopped
    [ -f "$PID_FILE" ] && kill -9 "$(cat "$PID_FILE")" 2>/dev/null
    rm -f "$SOCK_FILE" "$PID_FILE"

    # Start daemon in background
    ./bin/my_daemon "$SOCK_FILE" > "/tmp/plugins/${HARMONY_PLUGIN_ID}.log" 2>&1 &
    echo $! > "$PID_FILE"
    ;;
  stop)
    if [ -f "$PID_FILE" ]; then
      kill -15 "$(cat "$PID_FILE")" 2>/dev/null
      sleep 1
      kill -9 "$(cat "$PID_FILE")" 2>/dev/null
      rm -f "$PID_FILE" "$SOCK_FILE"
    fi
    ;;
  status)
    if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
      echo "Running (PID $(cat "$PID_FILE"))"
      exit 0
    fi
    echo "Stopped"
    exit 1
    ;;
  *)
    echo "Usage: $0 {start|stop|status}"
    exit 1
    ;;
esac
```

#### Environment Variables Passed to Plugins

- `HARMONY_PLUGIN_ID`: The unique plugin ID (e.g. `roku-ecp`).
- `HARMONY_SOCKET`: The absolute path for the UNIX domain datagram socket (`/tmp/plugins/<id>.sock`).
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
  "command": "Home",
  "action": "tap",
  "value": 0,
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
| `command` | `string` | Command name to execute (e.g. `Home`, `PowerOff`, `VolumeUp`). |
| `action` | `string` | Action lifecycle verb: `"tap"`, `"press"`, `"repeat"`, or `"release"`. |
| `value` | `number` | Optional numeric argument (e.g. slider position 0–100). |
| `config` | `object` | Configuration dictionary defined by the user in Device Studio. |

#### KeyCode Overrides
For custom keybindings, commands can specify a direct keycode override in the format:
```
PLUGIN:<plugin_id>:<subcommand>
```
When present, `hub_daemon` routes the command directly to `<plugin_id>` and sets the `command` field in the JSON payload to `<subcommand>`.

---

### Action Lifecycle & Debounce Rules

- **Tap (`tap`)**: Standard momentary press (e.g. WebUI click or short physical button tap). Dispatched once.
- **Press (`press`)**: Triggered when a physical remote button is first depressed.
- **Repeat / Hold (`repeat`)**: Triggered periodically while a physical remote button is held down.
- **Release (`release`)**: Triggered when the button is released.
- **Hardware Debounce**: `hub_daemon` enforces an automatic 90 ms debounce window. Duplicate identical `tap` or `press` pulses sent within 90 ms are dropped to prevent erratic double-fires.

---

### Target Architecture & Cross-Compilation

The Harmony Hub CPU is an **Atheros AR9344 (MIPS 24Kc, big-endian)** running Linux 3.3.8 with **uClibc 0.9.30.1**.

#### Cross-Compiler Toolchain
Use the buildroot toolchain provided in this repository under `build/toolchains/mips32--uclibc--stable`:

```bash
export PATH="$(pwd)/build/toolchains/mips32--uclibc--stable/bin:$PATH"
mips-buildroot-linux-uclibc-gcc -Wall -Os -s -o my_daemon main.c -lm
```

#### Binary Guidelines
1. **Never use static linking with heavy runtimes**: Flash space on `/data` is limited.
2. **Dynamic linking with uClibc**: Link against standard system libraries (`libc.so.0`, `libpthread.so.0`).
3. **Prefer POSIX shell scripts** using BusyBox applets (`sed`, `awk`, `wget`, `curl`, `nc`) when network protocols allow (such as HTTP REST APIs).

---

### Packaging & Testing

1. Create a clean staging directory with `manifest.json`, `run.sh`, and your binaries:
   ```bash
   mkdir /tmp/pkg_staging
   cp manifest.json run.sh /tmp/pkg_staging/
   chmod +x /tmp/pkg_staging/run.sh
   tar -czf my-plugin.tar.gz -C /tmp/pkg_staging manifest.json run.sh
   ```

2. Test installation locally on the Hub:
   ```bash
   # Upload and test via API
   base64 my-plugin.tar.gz | tr -d '\n' > /tmp/pkg.b64
   curl -X POST http://<hub-ip>:8089/api/plugins/install \
        -H "Content-Type: application/json" \
        -d "{\"archive_base64\":\"$(cat /tmp/pkg.b64)\"}"
   ```

3. Run the automated test suite:
   ```bash
   python tests/test_plugin_manifest.py
   ```

---

### Publishing to a Repository

To distribute your plugin via the Marketplace, host a `repository.json` index on GitHub or any HTTP server:

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

Users can add your repository URL in the **Repositories** tab to see your plugin in their Marketplace catalog.

---

## Reference Plugins

Official reference implementations are hosted in the **harmony-ng-plugins** repository:
👉 [**https://github.com/iranl/harmony-ng-plugins**](https://github.com/iranl/harmony-ng-plugins)

- **`roku-ecp`**: Wi-Fi External Control Protocol for Roku streaming sticks and Roku TVs.
- **`ps5-wake`**: DualSense/DualShock Bluetooth wake packets for PlayStation 4 and PlayStation 5 consoles.
- **`android-tv`**: Local network key and media controls for Android TV and Google TV devices.
