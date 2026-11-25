# Harmony Hub - First Time Installation & Setup Guide

This guide walks you through rooting and installing **HarmonyNG** (`hub_daemon`) on a stock Logitech Harmony Hub (Standard Model O-R0004 or Harmony Pro 2400).

---

## 1. Prerequisites & Getting the Files

### Hardware & Network Requirements
- **Harmony Hub**: Standard Hub (Model O-R0004, USB `046D:C129`) or Harmony Pro 2400 (PoE/RJ45, USB `046D:C132`).
- **Micro-USB Cable**: Data-capable cable (not charge-only).
- **Host PC**: Windows 10/11, macOS, or Linux.
- **Python 3.13 or newer**: Installed on host PC (no external libraries needed; uses standard library). Ubuntu 24.04 and Debian 12 ship an older Python: there, install 3.13 from python.org or with `uv python install 3.13`.
- **OpenSSH client**: `ssh`, `scp` and `ssh-keygen` in the PATH. The tools run these programs.
- **7-Zip** (`7z` or `7zz` in the PATH): only for the icon archive of the deploy (section 4).
- **Linux only**: the kernel binds no driver to Logitech's Harmony USB IDs (046D:C110-C14F are ignored on purpose), so there is no `/dev/hidraw*` node; the tools open the USB device node `/dev/bus/usb/<bus>/<device>` directly. No package is needed, only read and write access to that node: run the USB tools with `sudo`, or add a udev rule as `/etc/udev/rules.d/60-harmony.rules`, run `sudo udevadm control --reload` and connect the device again. This rule gives the user logged in at the desktop access:
  ```
  SUBSYSTEM=="usb", ATTR{idVendor}=="046d", ATTR{idProduct}=="c129|c12b|c132", TAG+="uaccess"
  ```
  Without a desktop login (e.g. over SSH), on Debian or Ubuntu, use this line instead and add your user to `plugdev` (`sudo usermod -aG plugdev $USER`, then log in again). Do not add `GROUP="plugdev"` where that group does not exist: newer systemd then ignores the whole line.
  ```
  SUBSYSTEM=="usb", ATTR{idVendor}=="046d", ATTR{idProduct}=="c129|c12b|c132", MODE="0660", GROUP="plugdev"
  ```
  Without access the tools say that the device is on USB but may not be opened, and name the node.
- **Local Network**: 2.4 GHz 802.11b/g/n Wi-Fi, or direct RJ45 Ethernet cable (Pro 2400).

### Choose Your Installation Method

