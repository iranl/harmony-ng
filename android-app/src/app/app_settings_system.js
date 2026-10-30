import { HarmonyClient } from '../api.js';
import { getSavedHubs, saveHub, deleteHub, getActiveHub, setActiveHubId } from '../storage.js';
import { RemoteEngine } from '../remote-engine.js';
import {
  getHarmonyManufacturers,
  searchHarmonyManufacturers,
  fetchHarmonyModels,
  searchHarmonyModels,
  fetchHarmonyDeviceDetail,
  fetchHarmonyCodeset,
  mapHarmonyDeviceType,
  buildDeviceFromHarmony,
  parseIrText,
  loadIrdSource,
  rankIrdEntries,
  fetchIrdFile,
  sourceLabel,
  safeImportName
} from '../irdb.js';
import { t, setLanguage, getInitialLanguage } from '../i18n.js';
import iconMetadata from '../icon-metadata.json';
import {
  initIconCache,
  hasCachedIcon,
  getCachedIconUrl,
  cacheIconFromUrl,
  syncIconsFromHub,
  syncIconsFromOta,
  getCachedIconsCount,
  clearIconCache,
  getAllCachedKeys
} from '../icon-cache.js';
import {
  APP_VERSION,
  stockIconsAvailable,
  stockIconBaseUrl,
  failedIconUrls,
  setStockIconBaseUrl,
  setStockIconsAvailable,
  escapeHtml,
  autoInvertDarkIcon,
  showToast,
  showConfirm,
  isCustomIcon,
  isDarkMonochromeIcon,
  getStockIconUrl,
  getActivityIconSvg,
  getRawActivityIconSvg,
  getScreenBtnIconSvg,
  getRawScreenBtnIconSvg,
  getDeviceIconSvg,
  getRawDeviceIconSvg
} from './app_icons_ui.js';
import { HarmonyAppRemotesRf } from './app_remotes_rf.js';

export class HarmonyAppSettingsSystem extends HarmonyAppRemotesRf {
  async loadMqttView() {
    const badge = document.getElementById('mqttConnBadge');
    try {
      const st = await this.client.getMqttStatus();
      if (badge) {
        badge.className = `badge ${st.connected ? 'ok' : 'bad'}`;
        badge.textContent = st.connected ? 'Connected' : 'Disconnected';
      }
      if (st.config) {
        document.getElementById('mqttEnabled').checked = Boolean(st.config.enabled);
        document.getElementById('mqttHost').value = st.config.host || '';
        document.getElementById('mqttPort').value = st.config.port || 1883;
        document.getElementById('mqttUsername').value = st.config.username || '';
        document.getElementById('mqttBaseTopic').value = st.config.baseTopic || 'harmony/hub';
        document.getElementById('mqttDiscoveryPrefix').value = st.config.discoveryPrefix || 'homeassistant';
        document.getElementById('mqttClientId').value = st.config.clientId || 'harmony_hub';
        document.getElementById('mqttName').value = st.config.name || 'Harmony Hub';
        document.getElementById('mqttKeepAlive').value = st.config.keepAlive || 60;
        document.getElementById('mqttHaDiscovery').checked = Boolean(st.config.haDiscovery);
      }
    } catch (err) {
      if (badge) badge.textContent = 'Offline';
    }

    await this.loadHaActionHelper().catch(() => {});
  }

  async loadHaActionHelper() {
    const devSelect = document.getElementById('haHelperDevice');
    if (!devSelect) return;

    if (!this.haHelperQueue) {
      this.haHelperQueue = [];
    }

    if (!this.inventory || !this.inventory.devices || !this.inventory.devices.length) {
      try {
        this.inventory = await this.client.getInventory();
      } catch (e) {
        this.inventory = { devices: [] };
      }
    }

    const devs = this.inventory?.devices || [];
    const prevDevVal = devSelect.value;
    devSelect.innerHTML = devs.map(d => `<option value="${this.escapeHtml(d.id)}">${this.escapeHtml(d.name || ('Device ' + d.id))}</option>`).join('');

    if (prevDevVal && devs.some(d => String(d.id) === String(prevDevVal))) {
      devSelect.value = prevDevVal;
    } else if (devs.length > 0) {
      devSelect.value = String(devs[0].id);
    }

    await this.updateHaHelperCommandsDropdown();
    this.renderHaHelperQueue();
    this.updateHaActionYaml();
  }

  async updateHaHelperCommandsDropdown() {
    const devSelect = document.getElementById('haHelperDevice');
    const cmdSelect = document.getElementById('haHelperCommandSelect');
    if (!devSelect || !cmdSelect) return;

    const devId = devSelect.value;
    if (!devId) {
      cmdSelect.innerHTML = '<option value="">No device selected</option>';
      return;
    }

    let cmds = this.deviceCommandsMap ? this.deviceCommandsMap[devId] : null;
    if (!cmds || !cmds.length) {
      const dev = (this.inventory?.devices || []).find(d => String(d.id) === String(devId));
      if (dev && Array.isArray(dev.commands) && dev.commands.length) {
        cmds = dev.commands;
      } else {
        try {
          cmds = await this.client.getDeviceCommands(devId);
        } catch {
          cmds = [];
        }
      }
      if (!this.deviceCommandsMap) this.deviceCommandsMap = {};
      this.deviceCommandsMap[devId] = cmds || [];
    }

    if (!cmds || !cmds.length) {
      cmdSelect.innerHTML = '<option value="">No commands available</option>';
      return;
    }

    cmdSelect.innerHTML = cmds.map(c => {
      const name = typeof c === 'string' ? c : (c.name || c.Name || '');
      return `<option value="${this.escapeHtml(name)}">${this.escapeHtml(name)}</option>`;
    }).filter(opt => !opt.includes('value=""')).join('');
  }

