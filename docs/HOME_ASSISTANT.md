# Home Assistant and harmony-ng

harmony-ng talks to Home Assistant in four ways. They can run at the same time on one Hub; pick
the one that fits how you run Home Assistant. This page says what each layer is, what it gives you,
what it costs, and how to switch it on. The wire details are in [API.md](API.md).

| Layer | Needs in Home Assistant | Needs on the Hub | Discovery | Gives |
| --- | --- | --- | --- | --- |
| 1. harmony-ng integration, API transport | the `ha-harmony-ng` custom integration (HACS) | nothing (a token if authentication is on) | mDNS (`_harmony-ng._tcp`) | the full entity set, pushed over the WebSocket |
| 2. harmony-ng integration, MQTT transport | the same integration plus the MQTT integration and a broker | MQTT settings | mDNS, broker credentials in the flow | the same entity set, through the broker |
| 3. MQTT discovery | the MQTT integration and a broker, no custom code | MQTT settings with "Create Home Assistant entities" on | the broker (discovery topics) | an activity select, sensors and buttons |
| 4. Logitech protocol | the core `harmony` integration, no custom code | "Logitech protocol compatibility" on | SSDP | the entities of a stock Hub: remote and activity select |

## 1. The harmony-ng integration over the API

**What it is.** A native Home Assistant integration,
[alexdelprete/ha-harmony-ng](https://github.com/alexdelprete/ha-harmony-ng), that connects to the
Hub's REST API and WebSocket on port 8089. The Hub pushes every change (activity state and
progress, Bluetooth hosts, remote status, configuration, every key pressed on a remote); Home
Assistant never polls. Commands are acknowledged.

**You get.** The Hub as a `remote` entity (activities, `send_command` with press, hold and
release), an activity select, sensors, Bluetooth host binary sensors, buttons, an event entity per
remote key, infrared emitters on Home Assistant's infrared platform (so any core IR integration can
use the Hub as its blaster), a firmware update entity, and one child device with its own remote for
every configured IR or Bluetooth device. Services for raw IR, Bluetooth keys, the activity Help flow
and cancel. Entities are keyed on the Hub's factory unit id, so an IP change never breaks them.

**Pros.** The complete feature set and the lowest latency; no broker; nothing to configure on the
Hub; one integration for several Hubs.

**Cons.** A custom integration (installed through HACS); needs Home Assistant 2026.11 or newer;
Home Assistant must reach port 8089 of the Hub directly.

**Configure.**
1. Hub: nothing. If "Require authentication" is on (System page), create a Personal Access Token
   on the Users page (role User is enough) and keep it for the next step.
2. Home Assistant: install `ha-harmony-ng` through HACS. The Hub appears under Settings, Devices
   and services as a discovered device (mDNS); confirm it, choose the API transport and paste the
   token if you created one. Without mDNS across your network, add the integration by hand with
   the Hub's address.

## 2. The harmony-ng integration over MQTT

**What it is.** The same integration, with the broker in between: the Hub publishes a retained state
tree under its base topic (`hub`, `activity_state`, `bt`, `rf`, `config`, `inventory/*`,
`remote/<slot>/button`) and answers commands on `<base>/cmd` with a reply on `<base>/cmd/response`.
A parity test in the integration proves the two transports produce the same entity states.

**You get.** The same entities as layer 1.

**Pros.** Home Assistant needs no direct connection to the Hub (useful across VLANs or with a remote
Home Assistant); other MQTT consumers (Node-RED, dashboards, loggers) read the same retained
topics; QoS 1 delivery.

**Cons.** A broker to run; the broker adds a hop; the integration is still required.

**Configure.**
1. Hub: web UI, Settings, MQTT: enable, broker host and port, username and password, base topic
   (default `harmony/hub`), client id. Save. The connection badge turns green.
2. Home Assistant: the MQTT integration pointed at the same broker; install `ha-harmony-ng` and,
   in its flow, choose the MQTT transport and the base topic.

## 3. Home Assistant MQTT discovery

