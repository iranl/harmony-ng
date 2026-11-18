# HarmonyNG roadmap

Version 0.2 (2026-11-18). Status: proposal for review. It collects the items of issue #17, the
work in progress on branches and the open design questions into phases with tasks, priorities
and a status for each task.

The order of the phases is a proposal, not a promise. The maintainer decides what goes into
which release.

## How to read it

- **Phase:** a group of tasks with one goal and an exit test. Phases overlap in time; the number
  is the order in which they unblock each other (see "Dependencies").
- **Task id:** `<phase>.<n>`, stable. The matching item on the project board carries it in its
  title (see "Tracking").
- **Priority:** P0 = needed before the next stable release; P1 = planned, next in line; P2 =
  wanted, not scheduled.
- **Size:** S = a day or less, M = a few days, L = a week or more, for one person who knows the
  code.
- **Status:** open, in progress (with the branch or PR), done (with the PR or commit).

## Where the project stands

Released: v2.4.0, v2.4.1 and v2.4.2 (2026-11-13 and 14). They came after a review of the whole
codebase; six security advisories were fixed and published with them.

In `main` since v2.4.2 (about 50 pull requests), among others:

- remote keys reach the device without the added delays (#207), a held infrared key repeats
  until its release (#208), lost key frames are counted (#209), a key pressed with the screen
  off is sent before the screen lights up (#210);
- no futex-based waits on the Hub, whose kernel has none (#211); config writes and update
  installs that survive a power cut (#191, #197, #198); request body limits (#202);
- documentation and OpenAPI checked against the code (#229); firmware tests in four parallel CI
  groups (#231);
- the recovery access point removed (#234) and replaced by the stock way: the Wi-Fi set over USB
  from a computer (#236);
- every web interface text behind translation keys, six languages (#235);
- one copy of the SSH server on the Hub instead of two (#239).

In progress on branches: plugin system and marketplace (`feat/plugin-system`), emptying the stock
resources to reclaim flash (`feat/empty-stock-resources`), Elite timing and screenshots
(`feat/elite-improvements`), BLE broadcast commands (#43).

## Phases at a glance

| Phase | Goal | Priority | Needs |
|---|---|---|---|
| 0. Foundation | A release people can trust: reviewed, documented, tested | P0 | none |
| 1. Plugins and network devices | Devices beyond IR and Bluetooth, added without touching the core | P0 | none |
| 2. Keys, holds and touch | Every key behaves the same from every source; the touch remotes get long press, gestures, number pad and device screens | P1 | none |
| 3. Activity engine | Activities start and stop as a state machine that knows what happened | P1 | 1 (reply channel), 2 (command classes) |
| 4. Home Assistant, both ways | State from Home Assistant drives the engine; the Hub and remotes control Home Assistant devices | P1 | 3 |
| 5. One app | One designed app for web, Android and desktop | P1 | none to start; 3 for state and repair screens |
| 6. Setup and operations | Easier first install, safer updates, more code sources | P2 | none |

## Phase 0. Foundation

Goal: the next stable release is reviewed, documented and tested. Exit test: every P0 task below
is done and a release candidate ran for a week on at least two setups without a regression.

| Id | Task | Pri | Size | Status |
|---|---|---|---|---|
| 0.1 | Whole-codebase review (safety of Hub and remote, deploy tools, HTTP and auth, updates, concurrency, RF, Bluetooth, orchestrator, UIs, tools) | P0 | L | done (six advisories, v2.4.1, v2.4.2) |
| 0.2 | Fix the review findings: every critical and high one, the medium and low ones in batches, the design questions decided | P0 | L | done (#111-#219; decisions in #220, carried out in #222-#228, #233-#235) |
| 0.3 | Documentation pass: every document checked against the code | P0 | M | done (#229); about 25 small points left, most on lines later PRs changed |
| 0.4 | OpenAPI pass: every route and event present and correct; the file is parsed and compared with the routes in CI | P0 | M | done (#229) |
| 0.5 | Test coverage map: list features without a test, add tests for the P0 ones | P0 | M | in progress (tests added with every fix; no map yet) |
| 0.6 | Build with current compilers | P0 | S | done (#111) |
| 0.7 | Sanitizer and static-analysis jobs in CI (address and undefined-behaviour sanitizers on the host tests, one analyser) | P1 | M | open |
| 0.8 | Hardware test checklist per release (Hub, Elite or Ultimate, a screenless remote, one IR and one Bluetooth device) with a results template | P0 | S | open |
| 0.9 | Elite screen turns off and on again at the first display timeout, off for good only after two timeouts (seen on 2.2.1) | P0 | S | open (#17); to be checked again on current `main` |
| 0.10 | README and remote documentation screenshots from the current Elite interface | P1 | S | in progress (`feat/elite-improvements`) |
| 0.11 | Remote power soak: a week of normal use with the flight recorder on, results in `REMOTE_BATTERY_OPTIMIZATION.md` | P1 | M | open |
| 0.12 | Release notes and upgrade notes written per release from the merged PRs | P1 | S | open |
| 0.13 | CI split into firmware tests (four parallel groups) and host tool tests, one Python version | P1 | S | done (#230, #231) |

## Phase 1. Plugins and network devices

Goal: a device that is controlled over the network (or any other way) is a plugin, installed from
the app, usable in activities and on any key. Exit test: an Android TV and a Roku are added from
the marketplace and run in an activity with power on, input and navigation.

| Id | Task | Pri | Size | Status |
|---|---|---|---|---|
| 1.1 | Plugin manager, marketplace, plugin devices in the device editor, documentation | P0 | L | in progress (`feat/plugin-system`) |
| 1.2 | Reference plugins: Roku (ECP), PlayStation wake, Android TV | P0 | M | in progress (`iranl/harmony-ng-plugins`) |
| 1.3 | Plugin packages verified before install (checksum from the repository index; signature for the official repository) and installed with a flash-space check | P0 | M | open |
| 1.4 | Optional reply datagram: a plugin can confirm or refuse a command (request id, ok, reason) | P1 | S | open |
| 1.5 | Optional state datagram: a plugin reports power, input and availability of its device | P1 | S | open |
| 1.6 | Android TV plugin on the Android TV Remote protocol: pairing code, discrete power, every key, app launch, power state | P1 | L | open |
| 1.7 | Wake-on-LAN as a command any plugin device can use for power on | P1 | S | open |
| 1.8 | Plugin for devices with a simple HTTP or TCP API described in the manifest (no code needed) | P2 | M | open |
| 1.9 | BLE broadcast commands | P1 | M | in progress (#43) |
| 1.10 | Plugin author kit: template, cross-build recipe, test harness that plays the Hub side | P2 | M | open |

## Phase 2. Keys, holds and touch

Goal: press, hold and release mean the same thing from every input (RF remote, Bluetooth remote,
web, Android, API, MQTT) to every output (IR, Bluetooth, plugin, MQTT), and the touch remotes
offer what the stock firmware offered. Exit test: on an Elite, a key with a short and a long
action, a swipe, the number pad and a device screen all work and are editable in the app; a held
toggle fires once from every input.

| Id | Task | Pri | Size | Status |
|---|---|---|---|---|
| 2.1 | Input model document: the life of a key (press, hold, release, tap), timings, what each output does with each, per input source | P1 | S | open |
| 2.2 | One implementation of that model in the Hub, used by every input path; tests per input and output pair | P1 | L | in progress (key path #207, held IR keys #208, key loss counter #209, wake key #210) |
| 2.3 | Command classes: which commands repeat while held, which fire once; per-command override in the device editor | P1 | M | open |
| 2.4 | Multi-command and repeated requests over MQTT and WebSocket | P1 | S | done (#110) |
| 2.5 | Long press: a second action per hard key and per on-screen button (map format, RF key handling, remote interface, editors) | P1 | L | open |
| 2.6 | Gestures on the touch remotes: swipe up, down, left, right and tap on a gesture screen, each mapped like a key | P1 | L | open |
| 2.7 | Number pad screen on the touch remotes, per activity | P1 | M | open |
| 2.8 | Device screens on the touch remotes: per-device page with chosen on-screen buttons and the hard keys mapped to that device, built automatically and editable | P1 | M | open |
| 2.9 | Stock migration keeps long-press, gesture and number-pad entries of the stock maps | P1 | M | open |
| 2.10 | Double press as a third action per key | P2 | M | open |
| 2.11 | Favourite channels on the touch remotes | P2 | M | open |
| 2.12 | Screenless remotes (Companion, Smart Control): long press on the activity keys, same editor as the touch remotes | P2 | M | open |
| 2.13 | Acknowledged key delivery over RF, if the loss counter of #209 shows losses in real use | P2 | M | open |

## Phase 3. Activity engine

Goal: an activity start or stop is a state machine: one step at a time, each step sent, confirmed
or verified, a failure handled instead of ignored. Today the sequences rely on fixed delays (for
example the wait after a TV powers on before it takes an input command). Exit test: with a TV
that has a state source, an activity started twice in a row sends the power command once; a
device switched off by hand is switched on again; a failed step is shown with a repair.

| Id | Task | Pri | Size | Status |
|---|---|---|---|---|
| 3.1 | Agree the design: step contract, device state model, configuration fields, capability flag (a design proposal can be submitted as a separate PR) | P1 | S | open |
| 3.2 | The machine in the orchestrator, one step at a time, old configurations unchanged | P1 | L | open |
| 3.3 | Bluetooth acknowledgements: request ids, replies and pushed link events from `hub_btstack` | P1 | M | open |
| 3.4 | Device state module with the sources Bluetooth link and network probe | P1 | M | open |
| 3.5 | Reported state: one report format for outside reporters, with its rules (never invented, stale after silence, RAM only, rate limited) | P1 | M | open |
| 3.6 | Verified steps and skipping: wait for a state instead of a delay; leave out a power or input command that is already satisfied | P1 | M | open |
| 3.7 | `device_command` and `device_state` events on WebSocket and MQTT | P1 | S | open |
| 3.8 | Repair: list of single fixes for the running activity with state hints, "resend what is needed" | P1 | M | open |
| 3.9 | Step state on the remote's start screen and repair list on its Help screen | P2 | M | open |
| 3.10 | Editors: completion rule and failure rule per step, state source per device, preview with criteria | P1 | M | open |
| 3.11 | Migration helper: propose verified waits where a device has a state source | P2 | S | open |
| 3.12 | Co-simulation devices that acknowledge and expose state, so the machine is tested end to end | P1 | M | open |

## Phase 4. Home Assistant, both ways

Goal: Home Assistant is a state source for the engine and a target for the Hub and the remotes.
Exit test: a light and a media player from Home Assistant are on an Elite key and in an activity;
the state of an infrared TV known to Home Assistant drives the activity's power step.

Today the Hub is controlled from Home Assistant three ways (`HOME_ASSISTANT.md`): the
`ha-harmony-ng` custom integration over the API and WebSocket, MQTT with discovery, and the
Logitech-compatible mode for the built-in Harmony integration. Control in the other direction is
open.

| Id | Task | Pri | Size | Status |
|---|---|---|---|---|
| 4.1 | The integration reports chosen entities as device state (needs 3.5) | P1 | M | open |
| 4.2 | The integration exposes `device_command` as events, so any key or step can start an automation (needs 3.7) | P1 | S | open |
| 4.3 | Design: Home Assistant devices as Hub devices. Commands travel as events on the connection the integration already holds, with a reply; no token on the Hub | P1 | S | open (#17) |
| 4.4 | Hub device type for Home Assistant devices: commands, power and inputs supplied by the integration, command sets that only grow | P1 | L | open |
| 4.5 | Integration side: pick devices or single entities, command profiles per kind of entity (lights, switches, covers, fans, scenes, scripts, media players), state back | P1 | L | open |
| 4.6 | Home control keys of the Elite (the four keys and the rocker) mapped to Home Assistant devices, with their state on the remote's screen | P1 | M | open (#17) |
| 4.7 | Hub devices mirrored into Home Assistant per device (power switch, input select, command buttons), opt-in | P2 | M | open |
| 4.8 | Integration follows the Hub without reloads: new and removed remotes, hosts and devices | P1 | M | open (in the integration's repository) |
| 4.9 | MQTT command roles (which account may send which command) and MQTT over TLS | P2 | M | open (deferred in #220) |

## Phase 5. One app

Goal: one designed app for the browser, Android and desktop, replacing the two web interfaces.
Exit test: every function of the legacy interface is reachable in the new app within three taps
from one of four tabs, in six languages, and the legacy interface is removed.

| Id | Task | Pri | Size | Status |
|---|---|---|---|---|
| 5.1 | Decide the base: continue the new interface (`frontend/`) or start a new unified one, and agree a design document | P1 | S | open (#17) |
| 5.2 | Tokens, type, shapes and base components; icons instead of emoji | P1 | M | open |
| 5.3 | Navigation: four tabs, routes for every screen, sub-page top bar; today's sections moved without changing what they do | P1 | M | open |
| 5.4 | Remote tab: activity picker, running remote, more keys, keyboard, switch activity | P1 | L | open |
| 5.5 | Activities tab and the activity editor; buttons editor (on-screen, hard keys, gestures) | P1 | L | open |
| 5.6 | Devices tab: list with badges, device page, add device (infrared, Bluetooth, plugin), settings, tools | P1 | L | open |
| 5.7 | Settings pages in the list and save-bar pattern, Remotes first | P1 | L | open |
| 5.8 | State strip and repair sheet (needs 3.7, 3.8) | P1 | M | open |
| 5.9 | Light theme, accessibility pass, language pass | P1 | M | open |
| 5.10 | Legacy interface retired | P1 | S | open |
| 5.11 | Android: Hub discovery and connect screen, haptics, volume keys | P1 | M | open |
| 5.12 | Android extras: Quick Settings tile, home-screen widget, notification and lock-screen remote, deep links | P2 | L | open |
| 5.13 | Desktop app (macOS first, then Windows and Linux): the same app in a window, discovery, tray menu, media keys | P2 | L | open |
| 5.14 | Size budget measured at each release; nothing loaded from the internet to draw a screen | P1 | S | open |
| 5.15 | Every interface text behind translation keys, six languages (machine translated, human review later) | P1 | M | done (#235); human review open |

## Phase 6. Setup and operations

Goal: a first install that a non-developer finishes, updates that cannot leave a device half
written, more ways to get device codes. Exit test: a new user goes from a stock Hub to a working
activity with the documented steps only, and an interrupted update leaves the Hub running the old
version.

| Id | Task | Pri | Size | Status |
|---|---|---|---|---|
| 6.1 | Deploy and update with a preflight (space, versions, reachability), staged files, verified switch-over and automatic roll back, on Hub and remote | P1 | L | in progress (signed updates, verify then install #197, atomic remote install #198, deploy dry run; automatic roll back open) |
| 6.2 | One guided installer for the first install (root, back up, install, migrate stock configuration) with a dry run | P1 | L | open |
| 6.3 | Setting up a factory-fresh Hub without a Logitech account: research, then document or automate | P2 | M | open |
| 6.4 | Infrared encoder coverage: list of protocols seen in the stock database against the encoder, add the missing common ones | P1 | M | in progress (Samsung, Toshiba, Saitek encoders added; no coverage list yet) |
| 6.5 | Code import from more formats (Pronto, Flipper, LIRC, pasted lists) with a preview that explains what was skipped | P1 | M | open |
| 6.6 | Backup and restore: one bundle, checked before restore, secrets left out unless asked | P1 | M | in progress (`backup_hub.py`; import checked before it is applied #195) |
| 6.7 | Hub tests on the real MIPS binaries under emulation in CI | P2 | L | open |
| 6.8 | Reproducible builds with pinned toolchains and published checksums | P2 | M | open |
| 6.9 | Return to stock: one documented, tested command for Hub and remote | P2 | M | in progress (`restore_stock.py`, `factory_reset.py`; no end-to-end test) |
| 6.10 | Container image that installs and updates a Hub for users without a development machine | P2 | M | open |
| 6.11 | Recovery when the Hub is on no network: Wi-Fi set over USB from a computer, as the stock firmware did | P1 | M | done (#234, #236); emergency boot path and an Ethernet adapter on the port not yet tested on hardware |
| 6.12 | Free flash on the Hub: one copy of the SSH server, stock resources that can be emptied | P1 | S | in progress (#239 done; `feat/empty-stock-resources`) |
| 6.13 | RF protocol version exchanged between Hub and remote, so mixed versions are detected | P2 | M | open (deferred in #220) |

## Items of issue #17

The TODO list of #17 as it stood on 2026-11-18, with its status and where each item lives in this
roadmap.

| #17 item | Status | Task |
|---|---|---|
| Hygiene: documentation fully up to date and reviewed | done | 0.3 |
| Hygiene: OpenAPI up to date and reviewed | done | 0.4 |
| Hygiene: tests complete for all features and parts | done in #17; a coverage map is still open | 0.5 |
| Hygiene: code review | done | 0.1, 0.2 |
| Feature: plugin system | in progress (`feat/plugin-system`) | 1.1, 1.2 |
| Feature: bi-directional communication between Elite/Hub and MQTT (smart home control on the Elite and the Hub) | open | 4.3, 4.6 |
| Feature: better press, release and hold for every input (Bluetooth remote, RF remote, web, Android) and output (IR, Bluetooth, MQTT) | in progress (#110, #207-#210) | 2.1, 2.2, 2.3 |
| Feature: the new frontend reaches parity with the legacy interface, or a new unified UI | open | 5.1 |
| Feature: reduce flash wear and SPI contention (Hub and Linux RF remotes) | done (#106) | |
| Feature: full Elite physical button navigation | done (506794a) | |
| Feature: no Elite update image on the Hub's flash; downloaded to `/tmp` only for an update | done (#108) | |
| Bug: Elite screen turns off and on again at the first display timeout, off only after two (2.2.1) | open | 0.9 |
| Docs: README screenshots of the new Elite interface after #36 | in progress (`feat/elite-improvements`) | 0.10 |

## Dependencies

```
0 Foundation ──────────────────────────────────────────────> every release
1 Plugins ──(1.4, 1.5 reply and state)──┐
2 Keys ─────(2.3 command classes)───────┼──> 3 Engine ──(3.5, 3.7)──> 4 Home Assistant
                                        │         └─────(3.7, 3.8)──> 5.8 State and repair screens
5 One app (5.1 to 5.7) can start now ───┘
6 Setup and operations: independent
```

A possible cut into releases, for discussion: phase 0 and tasks 1.1 to 1.3 first; phase 2 next;
phase 3 together with the first tasks of phase 4, because they change the configuration format
once; phase 5 delivered screen by screen along the way.

## Not planned

- Running the stock Logitech engine next to HarmonyNG, or patching stock binaries.
- Cloud services, accounts or telemetry of any kind.
- Absolute values on keys (a volume level, a brightness percentage): remote keys are steps.
- Support for Hub firmware other than the versions the install guide names, until someone can
  test them.

## Tracking

The roadmap is the plan; the project board ("Harmony-NG", owned by the maintainer and linked to
this repository) holds the state.

- **Board items:** every open task is a draft item titled `[<id>] <task>` (for example
  `[2.5] Long press on hard keys and on-screen buttons`), with the fields Status (Backlog, Ready,
  In progress, In review, Done), Phase, Priority, Size and Area. Tasks already done are not added.
- **Issues only when needed:** a draft item becomes an issue when someone starts the work or it
  needs a written decision. Discussion otherwise happens on Discord. Pull requests reference the
  issue (`Refs #<issue>`).
- **Issue #17** is replaced by this document and the board; its items are mapped above.
- **This document** changes by pull request when a task is added, dropped or moved between
  phases. The live status is on the board; the Status column here is refreshed at releases.

## Changes to this roadmap

| Version | Date | Change |
|---|---|---|
| 0.1 | 2026-11-13 | First proposal (not published) |
| 0.2 | 2026-11-18 | Status of every task after v2.4.2 and the work merged since; items of #17 with their status; tasks 0.13, 2.13, 4.9, 5.15, 6.11, 6.12, 6.13 added; tracking on the project board with draft items instead of one issue per task |