  renderHaHelperQueue() {
    const container = document.getElementById('haHelperCmdQueue');
    const countEl = document.getElementById('haHelperCmdCount');
    if (!container) return;

    const queue = this.haHelperQueue || [];
    if (countEl) countEl.textContent = queue.length;

    if (!queue.length) {
      container.innerHTML = '<div class="muted mini" style="text-align: center; padding: 10px;">No commands added yet. Pick a command above and click "+ Add".</div>';
      return;
    }

    container.innerHTML = queue.map((cmd, idx) => `
      <div style="display: flex; justify-content: space-between; align-items: center; padding: 4px 8px; background: var(--bg-card); border-radius: var(--radius-sm); margin-bottom: 4px; border: 1px solid var(--border); font-size: 12px;">
        <div style="display: flex; align-items: center; gap: 8px;">
          <span style="font-family: monospace; color: var(--text-muted); font-size: 11px;">#${idx + 1}</span>
          <strong style="color: var(--text-main);">${this.escapeHtml(cmd)}</strong>
        </div>
        <div style="display: flex; gap: 4px; align-items: center;">
          <button type="button" class="btn btn-xs btn-ghost btn-ha-cmd-up" data-idx="${idx}" ${idx === 0 ? 'disabled style="opacity:0.3"' : ''} title="Move up">▲</button>
          <button type="button" class="btn btn-xs btn-ghost btn-ha-cmd-down" data-idx="${idx}" ${idx === queue.length - 1 ? 'disabled style="opacity:0.3"' : ''} title="Move down">▼</button>
          <button type="button" class="btn btn-xs btn-ghost btn-ha-cmd-del" data-idx="${idx}" style="color: var(--danger);" title="Remove">✕</button>
        </div>
      </div>
    `).join('');

    container.querySelectorAll('.btn-ha-cmd-up').forEach(btn => {
      btn.addEventListener('click', (e) => {
        e.stopPropagation();
        const idx = Number(btn.getAttribute('data-idx'));
        if (idx > 0) {
          const item = this.haHelperQueue.splice(idx, 1)[0];
          this.haHelperQueue.splice(idx - 1, 0, item);
          this.renderHaHelperQueue();
          this.updateHaActionYaml();
        }
      });
    });

    container.querySelectorAll('.btn-ha-cmd-down').forEach(btn => {
      btn.addEventListener('click', (e) => {
        e.stopPropagation();
        const idx = Number(btn.getAttribute('data-idx'));
        if (idx < this.haHelperQueue.length - 1) {
          const item = this.haHelperQueue.splice(idx, 1)[0];
          this.haHelperQueue.splice(idx + 1, 0, item);
          this.renderHaHelperQueue();
          this.updateHaActionYaml();
        }
      });
    });

    container.querySelectorAll('.btn-ha-cmd-del').forEach(btn => {
      btn.addEventListener('click', (e) => {
        e.stopPropagation();
        const idx = Number(btn.getAttribute('data-idx'));
        this.haHelperQueue.splice(idx, 1);
        this.renderHaHelperQueue();
        this.updateHaActionYaml();
      });
    });
  }

  updateHaActionYaml() {
    const previewEl = document.getElementById('haHelperYamlPreview');
    if (!previewEl) return;

    const devSelect = document.getElementById('haHelperDevice');
    const devId = devSelect?.value;
    const dev = (this.inventory?.devices || []).find(d => String(d.id) === String(devId));
    const devName = dev?.name || dev?.id || 'Target Device';

    const queue = this.haHelperQueue || [];
    const delayVal = parseFloat(document.getElementById('haHelperDelay')?.value);
    const holdVal = parseFloat(document.getElementById('haHelperHold')?.value);
    const repeatsVal = parseInt(document.getElementById('haHelperRepeats')?.value, 10);
    const useAutomationIndent = document.getElementById('haHelperIndentCheck')?.checked;

    const p0 = useAutomationIndent ? '      ' : '';
    const p1 = useAutomationIndent ? '        ' : '  ';
    const p2 = `${p1}  `;
    const p3 = `${p2}  `;

    const lines = [];
    lines.push(`${p0}- action: script.harmony_send_command`);
    lines.push(`${p1}data:`);
    lines.push(`${p2}device: ${devName}`);

    if (queue.length === 0) {
      lines.push(`${p2}command:`);
      lines.push(`${p3}- # Pick a command above and click "+ Add"`);
    } else {
      lines.push(`${p2}command:`);
      queue.forEach(cmd => {
        lines.push(`${p3}- ${cmd}`);
      });
    }

    if (!isNaN(delayVal) && delayVal >= 0) {
      lines.push(`${p2}delay_secs: ${delayVal}`);
    }
    if (!isNaN(holdVal) && holdVal >= 0) {
      lines.push(`${p2}hold_secs: ${holdVal}`);
    }
    if (!isNaN(repeatsVal) && repeatsVal > 1) {
      lines.push(`${p2}num_repeats: ${repeatsVal}`);
    }

    previewEl.textContent = lines.join('\n');
  }