**What it is.** The Hub publishes Home Assistant discovery configs through the broker, and the core
MQTT integration creates the entities itself. No custom code in Home Assistant. The entities read
the v2 topics of layer 2 and write the Hub's command topics.

**You get.** One device "<hub name>" with: a select `Activity` (PowerOff and the activities),
sensors `Activity ID`, `IP Address`, `Firmware`, `IR Devices`, `IR Commands`, buttons `Power Off`,
`Sync`, `Remote 1 Sleep` and `Remote 2 Sleep`. Everything else (a command to a device, a key
pressed on the remote, a pulse from an MQTT-type device) is available to automations through the
topics in [API.md 8.1](API.md#81-mqtt-v2): an MQTT trigger on `<base>/remote/<slot>/button`, a
publish to `<base>/send_command` or `<base>/cmd`. The MQTT page of the new web UI has a helper that
writes the automation YAML for `send_command`.

**Pros.** Nothing to install in Home Assistant beyond MQTT; works with any MQTT client, not only
Home Assistant; the entities created by the first MQTT layer of harmony-ng keep their ids.

**Cons.** A small, fixed entity set: no per-device remotes, no key event entities, no IR emitters;
commands from Home Assistant go through automations or scripts, not entity services.

**Configure.**
1. Hub: the MQTT settings of layer 2 with "Create Home Assistant entities automatically" ticked and
   the discovery prefix equal to the one of your MQTT integration (`homeassistant` unless you
   changed it). Save.
2. Home Assistant: nothing. The device appears under the MQTT integration within seconds; it is
   republished whenever Home Assistant restarts (birth message), when the Hub reconnects, when you
   rename an activity or the Hub, and when you press `Sync`. Unticking the box removes the entities.

**MQTT-type devices.** A device of type MQTT in the Hub's configuration is driven through the
broker: a command to it (from an activity sequence, the remote or Home Assistant) publishes `1` and,
after the pulse time, `0` on the device's topic template (device editor, "Device MQTT Bridge"). That
is how a plug, a relay or a Node-RED flow becomes a device in an activity.

## 4. The Logitech protocol (core Harmony integration)

**What it is.** With "Logitech protocol compatibility" on, the Hub answers SSDP searches like a stock
Harmony Hub and serves the stock protocol on port 8088: the provisioning handshake, the WebSocket
envelope, the configuration in the stock layout, activity changes with the stock progress sequence,
`holdAction` commands and `changeChannel`. Home Assistant's core `harmony` integration and any other
stock client (the `aioharmony` library, Homebridge plugins) talk to it unchanged.

**You get.** What a stock Hub gave: the Hub as a `remote` entity (activities, `send_command` to a
device) and the `Activities` select. Entities are keyed on the stock `remoteId` stored on the Hub,
so a Hub that was used with the core integration before harmony-ng gets its old device, entity ids
and history back.

**Pros.** Nothing custom in Home Assistant and no broker; existing automations and dashboards built
on the core integration keep working; other stock clients work too.

**Cons.** The stock feature set only: no key events, no Bluetooth keys, no raw IR, no remote status;
port 8088 has no authentication (the stock protocol has none), so keep it off on networks you do
not trust; the stock protocol polls where layer 1 pushes.

**Configure.**
1. Hub: web UI, Settings, System, tick "Logitech protocol compatibility (port 8088)". Save. The
   Hub announces itself by SSDP at once.
2. Home Assistant: the core Harmony integration discovers the Hub within a minute (Settings,
   Devices and services, discovered); confirm it. If the Hub was named like your old one, the
   previous entities come back. Untick the setting to close the port; the entities go unavailable.

## Which one

- You run Home Assistant and want everything the Hub can do: layer 1, or layer 2 if you already run
  a broker and prefer not to open the Hub to Home Assistant directly.
- You want nothing custom in Home Assistant and a broker is already there: layer 3.
- You want nothing custom and no broker, or you have automations built on the stock Harmony
  integration: layer 4.
- Node-RED, Grafana or other MQTT consumers next to Home Assistant: layer 2 or 3, they share the
  same topics.
