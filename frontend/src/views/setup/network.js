// Network (Wi-Fi & Ethernet) Configuration View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';

// WPA pass phrase: 8 to 63 characters, or 64 hex digits. wpa_supplicant refuses anything else.
export function wifiPassphraseError(password) {
  if (!password) return null;
  if (password.length >= 8 && password.length <= 63) return null;
  if (/^[0-9a-fA-F]{64}$/.test(password)) return null;
  return t('wifi_pass_length_error', 'Wi-Fi password must be 8 to 63 characters (or 64 hex digits)');
}

export function createView() {
  const container = h('div', { className: 'setup-subview-network', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  const statusCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('network_status', 'Network Status') }),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: t('refresh', 'Refresh'),
        onclick: () => loadNetStatus(),
      }),
    ]),
    h('div', { id: 'net-status-mount', className: 'text-dim', text: t('loading_network_info', 'Loading network info...') }),
  ]);

  const wifiCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: t('wifi_connection_scan', 'Wi-Fi Connection & Scan') }),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: `📡 ${t('scan_networks', 'Scan Networks')}`,
        onclick: () => scanWifiNetworks(),
      }),
    ]),
    h('form', { onsubmit: (e) => saveWifi(e) }, [
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: t('network_ssid', 'Network SSID') }),
        h('input', { id: 'wifi-ssid-input', type: 'text', className: 'form-input', placeholder: 'MyHomeWiFi' }),  // i18n-ok: example value
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: t('wifi_password', 'Wi-Fi Password') }),
        h('input', { id: 'wifi-pass-input', type: 'password', className: 'form-input' }),
      ]),
      h('button', {
        type: 'submit',
        className: 'btn btn-primary',
        text: t('connect_wifi', 'Connect Wi-Fi'),
      }),
    ]),
    h('div', { id: 'wifi-scan-results', style: { marginTop: '12px' } }),
  ]);

  // Wi-Fi setup over USB (USB_WIFI_SETUP in ethernet.conf). The save route rewrites the whole
  // Ethernet file, so the stored Ethernet values the Hub reported are sent back unchanged. The
  // switch stays disabled until those values are loaded.
  let netConfig = null;
  const usbToggle = h('input', { id: 'usb-wifi-setup-toggle', type: 'checkbox', onchange: () => saveUsbWifiSetup() });
  usbToggle.disabled = true;
  const usbCard = h('div', { className: 'card' }, [
    h('label', { style: { display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer' } }, [
      usbToggle,
      h('span', { style: { fontWeight: '600' }, text: t('usb_wifi_setup_lbl', 'Wi-Fi setup over USB') }),
    ]),
    h('div', { className: 'text-dim', style: { fontSize: '12px', marginTop: '6px' }, text: t('usb_wifi_setup_desc', "A computer connected to the Hub's USB port can read the network name and set the Wi-Fi network (tools/set_wifi_usb.py). Turn this off if others can plug a cable into the Hub. It applies at once.") }),
  ]);

  async function saveUsbWifiSetup() {
    if (!netConfig) return;
    const on = usbToggle.checked;
    const c = netConfig;
    usbToggle.disabled = true;
    try {
      await api.saveEthernet({
        apply: 'save',
        eth_enabled: c.eth_enabled === false ? 0 : 1,
        eth_fallback_wifi: c.eth_fallback_wifi === false ? 0 : 1,
        eth_mode: c.eth_mode || 'dhcp',
        eth_ip: c.eth_ip || '',
        eth_netmask: c.eth_netmask || '',
        eth_gateway: c.eth_gateway || '',
        eth_dns: c.eth_dns || '',
        usb_wifi_setup: on ? 1 : 0,
      });
      netConfig = { ...c, usb_wifi_setup: on };
      toast.success(on
        ? t('usb_wifi_setup_turned_on', 'Wi-Fi setup over USB turned on')
        : t('usb_wifi_setup_turned_off', 'Wi-Fi setup over USB turned off'));
    } catch (err) {
      usbToggle.checked = !on;
      toast.error(err.message);
    } finally {
      usbToggle.disabled = false;
    }
  }

  async function loadNetStatus() {
    const mount = container.querySelector('#net-status-mount');
    if (!mount) return;
    try {
      const net = await api.getNetworkStatus();
      mount.innerHTML = '';
      if (net.config && typeof net.config.usb_wifi_setup === 'boolean') {
        netConfig = net.config;
        usbToggle.checked = net.config.usb_wifi_setup;
        usbToggle.disabled = false;
      }

      const grid = h('div', { style: { display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(140px, 1fr))', gap: '8px' } }, [
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: t('ip_address', 'IP Address') }),
          h('div', { style: { fontWeight: 'bold' }, text: net.ip || t('unknown', 'Unknown') }),
        ]),
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: t('net_type', 'Type') }),
          h('div', { style: { fontWeight: 'bold' }, text: net.connection_type || 'Wi-Fi' }),
        ]),
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: t('net_interface', 'Interface') }),
          h('div', { style: { fontWeight: 'bold' }, text: net.active_interface || 'ath0' }),
        ]),
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: t('mac_address', 'MAC Address') }),
          h('div', { style: { fontWeight: 'bold', fontSize: '11px' }, text: net.mac || net.wifi_mac || t('unknown', 'Unknown') }),
        ]),
      ]);

      mount.appendChild(grid);
      const drv = ethDriverNotice(net.eth_driver);
      if (drv) mount.appendChild(drv);
    } catch (e) {
      mount.textContent = t('error_msg', 'Error: {error}', { error: e.message });
    }
  }

  // USB Ethernet driver on demand: the Hub found an adapter whose driver is not installed
  let drvPoll = null;
  function ethDriverNotice(drv) {
    clearTimeout(drvPoll);
    if (!drv || (!drv.slow && !drv.unsupported && !(drv.missing && drv.missing.length) && !drv.installing && !drv.install_failed && !(drv.driver && drv.loaded === false))) return null;
    const id = drv.vid && drv.pid ? `${drv.vid}:${drv.pid}` : '';
    const driver = drv.driver || '';
    const box = h('div', { className: 'card', style: { margin: '8px 0 0', padding: '8px', display: 'flex', gap: '8px', alignItems: 'center', flexWrap: 'wrap' } });
    if (drv.installing) {
      box.appendChild(h('span', { className: 'text-dim', text: t('eth_driver_downloading', 'Downloading the Ethernet driver {driver}...', { driver }) }));
      drvPoll = setTimeout(() => loadNetStatus(), 3000);
      return box;
    }
    if (drv.slow) {
      box.appendChild(h('span', { text: t('eth_driver_slow', 'USB adapter {id} runs at full speed (USB 1.1). Such an adapter froze the Hub in a test, so Ethernet stays off. Use a USB 2.0 (high-speed) adapter, hub or OTG cable.', { id }) }));
      return box;
    }
    if (drv.unsupported) {
      box.appendChild(h('span', { text: t('eth_driver_unsupported', 'USB adapter {id} is not supported by this firmware.', { id }) }));
      return box;
    }
    // files there but the module did not load (an old build): download it again
    const notLoaded = !drv.install_failed && !(drv.missing && drv.missing.length);
    box.appendChild(h('span', {
      text: drv.install_failed
        ? t('eth_driver_failed', 'The Ethernet driver {driver} could not be downloaded.', { driver })
        : notLoaded
          ? t('eth_driver_not_loaded', 'The Ethernet driver {driver} is installed but could not be loaded.', { driver })
          : t('eth_driver_missing', 'USB Ethernet adapter {id} found: its driver {driver} is not installed.', { id, driver }),
    }));
    if (driver) {
      const btn = h('button', {
        className: 'btn btn-primary btn-sm',
        text: notLoaded ? t('eth_driver_redownload_btn', 'Download again') : t('eth_driver_download_btn', 'Download driver'),
        onclick: async () => {
          btn.disabled = true;
          try {
            await api.installEthernetDriver(driver);
            loadNetStatus();
          } catch (err) {
            btn.disabled = false;
            toast.error(err.message);
          }
        },
      });
      box.appendChild(btn);
    }
    return box;
  }

  async function scanWifiNetworks() {
    const resultsMount = container.querySelector('#wifi-scan-results');
    if (!resultsMount) return;
    resultsMount.innerHTML = `<div class="text-dim">${t('scanning_wifi_bands', 'Scanning Wi-Fi bands...')}</div>`;
    try {
      const res = await api.scanWifi();
      const networks = Array.isArray(res) ? res : (res?.networks || res?.aps || []);
      resultsMount.innerHTML = '';
      if (networks.length === 0) {
        resultsMount.textContent = t('no_networks_found', 'No networks found');
        return;
      }
      const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px' } });
      networks.forEach(n => {
        const ssid = n.ssid || n.name;
        if (!ssid) return;
        const row = h('div', {
          className: 'card',
          style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '8px 12px', margin: 0, cursor: 'pointer' },
          onclick: () => {
            const input = container.querySelector('#wifi-ssid-input');
            if (input) input.value = ssid;
          },
        }, [
          h('span', { style: { fontWeight: '600' }, text: ssid }),
          h('span', { className: 'badge', text: `${n.signal || n.rssi || ''} dBm` }),
        ]);
        list.appendChild(row);
      });
      resultsMount.appendChild(list);
    } catch (e) {
      resultsMount.textContent = t('scan_failed_error', 'Scan failed: {error}', { error: e.message });
    }
  }

  async function saveWifi(e) {
    e.preventDefault();
    const ssid = container.querySelector('#wifi-ssid-input')?.value.trim();
    const password = container.querySelector('#wifi-pass-input')?.value;
    if (!ssid) return toast.warn(t('enter_wifi_ssid', 'Enter Wi-Fi SSID'));
    const passError = wifiPassphraseError(password);
    if (passError) return toast.warn(passError);

    try {
      toast.info(t('wifi_testing_before_switch', 'Testing the Wi-Fi connection before switching (up to 15 s)...'));
      const res = await api.saveWifi({ ssid, password });
      toast.success(res?.message || t('wifi_verified_applied', 'Wi-Fi settings verified and applied'));
    } catch (err) {
      toast.error(err.message);
    }
  }

  container.appendChild(statusCard);
  container.appendChild(wifiCard);
  container.appendChild(usbCard);
  loadNetStatus();

  return container;
}

export default createView;