  async copyHaActionYaml() {
    const previewEl = document.getElementById('haHelperYamlPreview');
    const statusEl = document.getElementById('haHelperActionStatus');
    const copyBtn = document.getElementById('btnHaHelperCopy');
    if (!previewEl || !previewEl.textContent) return;

    try {
      await navigator.clipboard.writeText(previewEl.textContent);
      if (copyBtn) copyBtn.textContent = '✓ Copied!';
      if (statusEl) statusEl.innerHTML = '<span style="color:var(--success)">Copied to clipboard</span>';
      setTimeout(() => {
        if (copyBtn) copyBtn.textContent = '📋 Copy YAML';
        if (statusEl) statusEl.textContent = '';
      }, 2000);
    } catch (err) {
      if (statusEl) statusEl.innerHTML = `<span style="color:var(--danger)">Copy failed: ${err.message}</span>`;
    }
  }

  async testHaActionSequence() {
    const devSelect = document.getElementById('haHelperDevice');
    const devId = devSelect?.value;
    const queue = this.haHelperQueue || [];
    const statusEl = document.getElementById('haHelperActionStatus');
    const testBtn = document.getElementById('btnHaHelperTest');

    if (!devId) {
      if (statusEl) statusEl.textContent = 'Select device first';
      return;
    }
    if (!queue.length) {
      if (statusEl) statusEl.textContent = 'Add commands first';
      return;
    }

    const delaySecs = parseFloat(document.getElementById('haHelperDelay')?.value) || 0.4;
    const repeats = parseInt(document.getElementById('haHelperRepeats')?.value, 10) || 1;

    if (testBtn) testBtn.disabled = true;
    if (statusEl) statusEl.textContent = 'Running sequence...';

    try {
      for (let r = 0; r < repeats; r++) {
        for (let i = 0; i < queue.length; i++) {
          const cmd = queue[i];
          if (statusEl) {
            statusEl.textContent = repeats > 1
              ? `Rep ${r + 1}/${repeats} • Cmd ${i + 1}/${queue.length}: ${cmd}`
              : `Cmd ${i + 1}/${queue.length}: ${cmd}`;
          }
          await this.client.sendCommand(devId, cmd);
          if (i < queue.length - 1 || r < repeats - 1) {
            await new Promise(res => setTimeout(res, Math.max(50, delaySecs * 1000)));
          }
        }
      }
      if (statusEl) statusEl.innerHTML = '<span style="color:var(--success)">✓ Test completed!</span>';
    } catch (err) {
      if (statusEl) statusEl.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    } finally {
      if (testBtn) testBtn.disabled = false;
      setTimeout(() => {
        if (statusEl) statusEl.textContent = '';
      }, 4000);
    }
  }