#### Option A: Release Bundle (Recommended — No Compilation)
1. Download the latest `harmony-hub-v*.zip` (Windows) or `harmony-hub-v*.tar.gz` (Linux/macOS) from the [**Releases Page**](https://github.com/iranl/harmony-ng/releases).
2. Extract the archive into a folder on your computer.
3. Open a terminal (PowerShell or Bash) in the extracted directory. All required binaries, scripts, modules, and deployment tools are pre-packaged and ready to run.

#### Option B: From Git Repository (Developers)
```bash
git clone https://github.com/iranl/harmony-ng.git
cd harmony-ng

# 0. Bootlin MIPS toolchain (the one the release build uses), unpacked into build/toolchains/
mkdir -p build/toolchains
curl -fsSLO https://toolchains.bootlin.com/downloads/releases/toolchains/mips32/tarballs/mips32--uclibc--stable-2017.05-toolchains-1-1.tar.bz2
tar -xjf mips32--uclibc--stable-2017.05-toolchains-1-1.tar.bz2 -C build/toolchains/

# 1. Compile MIPS userspace binaries (no target = "all"; the "hub" target leaves out
#    hub_btstack, and the deploy then installs no Bluetooth daemon). Fetches BTstack with git.
./build/compile_all.sh

# 2. Compile WebUI bundle (deploy_hub.py runs "npm run build" again itself, so npm and
#    "npm install" are needed in any case)
cd android-app && npm install && npm run build && cd ..
```

> [!NOTE]
> The automatic stock backup in `tools/root_hub_usb.py` needs a MIPS BusyBox with `tar` (the stock one has none). The release bundle ships it as `bin/busybox`; a git checkout does not, so build it first with `./build/build_busybox.sh mips` (output: `payload/bin/busybox`), otherwise the root tool cannot take the backup.

---

## 2. Automated USB HID Rooting & Firmware Update

> [!IMPORTANT]
> **Up-to-Date Firmware Required**: HarmonyNG requires the latest official Logitech firmware (**v4.15.600** for standard Hub, or **v10.0.230** for Harmony Pro 2400). You do **not** need the proprietary Logitech desktop app or MyHarmony software. Our automated tooling checks, downloads, and flashes firmware directly over USB HID.

If your Hub is running stock firmware with SSH disabled, use this one-time automated USB HID rooting procedure.

### Step 2.1: Connect Hub via Micro-USB
1. Connect the Harmony Hub directly to your host PC using a standard Micro-USB data cable.
2. The Hub powers on and enumerates on USB as `046D:C129` (Standard Hub) or `046D:C132` (Harmony Pro 2400).

### Step 2.2: Run Automated USB Root Tool
Run the all-in-one root tool:

```bash
# If your Hub is already connected to your local network (Wi-Fi or Pro 2400 WiFi/Ethernet):
python tools/root_hub_usb.py --hub-ip <YOUR_HUB_IP>

# If your Hub is factory-reset or needs Wi-Fi credentials configured:
python tools/root_hub_usb.py --wifi-ssid "YourNetworkName"   # asks for the Wi-Fi password
```

The tool automatically:
1. Detects the Harmony Hub on USB HID (`046D:C129` or `046D:C132`).
2. **Checks Firmware Version**: Queries `/sys/sysinfo` over USB HID and checks against the official Logitech SUS update servers (v4.15.600 for standard Hub Skin 97, v10.0.230 for Pro 2400 Skin 115). If the Hub is not running latest firmware, it prompts:
   ```text
   [!] Hub firmware is 4.15.250, but latest official firmware is 4.15.600.
   [?] Download and install latest official firmware (4.15.600) first? (y/N):
   ```
   If confirmed, it downloads the official firmware, flashes it cleanly via USB HID, waits for the reboot, and resumes rooting automatically. *(Pass `--skip-fw-check` to bypass).*
3. Configures Wi-Fi credentials over USB HID if `--wifi-ssid` is supplied.
4. Stages the self-extracting MIPS Dropbear payload to `/sys/dropbear`.
5. Arms the persistent boot trigger `/tde/enable` -> `/etc/tdeenable`.
6. Sends reboot signal to `/sys/reboot`.
7. Hub restarts with Dropbear SSH permanently enabled on port 22 on your local network, for key logins only (the key the tool installs; password logins are refused). A Hub rooted before this change gets key-only SSH from the first boot after harmony-ng is deployed; the server that already runs keeps its options until then.
8. **Automatically backs up stock partitions**: Streams the entire `/cache` and `/data` partitions to `backups/stock_hub_backup_<timestamp>.tar` and `backups/stock_hub_backup_latest.tar`, and on the first run also `backups/golden_hub_backup.tar` (uncompressed tar; no gzip is used to avoid memory/CPU exhaustion and BusyBox compression incompatibilities on embedded MIPS).

What the root step changes on the Hub besides SSH: it empties the root password in `/etc/passwd` and turns the stock watchdog daemon off (`/etc/nowatchdog`). The backup of step 8 is a tar of the `/cache` and `/data` directories, taken after these changes, and `/etc` and `/usr`, which live on the same flash as `/data`, are not in it: it is a copy of the stock `/cache` and `/data` contents, not an image of the whole Hub.

> [!TIP]
> You can also check, download, or flash Hub firmware at any time with the standalone tool:
> ```bash
> python tools/harmony_firmware.py check
> python tools/harmony_firmware.py flash --target hub
> ```
> `flash` writes a bundle only when its intended skins list the skin the device reports over USB (a remote without a bundle of its own gets the remote bundle the tool picks for it); `--force` overrides that check.
> Other subcommands and options: `check --all-skins` (every known skin), `download` (`--target hub|remote`, `--skin <id>`, `--out-dir <folder>`, `--extract-ezhex`), and for `flash`: `--firmware <file.hfw2>`, `--dry-run` (checks the bundle, writes nothing) and `--yes` (no confirmation question). Downloads go to the `firmware/` folder next to `tools/`.

What the root tool needs, and when it stops:
- It reads the Hub's Wi-Fi state over USB. When the Hub is not connected, it asks for the SSID and the password, and stops without rooting if the Hub is not connected 40 seconds later.
- It stops without rooting when the PC cannot reach the Hub's address on port 8088 or 22, so the PC and the Hub must be on the same network.
- The stock backup of step 8 is taken only when SSH answers within 60 seconds after the reboot. If the tool prints a timeout instead, no backup was taken.

Other options of `tools/root_hub_usb.py`: `--pubkey <file.pub>` (authorize this key, an Ed25519 key only: the Hub's SSH server takes no RSA or ECDSA key; default: the newest `~/.ssh/harmony_owner_*.pub`, created as `harmony_owner_ed25519` when there is none), `--wifi-pass <password>` or the environment variable `HARMONY_WIFI_PASS` (instead of the password prompt), `--wifi-enc` (default `WPA2-PSK`; `NONE` or `OPEN` for a network without a password), `--wifi-only` (set up Wi-Fi and reboot, without SSH), `--no-reboot` and `--no-backup`.

### Step 2.3: Verify SSH Access
Test SSH connectivity over your local network:
```bash
ssh -i ~/.ssh/harmony_owner_ed25519 root@<YOUR_HUB_IP>
# Output: Linux HarmonyHub 2.6.31.14 ...
exit
```

---

## 3. Stock Backup & Recovery Safety

When rooting via `tools/root_hub_usb.py`, a backup of your Hub's `/cache` and `/data` directories is automatically saved to:
```text
backups/stock_hub_backup_<timestamp>.tar
backups/stock_hub_backup_latest.tar
```

When `backups/golden_hub_backup.tar` does not exist yet, the root tool also copies the archive to that name; later runs never replace it. Every run replaces `stock_hub_backup_latest.tar`: after a factory reset and a new rooting it holds the reset Hub, and the backup with your stock configuration is the dated file (or the golden one).

> [!NOTE]
> **No gzip compression**: Backups and restores use uncompressed tar archives (`.tar`, not `.tar.gz`). The embedded BusyBox on the Hub has limited RAM and flash bandwidth; omitting gzip avoids CPU exhaustion and out-of-memory kernel panics during tar extraction.

The backup that `tools/backup_hub.py` makes (also before every `deploy_hub.py` run) holds `/data/harmony` (devices, activities, button maps, Bluetooth keys, certificates), the stock `/data/resources` (also kept in `stock_resources_<hub>.tar`, never replaced: the copy that allows emptying them, see below), the Wi-Fi configuration, the SSH host keys and the saved stock boot script. The files are named after the Hub (its Wi-Fi MAC), so several Hubs can share one `backups/` folder: `golden_hub_backup_<hub>.tar` is the first backup of each Hub and is never replaced (a `golden_hub_backup.tar` in the same folder is kept as the golden backup, and no new one is made next to it: older versions of the tool wrote that file, and `tools/root_hub_usb.py` still writes it, so a Hub rooted from this folder gets no `golden_hub_backup_<hub>.tar`), and of the `hub_backup_<hub>_<time>.tar` files the newest five are kept, plus the newest of each of the eight weeks before them. `deploy_hub.py` makes such a backup before every deployment and stops without changing the Hub when it cannot make one (`--no-backup` skips it; a `--webui-only` run makes none). To put such an archive back on the Hub, name it: `python tools/restore_stock.py --hub-ip <YOUR_HUB_IP> --backup backups/hub_backup_<hub>_<time>.tar` (without `--backup` the tool takes the stock backup; with `--hub-ip` it uses SSH when the Hub answers there, without the USB step). The tool empties `/cache` first and these archives hold no `/cache`, so run `deploy_hub.py` again afterwards.

You can also run manual backups at any time:
```bash
# Cross-platform (Windows, Linux, macOS):
python tools/backup_hub.py <YOUR_HUB_IP>
```

Options: `--key <file>` (the SSH private key), `--user`, `--backup-dir <folder>` (instead of `backups/`) and `--dry-run` (shows what it would save). Always give the Hub's address: without one the tool tries `192.168.1.1`.

The golden backup holds no `/data/harmony`. Neither kind is a stock image: to return to stock, use the root backup above (`backups/stock_hub_backup_*.tar`) with `python tools/restore_stock.py`.

---

## 4. Deploy HarmonyNG Firmware

Run the deployment script to push `hub_daemon`, the WebUI, network manager, and kernel modules to the Hub:

```bash
# Cross-platform (Windows, Linux, macOS):
# From repo root:
python build/deploy_hub.py <YOUR_HUB_IP>

# Or from extracted release zip:
python deploy_hub.py <YOUR_HUB_IP>
```

Add `--dry-run` (or `-n`) to see what a deployment would change without changing anything: it reads the Hub's checksums, lists the files it would upload or purge and the steps it would run (backup, service stop, restart or reboot), and leaves the Hub and the local tree as they are. It does not build the web UI; from the repository it compares the files already in `payload/www`.

Other options: `--key <file>` or `-k` (the SSH private key; default: the file named by `$SSH_KEY`, else the newest `~/.ssh/harmony_owner_*` key), `--user`, `--ui legacy|new` (the web UI a git checkout builds and deploys; default `legacy`), `--webui-only` (uploads only the web UI: no backup, no service stop before the upload; the services are still restarted at the end), `--reboot` (reboots the Hub instead of restarting the services), `--no-backup`, `--stage-remote-ota` (copies a local `remote_ui.ota.gz` to `/tmp/remote_ui.ota.gz` on the Hub, in RAM, as the image for a remote update) and `--dev-btstack` (from a git checkout only: installs `build/output/hub_btstack_dev`, the Bluetooth daemon built with the BTstack info logs; with `touch /data/harmony/bt_packet_dump.conf` and a restart it also logs the packets. A release build logs packets and errors only. A crash line of the dev build resolves with `build/output/hub_btstack_dev.debug`; a later deploy without the option puts the release build back. Each GitHub release also carries `hub_btstack_dev_v<version>.tar.gz` with the dev build and both `.debug` files of that release). From a git checkout the deploy builds the web UI itself, so `npm` must be installed and `npm install` done in `android-app/` (or `frontend/` for `--ui new`).

The installer automatically:
1. Validates available flash memory before writing.
2. Installs `hub_daemon` (Hub micro-daemon) to `/data/harmony/bin/`.
3. Deploys startup scripts and USB Ethernet kernel drivers.
4. Deploys the lightweight Web UI to `/cache/www/`.
5. **Auto-extracts Remote UI Icons**: If the icon archive is not yet built (`icons.tar.gz` in the folder of the release bundle, `build/icons.tar.gz` in a git checkout), the script asks:
   ```text
   [?] icons.tar.gz not found on host.
   Download official remote firmware, extract 1,350 icons, and build icons.tar.gz now? [Y/n]
   ```
   If confirmed, it downloads the official Elite firmware (`remote_111_4.15.330.hfw2`), extracts the device & activity icons (about 1,350) from the internal SquashFS filesystem (7-Zip must be installed), packages them into that archive, and stages them to `/cache/icons.tar.gz` on the Hub. Without 7-Zip, or when you answer `n`, the deploy goes on and leaves the Hub's icons as they are.
6. Restarts the services (`init.sh restart`) and checks with `pidof`, for up to about 11 s, that a Hub daemon runs (`hub_daemon`, `hub_rf` or `hub_btstack`). With `--reboot` it reboots the Hub and checks nothing. It requests no API: open `http://<YOUR_HUB_IP>:8089` to check the Hub answers.

The installer also changes the Hub outside `/data/harmony` and removes old files, on every run:
- links every BusyBox applet into `/cache/bin` and removes the links to it that older versions made in `/usr/bin` (a login shell has `/cache/bin` on its PATH; a command sent over ssh without one has only the stock applets, so call `/cache/bin/busybox <applet>` there), writes `/etc/profile.d/00-harmony.sh` (PATH) and rewrites `/root/.profile` and `/home/root/.profile`;
- renames `/data/harmony/mqtt_config.json`, `elite_remote_map.json` and `bt_devices.json` to their current names (`mqtt.json`, `remote_slot1_map.json`, `bt-devices.json`) when no file of the new name exists, and deletes them otherwise; deletes other files of older layouts without converting them, among them `/data/harmony/devices.json`, `/data/devices.json` and `/data/activities.json`; the current configuration lives in `/data/harmony/config/`. `--dry-run` lists this step as one line.

### Stock configuration import

On the first deploy (no `/data/harmony/config/devices.json` on the Hub yet), the deploy script copies the stock Logitech files from `/data/resources` and converts them with `tools/migrate_hub_configs.py`:

| Stock | HarmonyNG |
| :--- | :--- |
| Devices and their commands (`DeviceList.json`) | `devices.json`, with short device ids (`1`, `2`, ...). IR commands get no `repeats` field: stock keeps the count of a press per device. That count, the device's `PressMinRepeats` in the Logitech account, becomes the device's `pressMinRepeats` (left out when the account has none: the Hub then uses 3, as stock does), so a press sends the same infrared frames as on the stock Hub (docs/API.md 5.1) |
| Power settings (`PowerFeature`): power-on delay, discrete on/off actions | Each device's `powerOnDelay`, `powerOnSequence` and `powerOffSequence`. Toggle-only devices keep empty sequences and use their `PowerToggle` command |
| Activities, their devices and the input each device is set to (`ActivityList.json` roles) | `activities.json`, with the input commands at the start of each activity's start sequence |
| Remote button maps (`MapList.json`) | One map per stock remote, told apart by the remote model at the start of each map's `ButtonMapIdentifier` (`16417` a touchscreen remote, `16422` a Companion, `16414` a Smart Control), since every remote of a Hub carries the same `RemoteId`. The remote with screen buttons goes to `remote_slot1_map.json`, the others to the slots after it (`remote_slot2_map.json` to `remote_slot6_map.json`): the keys of each activity, the keys of each device (device browser), each activity's screen buttons, and the activity keys of its `Root` map (a Companion's or Smart Control's) as the "no activity" entry (`-1`). A stock map with nothing left to import, empty or with every button skipped, gets the default keys, as a missing one does |

Not imported; the migration prints a warning for each:
- Long presses, double presses and touch gestures (HarmonyNG has no equivalent yet).
- Inputs that the stock config selects by stepping through a Source key (common on soundbars). Add the steps to the activity by hand.
- Commands of a second transport on the same device, e.g. the Bluetooth keyboard commands of an IR TV.
- Screen buttons beyond the config push limit of 240 per remote, which is shared out between the activities.
- Device command keys of a remote's `Root` map: the Hub uses the "no activity" entry for every key the running activity leaves unmapped, so they would be sent in every activity. Only the keys that start an activity are kept.
- The button maps of stock remotes beyond the 6 RF slots.

**Bluetooth devices** (e.g. an NVIDIA Shield) have to be paired again: HarmonyNG uses its own Bluetooth stack, so the stock pairings are not carried over. A host paired by mistake, or one you no longer use, can be removed with **Forget** in the Bluetooth host list of the web UI.

The deploy only imports when the Hub has no configuration yet. To run the import again, for example to read its warnings, on a machine with this repository:

```bash
mkdir -p stock
scp -O -r -i ~/.ssh/harmony_owner_ed25519 root@<YOUR_HUB_IP>:/data/resources stock/
python3 tools/migrate_hub_configs.py --src stock/resources --dest migrated
```

To use the result, back up `/data/harmony` on the Hub first, copy `migrated/config/*` to `/data/harmony/config/` and `migrated/*.json` to `/data/harmony/`, then reboot the Hub.

**Emptying `/data/resources`.** After the import HarmonyNG does not read the stock files. They take about 250 kB as files, less on the Hub's compressed flash (JFFS2). They can be removed from the web UI (Settings, Stock Resources), with `python tools/clean_stock_resources.py <YOUR_HUB_IP>` or with `deploy_hub.py --clean-stock-resources`. All three refuse until:
- the stock configuration was imported (a non-empty `/data/harmony/config/devices.json`); the deploy never removes the files in the run that imported them, and
- a backup saved them as they are now: `tools/backup_hub.py` (run by every deploy) keeps `backups/stock_resources_<hub>.tar` (`<hub>` is the Wi-Fi MAC; without a readable MAC nothing is saved). Each run checks the copy file by file (names, sizes, md5) against the Hub. A copy is never replaced: when the files changed since, a new `stock_resources_<hub>_<time>.tar` is saved beside it. Then the Hub is marked (`/data/harmony/stock_resources_saved`, the copy's name and a fingerprint of the file names and sizes). `clean_stock_resources.py` and the deploy need the matching copy in the `backups/` folder of the computer they run on; the web UI cannot see that folder and needs the mark to match the files on the Hub. Keep these files: they are the only copy.

The stock firmware needs the files. `tools/restore_stock.py` puts the newest `stock_resources_<hub>*.tar` back on a restore of the stock firmware (an archive without `/data/harmony`) that holds no stock files while the Hub has none, and stops before stopping any process when there is no such copy. A rolling backup (`hub_backup_<hub>_<time>.tar`) restores HarmonyNG, which does not need them: nothing is added. `--stock-resources <file>` adds a copy in any case (also when the Hub's MAC cannot be read). The backup makes no golden backup while `/data/resources` is empty. After a software reset (which removes the mark with `/data/harmony`) or `tools/root_hub_usb.py` (which writes `stock_hub_backup_*.tar` and `golden_hub_backup.tar` itself), an emptied Hub can still give archives without stock files; `restore_stock.py` then adds the copy or refuses. Before "Revert to Stock Firmware" in the web UI on an emptied Hub, keep the copy: the stock firmware needs it put back with `restore_stock.py`. To run the import above again, extract the saved copy (`tar -xf backups/stock_resources_<hub>.tar` gives `data/resources`) and pass that folder to `--src`.

The tool also takes a HarmonyNG configuration folder or a backup bundle (`--src`). The RF protocol carries 4 bytes of a device id, so a device whose id is longer gets the lowest free number as its id (a number no device has and no activity or remote map refers to, so a reference left over from a deleted device never points at it); devices whose id fits keep it, and every reference follows (activities, power sequences, the Bluetooth trigger, the remote maps and the hosts of an old `bt-devices.json`). The migration lists each renumbered id in its warnings.

### SSH host keys

The host tools (`deploy_hub.py`, `backup_hub.py`, `root_hub_usb.py`, `restore_stock.py`, `factory_reset.py`, `deploy_remote.py`, `setup_remote.py`) record a device's SSH host key the first time they connect, in `~/.ssh/harmony_ng_known_hosts` (`$HARMONY_KNOWN_HOSTS` names another file), and refuse to connect when the device later shows a different key. A device that was rooted again, factory reset or replaced has a new key: the tool then stops and names `--reset-host-key`, which forgets the recorded key of that address, so the next connection records the new one. Check that the device at that address is yours before you reset its key.

- The key is recorded under the address or name you give the tool, in lower case (`HostKeyAlias`), so `--reset-host-key` with the same address finds it. A Hub used by name and by address has one entry for each: after its key changed, reset it under each spelling you use.
- The file belongs to the account that runs the tool: Windows and WSL, or a run with `sudo` and one without, each have their own. A key forgotten in one is still recorded in the other, and the tools run from there stop and name `--reset-host-key`.
- If the file cannot be written (for example a `~/.ssh` owned by root after a run with `sudo`), the tools stop at their start, before any USB or device work, and name the file; fix its owner or set `$HARMONY_KNOWN_HOSTS`.
- `accept-new` needs OpenSSH 7.6 or newer (Windows 10 and 11 ship a newer one). An older client refuses the option, and the tools report that they cannot reach the device.
- An entry for the remote that an older `setup_remote.py` wrote to `~/.ssh/config` is not changed; only `ssh` by hand uses it.
- The Hub's SSH server loads only an Ed25519 host key. A Hub that had only the stock RSA key gets an Ed25519 one when the server is installed; `deploy_hub.py` reads it over the connection it just checked and records it next to the RSA one. A Hub updated by OTA from v2.5.3 or older gets the key without that step: a client that recorded only its RSA key then reports a changed key, and `--reset-host-key` records the new one.
- If `root_hub_usb.py` meets a changed key after rooting, the Hub is rooted and only the stock backup is missing: `python tools/backup_hub.py <YOUR_HUB_IP> --reset-host-key` records the new key.

### Root's Ed25519 key

The Hub's SSH server takes Ed25519 user keys only. `deploy_hub.py` and an OTA update install it only when root's `/home/root/.ssh/authorized_keys` on the Hub holds an Ed25519 key line the server accepts, and `deploy_hub.py` only when this PC also logs in with that key. Otherwise they keep the current server and install the rest: `deploy_hub.py` prints `[!] dropbearmulti NOT updated` and ends with exit code 1, and the update log shows `[!] dropbearmulti not updated`. A Hub rooted with an RSA key gets this until an Ed25519 key is added:

```bash
# On this PC (skip if ~/.ssh/harmony_owner_ed25519 exists)
ssh-keygen -t ed25519 -f ~/.ssh/harmony_owner_ed25519
# Append it, logging in with the key that works today (the echo ends a last line that has no newline)
ssh -i <YOUR_CURRENT_KEY> root@<YOUR_HUB_IP> 'echo >> /home/root/.ssh/authorized_keys; cat >> /home/root/.ssh/authorized_keys' < ~/.ssh/harmony_owner_ed25519.pub
# Check the new key alone, then deploy with it
# (a new connection: with ControlMaster in ~/.ssh/config it would else reuse the login above)
ssh -i ~/.ssh/harmony_owner_ed25519 -o IdentitiesOnly=yes -o PubkeyAcceptedKeyTypes=ssh-ed25519 \
    -o ControlMaster=no -o ControlPath=none root@<YOUR_HUB_IP> true
python build/deploy_hub.py <YOUR_HUB_IP> --key ~/.ssh/harmony_owner_ed25519
```

In Windows PowerShell, which has no `<`, the append is `Get-Content $env:USERPROFILE\.ssh\harmony_owner_ed25519.pub | ssh -i <YOUR_CURRENT_KEY> root@<YOUR_HUB_IP> "echo >> /home/root/.ssh/authorized_keys; cat >> /home/root/.ssh/authorized_keys"`.

The line must start with `ssh-ed25519 ` and the whole key, or with one options field the server knows (for example `no-pty`; it refuses a line with an option it does not know, such as OpenSSH's `from=`), one blank and then the key. The checks answer no for a key with blanks in front but no options, a cut key, an Ed25519 key written as the comment of an RSA key, and two keys joined on one line: the server would not let that key in either.

---

## 5. Accessing the Web Console & Android App

1. In your browser or companion app, navigate to:
   ```text
   http://<YOUR_HUB_IP>:8089
   ```
2. Use the header language selector for English, Deutsch, Français, Nederlands, Español, or Italiano.
3. Download the Android companion app (`harmony-hub-release.apk`) from the [**Releases Page**](https://github.com/iranl/harmony-ng/releases) to control devices and activities directly from your phone.
4. If authentication is on, log in once in the app. The app does not save your password: it creates a
   personal access token named "Android app (<phone model>, <date>)" for your account and keeps only
   that token. Log out in the app deletes the token on the Hub; an admin can also delete it in the
   token list (User Accounts & Access Control), where each token shows its owner. A new password
   does not log the app out: to lock out a lost phone, delete its token there. An operator account
   can hold 10 tokens; logging in again replaces the app's own token. If the app reports that
   `config/tokens.json` cannot be read, an admin moves it aside with `POST /api/auth/tokens-reset`
   (`{"confirm": true}`, see API.md 11.1); every token then has to be created again. The old file
   stays as `config/tokens.json.bad` until the next reset; delete it over SSH once looked at.

> [!IMPORTANT]
> **Authentication is off after install.** Until you turn it on, every device on your network is admin on the Hub's API and web UI (port 8089): it can change the configuration, install updates and send any command. Turn it on in the web UI: **System** page, **User Accounts & Access Control**, enter an admin user name and password, tick **Require authentication** and save. Then use a personal access token for Home Assistant and scripts (`docs/API.md`, section 11). Commands sent over MQTT always run as admin, so the MQTT broker must be protected too (`docs/API.md`, section 8.1), and the Logitech protocol port 8088 (off by default) has no authentication at all.

### Hub on no network
When the Hub cannot join its Wi-Fi any more (a new router, a changed password) and the web UI is out
of reach, connect it to a computer over USB and set the network with
`python tools/set_wifi_usb.py --ssid "YourNetworkName"`: see
[ETHERNET_AND_NETWORK_SETUP.md](ETHERNET_AND_NETWORK_SETUP.md), section 5.

---

## 6. Factory Reset & Returning to Stock

If you ever wish to revert any changes, test official firmware, or recover from an unexpected state:

### Software Factory Reset (Web UI or SSH)
The web UI's "Revert to Stock Firmware" and `python tools/factory_reset.py --target hub --hub-ip <YOUR_HUB_IP>` both run the Hub's one reset script, `factory_reset.sh`. It puts back the stock boot files (`/etc/init.d/rcS.local`, `/etc/network/interfaces`) and deletes what harmony-ng keeps on the Hub: devices, activities and button maps, web UI accounts and tokens, the MQTT settings with their password, the Bluetooth keys, the Wi-Fi configuration (back to the stock firmware's copy, so the Hub needs Wi-Fi setup again) and SSH access. Save a backup first (`python tools/backup_hub.py`) to keep any of it.

`tools/factory_reset.py` asks for the typed word `FACTORY-RESET` (`--force` skips the question) and saves a Hub backup before the reset unless `--no-backup` is given. `--target all` resets the Hub and then the remote (`--remote-ip`, default 192.168.2.1), `--key <file>` picks the SSH key. `--restore-backup [file]` resets nothing: it runs `tools/restore_stock.py` with that backup (without a file, the latest).

It removes harmony-ng's data only. The stock firmware's own data stays (`/data/resources` with the devices and activities of the Logitech era, unless it was emptied as described under the stock import, the account ids in `/data/luaworks`), and so do a few harmony-ng leftovers without secrets (`/cache/bin`, `/cache/modules`, `/etc/TZ`, `/etc/ntp.conf`). Use the hardware reset below to wipe everything, for example before the Hub changes hands.

### Hardware Factory Reset (Zero-Disassembly Method)
1. **Unplug the micro-USB cable** from the back of the Hub.
2. **Press and hold the Pair/Reset button** on the rear of the Hub.
3. While holding the button, **plug the micro-USB cable into your PC**.
4. Keep holding for **15–20 seconds** until the front LED flashes red rapidly, then release.
5. The Hub formats the writable overlay (`/data` and `/cache`) and reboots to pristine stock SquashFS firmware (~30–45s).
6. When complete, the front LED pulses red slowly.

### Restore Stock with Full Configuration
To restore all your original Wi-Fi settings, devices, and activities from your automatic backup, run `restore_stock.py` after a factory reset: the hardware reset above, or the software reset `python tools/factory_reset.py --target hub --hub-ip <YOUR_HUB_IP>` (it asks you to type `FACTORY-RESET`, `--force` skips the question; it saves a `backup_hub.py` backup first, `--no-backup` skips that; then it deletes `/data/harmony` and puts the stock boot script back). On a Hub that still runs HarmonyNG, `restore_stock.py` does not bring the stock system back by itself: it only replaces the contents of `/cache` and `/data`, so the HarmonyNG boot script (`/etc/init.d/rcS.local`) and `/data/harmony` stay.

```bash
# All-in-one restoration:
python tools/restore_stock.py

# Or specify a custom backup file:
python tools/restore_stock.py --backup backups/stock_hub_backup_latest.tar

# Or restore pure stock with SSH removed:
python tools/restore_stock.py --pure-stock
```

`restore_stock.py` looks for the Hub on USB only as `046D:C129` (standard Hub). On a **Harmony Pro 2400** (`046D:C132`) it reports the Hub as not found: with SSH still working, run it with `--skip-usb-root --hub-ip <YOUR_HUB_IP>`. After a factory reset, root the Pro 2400 again first with `python tools/root_hub_usb.py --no-backup` (without `--no-backup` that run replaces `stock_hub_backup_latest.tar` with a backup of the reset Hub), then run `restore_stock.py` with `--skip-usb-root --hub-ip <YOUR_HUB_IP>`.

This automated tool gains root over USB HID, terminates background processes, extracts your backed-up `/cache` and `/data` partitions (plain uncompressed tar stream, no gzip on Hub), flushes SPI flash, and cleanly reboots back into your configured stock Logitech system.

What to expect when you run it:
- It asks you to type `RESTORE` before it changes anything (`--force` skips the question).
- Without `--backup` it takes `backups/stock_hub_backup_latest.tar`, else the newest `stock_hub_backup_*` file.
- After a factory reset the Hub has no BusyBox with `tar`, so the tool copies one from the PC: `bin/busybox` of the release bundle. In a git checkout build it first with `./build/build_busybox.sh mips`; without it the restore stops.
- The PC must reach the Hub's Wi-Fi address (port 8088 or 22), so both must be on the same network. `--wifi-ssid`, `--wifi-pass` and `--wifi-enc` work as in the root tool.
- When SSH on the Hub already works, `--skip-usb-root --hub-ip <YOUR_HUB_IP>` leaves out the USB step.

`--pure-stock` removes SSH access again (the Dropbear files and the boot trigger `/etc/tdeenable`) and the watchdog switch `/etc/nowatchdog`. It does not set the root password the root step emptied, so the Hub is close to, not identical with, a factory one.

---