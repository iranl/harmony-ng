# HarmonyNG Official Community Plugins

Official plugins repository for [**HarmonyNG**](https://github.com/iranl/harmony-ng).

This repository contains official reference plugins, packaging tools, and the master `repository.json` catalog served to the Harmony Hub Marketplace.

---

## Included Reference Plugins

| Plugin | ID | Description | Device Type |
| :--- | :--- | :--- | :--- |
| **Roku ECP Controller** | `roku-ecp` | Control Roku streaming devices and Roku TVs over local Wi-Fi via External Control Protocol (ECP). | `roku` |
| **PlayStation 5 BT Wake** | `ps5-wake` | Wake PlayStation 4 / PlayStation 5 consoles over Bluetooth using DualSense / DualShock emulation. | `ps5` |
| **Android TV Remote** | `android-tv` | Send directional, media, and navigation commands to Android TV and Google TV devices over local network. | `androidtv` |

---

## Repository Structure

```text
.
├── repository.json          # Master catalog consumed by Harmony Hub Marketplace
├── plugins/
│   ├── roku-ecp/            # Roku Wi-Fi ECP plugin source & manifest
│   ├── ps5-wake/            # PS5 Bluetooth Wake plugin source & manifest
│   └── android-tv/          # Android TV network controller source & manifest
└── scripts/
    └── package_all.py       # Builds and packages .tar.gz releases
```

---

## How to Create a Plugin

See the full [**HarmonyNG Plugin Architecture & Developer Guide**](https://github.com/iranl/harmony-ng/blob/main/docs/PLUGINS.md).

### 1. Requirements
- Atheros AR9344 MIPS 24Kc (big-endian) compatibility.
- Light footprint (< 500 KB unpacked).
- Zero flash wear (sockets and PIDs must use `/tmp/plugins/`).
- Processes must handle `run.sh start` and `run.sh stop`.
- Listen on UNIX domain datagram socket: `/tmp/plugins/<plugin_id>.sock`.