  async saveMqttSettings() {
    const status = document.getElementById('mqttSaveStatus');
    const data = {
      enabled: document.getElementById('mqttEnabled')?.checked ? 1 : 0,
      host: document.getElementById('mqttHost')?.value.trim(),
      port: Number(document.getElementById('mqttPort')?.value) || 1883,
      username: document.getElementById('mqttUsername')?.value.trim(),
      password: document.getElementById('mqttPassword')?.value,
      keep_password: document.getElementById('mqttKeepPass')?.checked ? 1 : 0,
      baseTopic: document.getElementById('mqttBaseTopic')?.value.trim(),
      discoveryPrefix: document.getElementById('mqttDiscoveryPrefix')?.value.trim(),
      clientId: document.getElementById('mqttClientId')?.value.trim(),
      name: document.getElementById('mqttName')?.value.trim(),
      keepAlive: Number(document.getElementById('mqttKeepAlive')?.value) || 60,
      haDiscovery: document.getElementById('mqttHaDiscovery')?.checked ? 1 : 0
    };

    try {
      if (status) status.textContent = 'Saving MQTT settings...';
      await this.client.saveMqtt(data);
      if (status) status.innerHTML = '<span style="color:var(--success)">Saved!</span>';
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  /* -------------------------------------------------------------
   * 10. Network View (Wi-Fi & Ethernet)
   * ------------------------------------------------------------- */
  async loadNetworkView() {
    try {
      const [st, sys] = await Promise.all([
        this.client.getNetworkStatus().catch(() => ({})),
        this.client.getSystemInfo().catch(() => ({}))
      ]);

      // Active Connection & Interface
      document.getElementById('netConnType').textContent = st.connection_type || 'Unknown';
      const activeIfEl = document.getElementById('netActiveIf');
      if (activeIfEl) {
        if (st.eth_present && st.wifi_connected) {
          const isEthActive = (st.active_interface === st.eth_interface) || (st.mode === 'ethernet');
          if (isEthActive) {
            activeIfEl.textContent = `${st.active_interface} (Ethernet Active + Wi-Fi Standby)`;
          } else {
            activeIfEl.textContent = (st.eth_carrier)
              ? `${st.active_interface} (Wi-Fi Active + Ethernet Linking)`
              : `${st.active_interface} (Wi-Fi Active + Ethernet Standby)`;
          }
        } else {
          activeIfEl.textContent = st.active_interface || 'none';
        }
      }

      // IP Address
      const ipEl = document.getElementById('netIp');
      if (ipEl) {
        if (st.eth_ip && st.wifi_ip && st.eth_ip === st.wifi_ip) {
          ipEl.textContent = `${st.ip} (Unified Static IP across Eth & Wi-Fi)`;
        } else if (st.eth_ip && st.wifi_ip) {
          ipEl.textContent = `${st.ip} (Eth: ${st.eth_ip} | Wi-Fi: ${st.wifi_ip})`;
        } else {
          ipEl.textContent = st.ip || 'None';
        }
      }
      document.getElementById('netMask').textContent = st.netmask || '-';
      document.getElementById('netGw').textContent = st.gateway || '-';
      document.getElementById('netMac').textContent = st.mac || '-';

      // USB Controller Mode
      const usbEl = document.getElementById('netUsbMode');
      if (usbEl) {
        if (st.eth_onboard) {
          usbEl.innerHTML = '<span style="color:var(--success); font-weight:600;">Gadget Mode (Native PC USB Control Active)</span>';
        } else {
          usbEl.innerHTML = st.usb_host_mode
            ? '<span style="color:var(--success); font-weight:600;">Host Mode (High-Speed 480Mbps active)</span>'
            : '<span style="color:var(--text-muted);">Gadget Mode (USB Console/PC)</span>';
        }
      }

      // Ethernet Adapter Status
      const ethEl = document.getElementById('netEthStatus');
      if (ethEl) {
        if (st.eth_present) {
          const ifStr = st.eth_interface ? ` [${st.eth_interface}]` : '';
          const devType = st.eth_onboard ? 'Onboard RJ45 / PoE' : '100Base-TX';
          if (st.eth_carrier) {
            const ipStr = st.eth_ip ? ` (IP: ${st.eth_ip})` : ' (Acquiring IP...)';
            ethEl.innerHTML = `<span style="color:var(--success); font-weight:600;">Connected (${devType} Link Up)${ifStr}</span>${ipStr}`;
          } else {
            const devLabel = st.eth_onboard ? 'Onboard RJ45 Port' : 'Adapter Detected';
            ethEl.innerHTML = `<span style="color:var(--warning);">${devLabel}${ifStr} (Cable Unplugged)</span>`;
          }
        } else {
          ethEl.innerHTML = '<span style="color:var(--text-muted);">Not Detected</span>';
        }
      }

      // Wi-Fi Status
      const wifiEl = document.getElementById('netWifiStatus');
      if (wifiEl) {
        if (st.wifi_connected) {
          const ssidStr = st.wifi_ssid ? ` [${st.wifi_ssid}]` : '';
          const ipStr = st.wifi_ip ? ` (IP: ${st.wifi_ip})` : '';
          wifiEl.innerHTML = `<span style="color:var(--success); font-weight:600;">Connected${ssidStr}</span>${ipStr}`;
        } else {
          wifiEl.innerHTML = '<span style="color:var(--danger);">Disconnected</span>';
        }
      }

      // Pre-fill Ethernet & IP configuration form
      if (st.config) {
        const modeSel = document.getElementById('ethModeSelect');
        if (modeSel && st.config.eth_mode) {
          modeSel.value = st.config.eth_mode;
          const staticFields = document.getElementById('ethStaticFields');
          if (staticFields) staticFields.style.display = (st.config.eth_mode === 'static') ? 'block' : 'none';
        }
        if (st.config.eth_ip && document.getElementById('ethIp')) document.getElementById('ethIp').value = st.config.eth_ip;
        if (st.config.eth_netmask && document.getElementById('ethNetmask')) document.getElementById('ethNetmask').value = st.config.eth_netmask;
        if (st.config.eth_gateway && document.getElementById('ethGateway')) document.getElementById('ethGateway').value = st.config.eth_gateway;
        if (st.config.eth_dns && document.getElementById('ethDns')) document.getElementById('ethDns').value = st.config.eth_dns;
      }

      // Ensure IP assignment mode select toggles static fields
      const modeSel = document.getElementById('ethModeSelect');
      if (modeSel && !modeSel._hasChangeAttached) {
        modeSel._hasChangeAttached = true;
        modeSel.addEventListener('change', () => {
          const sf = document.getElementById('ethStaticFields');
          if (sf) sf.style.display = (modeSel.value === 'static') ? 'block' : 'none';
        });
      }

      const ifcPre = document.getElementById('netIfconfigPre');
      if (ifcPre) {
        ifcPre.textContent = st.ifconfig ? st.ifconfig.trim() : 'No ifconfig output available.';
      }
      const netPre = document.getElementById('sysNetInfoPre');
      if (netPre && sys && sys.network_details) {
        netPre.textContent = sys.network_details.trim();
      }
    } catch (err) {
      console.warn('Network status error:', err);
    }
  }

  async saveEthernetConfig(applyMode = 'save') {
    const status = document.getElementById('ethSaveStatus');
    const data = {
      apply: applyMode,
      eth_enabled: 1,
      eth_fallback_wifi: 1,
      eth_mode: document.getElementById('ethModeSelect')?.value || 'dhcp',
      eth_ip: document.getElementById('ethIp')?.value.trim(),
      eth_netmask: document.getElementById('ethNetmask')?.value.trim(),
      eth_gateway: document.getElementById('ethGateway')?.value.trim(),
      eth_dns: document.getElementById('ethDns')?.value.trim()
    };
    try {
      if (status) status.textContent = 'Saving IP settings...';
      await this.client.saveEthernet(data);
      if (status) status.innerHTML = '<span style="color:var(--success)">Saved!</span>';
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  escapeHtml(str) {
    if (!str) return '';
    return String(str)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  async scanWifiNetworks() {
    const btn = document.getElementById('btnWifiScan');
    const btnText = document.getElementById('btnWifiScanText');
    const dropdown = document.getElementById('wifiScanDropdown');
    const status = document.getElementById('wifiSaveStatus');
    if (!dropdown) return;

    const originalText = btnText ? btnText.textContent : 'Scan Networks';
    if (btn) btn.disabled = true;
    if (btnText) btnText.textContent = 'Scanning...';
    if (status) status.innerHTML = '<span class="subtle">Scanning 2.4GHz channels...</span>';

    try {
      const res = await this.client.scanWifi();
      const networks = (res && res.networks) ? res.networks : [];
      if (networks.length === 0) {
        dropdown.innerHTML = '<div style="padding:12px; font-size:12px; color:var(--text-dim); text-align:center;">No networks found</div>';
        dropdown.style.display = 'block';
        if (status) status.textContent = '';
        return;
      }

      let html = '';
      networks.forEach(net => {
        const rawSsid = net.ssid || '';
        const safeSsid = this.escapeHtml(rawSsid);
        const sig = Number(net.signal) || -80;
        let bars = '●○○○';
        if (sig >= -55) bars = '●●●●';
        else if (sig >= -68) bars = '●●●○';
        else if (sig >= -78) bars = '●●○○';

        const secBadge = this.escapeHtml(net.security || 'WPA2');
        const isOpen = secBadge.toLowerCase() === 'open';

        html += `
          <div class="wifi-scan-item" data-ssid="${safeSsid}" data-open="${isOpen ? 1 : 0}">
            <div class="wifi-scan-info">
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" style="flex-shrink:0; color:var(--accent);">
                <path d="M5 12.55a11 11 0 0 1 14.08 0M1.42 9a16 16 0 0 1 21.16 0M8.53 16.11a6 6 0 0 1 6.95 0M12 20h.01"/>
              </svg>
              <span class="wifi-scan-ssid">${safeSsid}</span>
            </div>
            <div class="wifi-scan-meta">
              <span class="wifi-scan-badge">${secBadge}</span>
              <span class="wifi-scan-signal" title="${sig} dBm (Ch ${net.channel || '?'})">${bars}</span>
            </div>
          </div>
        `;
      });

      dropdown.innerHTML = html;
      dropdown.style.display = 'block';
      if (status) status.textContent = '';

      dropdown.querySelectorAll('.wifi-scan-item').forEach(item => {
        item.addEventListener('click', () => {
          const ssid = item.getAttribute('data-ssid');
          const isOpen = item.getAttribute('data-open') === '1';
          const input = document.getElementById('wifiSsidInput');
          if (input) {
            input.value = ssid;
            input.dispatchEvent(new Event('input', { bubbles: true }));
          }
          const openChk = document.getElementById('wifiOpen');
          if (openChk) openChk.checked = isOpen;

          const passInput = document.getElementById('wifiPskInput');
          if (passInput) {
            if (isOpen) {
              passInput.value = '';
              passInput.disabled = true;
            } else {
              passInput.disabled = false;
              passInput.focus();
            }
          }
          dropdown.style.display = 'none';
        });
      });
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Scan error: ${err.message}</span>`;
    } finally {
      if (btn) btn.disabled = false;
      if (btnText) btnText.textContent = originalText;
    }
  }

  async testWifiConfig() {
    const status = document.getElementById('wifiSaveStatus');
    const ssid = document.getElementById('wifiSsidInput')?.value.trim();
    if (!ssid) {
      if (status) status.innerHTML = '<span style="color:var(--danger)">Please enter an SSID first</span>';
      return;
    }

    const data = {
      ssid: ssid,
      password: document.getElementById('wifiPskInput')?.value,
      keep_password: document.getElementById('wifiKeepPass')?.checked ? 1 : 0,
      hidden: document.getElementById('wifiHidden')?.checked ? 1 : 0,
      open: document.getElementById('wifiOpen')?.checked ? 1 : 0
    };

    const testBtn = document.getElementById('btnWifiTest');
    if (testBtn) testBtn.disabled = true;

    try {
      if (status) status.innerHTML = '<span class="subtle">Testing Wi-Fi connection with AP (10-15s)...</span>';
      const res = await this.client.testWifi(data);
      if (res && res.ok) {
        if (status) status.innerHTML = '<span style="color:var(--success); font-weight:600;">Connection test passed!</span>';
      } else {
        const errMsg = (res && res.error) ? res.error : 'Connection test failed';
        if (status) status.innerHTML = `<span style="color:var(--danger); font-weight:600;">${errMsg}</span>`;
      }
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger); font-weight:600;">Test failed: ${err.message}</span>`;
    } finally {
      if (testBtn) testBtn.disabled = false;
    }
  }

  async saveWifiConfig(applyMode = 'save') {
    const status = document.getElementById('wifiSaveStatus');
    const ssid = document.getElementById('wifiSsidInput')?.value.trim();
    if (!ssid) {
      if (status) status.innerHTML = '<span style="color:var(--danger)">Please enter an SSID</span>';
      return;
    }

    const data = {
      apply: applyMode,
      ssid: ssid,
      password: document.getElementById('wifiPskInput')?.value,
      keep_password: document.getElementById('wifiKeepPass')?.checked ? 1 : 0,
      hidden: document.getElementById('wifiHidden')?.checked ? 1 : 0,
      open: document.getElementById('wifiOpen')?.checked ? 1 : 0
    };

    const isApplying = (applyMode === 'reconfigure' || applyMode === 'reboot' || applyMode === 'apply');

    try {
      if (status) {
        status.innerHTML = isApplying
          ? '<span class="subtle">Testing connection before switching...</span>'
          : '<span class="subtle">Saving Wi-Fi...</span>';
      }
      const res = await this.client.saveWifi(data);
      if (res && res.ok === false) {
        throw new Error(res.error || 'Failed to save Wi-Fi');
      }
      if (status) {
        status.innerHTML = isApplying
          ? '<span style="color:var(--success); font-weight:600;">Wi-Fi verified & applied!</span>'
          : '<span style="color:var(--success); font-weight:600;">Wi-Fi settings saved!</span>';
      }
    } catch (err) {
      if (status) {
        status.innerHTML = `<span style="color:var(--danger); font-weight:600;">${err.message}</span>`;
      }
    }
  }

  /* -------------------------------------------------------------
   * 11. Backup & Restore View
   * ------------------------------------------------------------- */
  loadBackupView() { }

  async exportBackupItem(target) {
    try {
      const url = this.client.baseUrl ? `${this.client.baseUrl}/export/${target}` : `/export/${target}`;
      const res = await fetch(url, { headers: this.client.getHeaders() });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const blob = await res.blob();
      const a = document.createElement('a');
      const filename = target === 'bundle' ? 'harmony_backup_bundle.json' :
        (target === 'network' || target === 'wifi' || target === 'auth') ?
          `${target}.conf` : `${target}.json`;
      a.href = URL.createObjectURL(blob);
      a.download = filename;
      document.body.appendChild(a);
      a.click();
      setTimeout(() => {
        URL.revokeObjectURL(a.href);
        a.remove();
      }, 1000);
    } catch (err) {
      alert(t("export_failed") + err.message);
    }
  }

  async restoreBackupPayload() {
    const target = document.getElementById('backupTarget')?.value;
    const payload = document.getElementById('backupPayload')?.value.trim();
    const restoreNet = !!document.getElementById('restoreNetworkCheckbox')?.checked;
    const status = document.getElementById('backupImportStatus');
    if (!payload) return alert(t("enter_backup_payload"));
    const confirmMsg = restoreNet ?
      t('restore_backup_net_confirm').replace('{target}', target) :
      t('restore_backup_safe_confirm').replace('{target}', target);
    if (!await showConfirm(confirmMsg, { title: t('title_restore_backup'), okText: t('btn_restore') })) return;
    try {
      if (status) status.textContent = t('restoring_status');
      await this.client.restoreBackup(target, payload, restoreNet);
      if (status) status.innerHTML = `<span style="color:var(--success)">${t('restored_status')}</span>`;
      alert(t("backup_restore_complete"));
      await this.refreshData();
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">${t('restore_failed_status')}${err.message}</span>`;
    }
  }

  /* -------------------------------------------------------------
   * 12. System Management View
   * ------------------------------------------------------------- */
  async loadSystemView() {
    this.client.getSettings().then(st => {
      if (st) {
        if (st.hub_name) {
          const inp = document.getElementById('hubCustomNameInput');
          if (inp) inp.value = st.hub_name;
          const label = document.getElementById('activeHubLabel');
          if (label) label.textContent = st.hub_name;
        }
        const ntpInp = document.getElementById('hubNtpServerInput');
        if (ntpInp && st.ntp_server) ntpInp.value = st.ntp_server;
        const tzEl = document.getElementById('hubTimezoneSelect') || document.getElementById('hubTimezoneInput');
        if (tzEl && st.timezone) {
          tzEl.value = st.timezone;
          if (!tzEl.value && st.timezone) {
            // Custom timezone not in preset list
            const opt = document.createElement('option');
            opt.value = st.timezone;
            opt.textContent = `Custom (${st.timezone})`;
            opt.selected = true;
            tzEl.appendChild(opt);
            tzEl.value = st.timezone;
          }
        }
        const ledSel = document.getElementById('hubLedModeSelect');
        if (ledSel && st.led_mode) {
          let mode = st.led_mode;
          if (mode === 'red') mode = 'always_on';
          else if (mode === 'amber' || mode === 'orange') mode = 'flashing_slow';
          ledSel.value = mode;
        }
      }
    }).catch(() => { });

    await Promise.allSettled([
      this.refreshServiceStatuses(),
      this.refreshSystemInfo(),
      this.refreshLogs(),
      this.loadUserAccounts(),
      this.loadPersonalAccessTokens()
    ]);
    this.updateIconCacheUI();
  }

  updateIconCacheUI() {
    if (!this.isNative) return;
    const badge = document.getElementById('iconCacheStatusBadge');
    const detail = document.getElementById('iconCacheDetail');
    if (!badge) return;

    const count = getCachedIconsCount();
    if (count > 0) {
      badge.className = 'badge badge-success';
      badge.style.background = 'rgba(16, 185, 129, 0.15)';
      badge.style.color = '#10b981';
      badge.textContent = `${count} Icons Cached`;
      if (detail) detail.textContent = 'Offline icon pack ready';
    } else if (stockIconsAvailable) {
      badge.className = 'badge';
      badge.style.background = 'rgba(59, 130, 246, 0.15)';
      badge.style.color = 'var(--accent)';
      badge.textContent = 'Streaming from Hub';
      if (detail) detail.textContent = 'Hub icons active; sync recommended for offline use';
    } else {
      badge.className = 'badge';
      badge.style.background = 'rgba(239, 68, 68, 0.15)';
      badge.style.color = '#ef4444';
      badge.textContent = 'No Icons Cached';
      if (detail) detail.textContent = 'Sync from Hub or OTA mirror';
    }
  }

  async syncIconPack(source = 'hub') {
    const progressWrap = document.getElementById('iconSyncProgressWrap');
    const statusText = document.getElementById('iconSyncStatusText');
    const progressBar = document.getElementById('iconSyncProgressBar');
    const pctLabel = document.getElementById('iconSyncPct');

    if (progressWrap) progressWrap.style.display = 'block';
    if (statusText) statusText.textContent = `Starting ${source === 'hub' ? 'Hub' : 'OTA'} icon sync...`;
    if (progressBar) progressBar.style.width = '0%';
    if (pctLabel) pctLabel.textContent = '0%';

    const onProgress = (p) => {
      const pct = p.percent || 0;
      if (progressBar) progressBar.style.width = `${pct}%`;
      if (pctLabel) pctLabel.textContent = `${pct}%`;
      if (statusText) {
        if (p.message) statusText.textContent = p.message;
        else if (p.filename) statusText.textContent = `Caching ${p.filename} (${p.current}/${p.total})...`;
      }
    };

    try {
      let res;
      if (source === 'hub') {
        res = await syncIconsFromHub(this.client.baseUrl, this.client, onProgress);
      } else {
        res = await syncIconsFromOta(onProgress);
      }
      setStockIconsAvailable(true);
      if (progressBar) progressBar.style.width = '100%';
      if (pctLabel) pctLabel.textContent = '100%';
      if (statusText) statusText.textContent = `Completed! ${res.count} icons cached locally.`;
      showToast(`Successfully cached ${res.count} icons offline!`, 'success');
      this.updateIconCacheUI();
      await this.loadAvailableIcons(true);
      if (this.currentTab === 'activity-remote') this.renderActivityRemote();
      else if (this.currentTab === 'control') this.renderControlView();
      setTimeout(() => {
        if (progressWrap) progressWrap.style.display = 'none';
      }, 4000);
    } catch (err) {
      console.error('Icon sync error:', err);
      if (statusText) statusText.textContent = `Failed: ${err.message}`;
      showToast(t("icon_sync_failed") + err.message, 'error');
      setTimeout(() => {
        if (progressWrap) progressWrap.style.display = 'none';
      }, 6000);
    }
  }

  async clearLocalIconCache() {
    const ok = await showConfirm('Clear local icon cache? Icons will need to be reloaded from the Hub.', {
      title: 'Clear Icon Cache',
      icon: '🗑️'
    });
    if (!ok) return;
    await clearIconCache();
    failedIconUrls.clear();
    this.updateIconCacheUI();
    showToast(t("icon_cache_cleared"), 'info');
  }

  async refreshServiceStatuses() {
    const rfBadge = document.getElementById('svcRfBadge');
    const btBadge = document.getElementById('svcBtBadge');
    const rfBtn = document.getElementById('btnToggleRfSvc');
    const btBtn = document.getElementById('btnToggleBtSvc');

    try {
      const svcs = await this.client.getServices();
      this.servicesState = svcs;

      if (rfBadge && rfBtn) {
        if (svcs.rf_enabled) {
          rfBadge.className = svcs.rf_running ? 'badge badge-success' : 'badge badge-warning';
          rfBadge.textContent = svcs.rf_running ? 'Running' : 'Enabled (Idle)';
          rfBtn.textContent = 'Disable RF';
          rfBtn.className = 'btn btn-sm btn-danger';
        } else {
          rfBadge.className = 'badge badge-danger';
          rfBadge.textContent = 'Disabled';
          rfBtn.textContent = 'Enable RF';
          rfBtn.className = 'btn btn-sm btn-primary';
        }
      }

      if (btBadge && btBtn) {
        if (svcs.bt_enabled) {
          btBadge.className = svcs.bt_running ? 'badge badge-success' : 'badge badge-warning';
          btBadge.textContent = svcs.bt_running ? 'Running' : 'Enabled (Idle)';
          btBtn.textContent = 'Disable Bluetooth';
          btBtn.className = 'btn btn-sm btn-danger';
        } else {
          btBadge.className = 'badge badge-danger';
          btBadge.textContent = 'Disabled';
          btBtn.textContent = 'Enable Bluetooth';
          btBtn.className = 'btn btn-sm btn-primary';
        }
      }
    } catch (err) {
      if (rfBadge) { rfBadge.className = 'badge badge-danger'; rfBadge.textContent = 'Offline'; }
      if (btBadge) { btBadge.className = 'badge badge-danger'; btBadge.textContent = 'Offline'; }
    }
  }

  async toggleRfService() {
    const isEnabled = this.servicesState ? this.servicesState.rf_enabled : false;
    const action = isEnabled ? 'disable' : 'enable';
    if (!await showConfirm(`${action === 'disable' ? 'Disable' : 'Enable'} Harmony Remote RF receiver service?`, { title: `${action === 'disable' ? 'Disable' : 'Enable'} RF Service`, okText: action === 'disable' ? 'Disable' : 'Enable' })) return;

    try {
      await this.client.toggleRfService(!isEnabled);
      await this.refreshServiceStatuses();
    } catch (err) {
      alert(t('failed_toggle_rf', 'Failed to toggle RF: ') + err.message);
    }
  }

  async toggleBtService() {
    const isEnabled = this.servicesState ? this.servicesState.bt_enabled : false;
    const action = isEnabled ? 'disable' : 'enable';
    if (!await showConfirm(`${action === 'disable' ? 'Disable' : 'Enable'} Bluetooth service?`, { title: `${action === 'disable' ? 'Disable' : 'Enable'} Bluetooth Service`, okText: action === 'disable' ? 'Disable' : 'Enable' })) return;

    try {
      await this.client.toggleBtService(!isEnabled);
      await this.refreshServiceStatuses();
    } catch (err) {
      alert(t('failed_toggle_bt', 'Failed to toggle Bluetooth: ') + err.message);
    }
  }

  async refreshLogs() {
    const logsEl = document.getElementById('sysLogsPre');
    const source = document.getElementById('sysLogSource')?.value || 'daemon';
    try {
      const res = await this.client.getLogs(source);
      if (logsEl) {
        logsEl.textContent = (res && (res.log || res.logs)) ? (res.log || res.logs) : (typeof res === 'string' ? res : 'No log entries available.');
        logsEl.scrollTop = logsEl.scrollHeight;
      }
    } catch (err) {
      if (logsEl) logsEl.textContent = 'Failed to load logs: ' + err.message;
    }
  }

  async refreshSystemInfo() {
    const infoEl = document.getElementById('sysInfoPre');
    const netEl = document.getElementById('sysNetInfoPre');

    try {
      const sys = await this.client.getSystemInfo();
      if (sys && sys.ok) {
        if (infoEl) {
          let text = `Kernel:        ${sys.kernel || 'Linux'}\n`;
          text += `Firmware:      ${sys.firmware || '4.15.600'}\n`;
          text += `Uptime:        ${sys.uptime || 'Unknown'}\n`;
          text += `Load Average:  ${sys.loadavg || 'N/A'}\n`;
          text += `Memory Total:  ${sys.mem_total_kb ? Math.round(sys.mem_total_kb / 1024) + ' MB (' + sys.mem_total_kb + ' kB)' : 'N/A'}\n`;
          text += `Memory Free:   ${sys.mem_free_kb ? Math.round(sys.mem_free_kb / 1024) + ' MB (' + sys.mem_free_kb + ' kB)' : 'N/A'}\n`;
          text += `Memory Avail:  ${sys.mem_available_kb ? Math.round(sys.mem_available_kb / 1024) + ' MB (' + sys.mem_available_kb + ' kB)' : 'N/A'}\n`;
          infoEl.textContent = text;
        }
        if (netEl && sys.network_details) {
          netEl.textContent = sys.network_details;
        }
      }
    } catch (err) {
      if (infoEl) infoEl.textContent = 'Failed to load system info: ' + err.message;
    }
  }

  async saveDebugLogging() {
    const status = document.getElementById('sysDebugStatus');
    const on = document.getElementById('sysDebugCheck')?.checked ? 1 : 0;
    try {
      await this.client.saveSystemAction('debug_logging', { debugLogging: on });
      if (status) status.innerHTML = '<span style="color:var(--success)">Saved!</span>';
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  applyRoleRestrictions() {
    const isUser = (this.userRole === 'user');

    // Settings nav button and drawer items
    document.querySelectorAll('.nav-tab-btn[data-tab="overview"], .drawer-item[data-view-target="overview"], .drawer-item[data-view-target="network"], .drawer-item[data-view-target="mqtt"], .drawer-item[data-view-target="backup"], .drawer-item[data-view-target="system"], .drawer-item[data-view-target="firmware"], .drawer-item[data-view-target="ir"], .drawer-item[data-view-target="bluetooth"], .drawer-item[data-view-target="remotes"]').forEach(el => {
      el.style.display = isUser ? 'none' : '';
    });

    // Activities editor & icon buttons
    document.querySelectorAll('#actNewBtn, .btn-act-edit, .btn-act-icon').forEach(el => {
      el.style.display = isUser ? 'none' : '';
    });

    // Role badge display in header if auth enabled
    const userBadge = document.getElementById('headerUserBadge');
    if (this.authEnabled && this.currentUser) {
      if (!userBadge) {
        const wrap = document.getElementById('hubStatusPill') || document.querySelector('.header-right');
        if (wrap) {
          const b = document.createElement('span');
          b.id = 'headerUserBadge';
          b.className = 'badge';
          b.style.fontSize = '10px';
          b.style.marginLeft = '6px';
          b.style.background = isUser ? 'rgba(59, 130, 246, 0.15)' : 'rgba(16, 185, 129, 0.15)';
          b.style.color = isUser ? 'var(--accent)' : '#10b981';
          b.textContent = `${this.currentUser} (${isUser ? 'User' : 'Admin'})`;
          wrap.appendChild(b);
        }
      } else {
        userBadge.style.display = '';
        userBadge.style.background = isUser ? 'rgba(59, 130, 246, 0.15)' : 'rgba(16, 185, 129, 0.15)';
        userBadge.style.color = isUser ? 'var(--accent)' : '#10b981';
        userBadge.textContent = `${this.currentUser} (${isUser ? 'User' : 'Admin'})`;
      }
    } else if (userBadge) {
      userBadge.style.display = 'none';
    }

    // Force redirection if non-admin is viewing a settings panel
    const allowed = ['activities', 'control', 'activity-remote', 'hubs'];
    if (isUser && !allowed.includes(this.currentTab)) {
      this.switchTab('activities', false);
    }
  }

}
