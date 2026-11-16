// MQTT & Home Assistant Configuration View
import { h, copyText, escapeHtml } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { store } from '../../store.js';

// Home Assistant script (scripts.yaml): a code sample, shown as it is.
const HA_SCRIPT_YAML = `harmony_send_command:
  alias: "Harmony Hub Send Command"
  description: "Send IR/BT command to device via Harmony Hub MQTT"
  mode: queued
  max: 20
  fields:
    device_id:
      description: "Target device ID (unique numeric or string ID)"
      example: "72918237"
      selector:
        text: {}
    device:
      description: "Target device name (display name)"
      example: "Samsung TV"
      selector:
        text: {}
    command:
      description: "Command or list of commands"
      example: "InputHdmi2"
      required: true
    delay_secs:
      description: "Delay between commands in seconds"
      example: 0.4
      default: 0.4
      selector:
        number:
          min: 0
          max: 10
          step: 0.1
          unit_of_measurement: "s"
    hold_secs:
      description: "Hold duration in seconds"
      example: 0
      default: 0
      selector:
        number:
          min: 0
          max: 5
          step: 0.1
          unit_of_measurement: "s"
    num_repeats:
      description: "Times to repeat command"
      example: 1
      default: 1
      selector:
        number:
          min: 1
          max: 20
          step: 1
  sequence:
    - action: mqtt.publish
      data:
        topic: "harmony/hub/send_command"
        payload: >-
          {{ ({
            'command': [command] if command is string else command,
            'delay_secs': delay_secs | default(0.4, true) | float,
            'hold_secs': hold_secs | default(0, true) | float,
            'num_repeats': num_repeats | default(1, true) | int
          } | combine({'device_id': device_id} if device_id is defined and device_id != '' else {'device': device})) | to_json }}
`;

export function createView() {
  const container = h('div', { className: 'setup-subview-mqtt', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  let mqttData = null;

  const card = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('mqtt_config', 'MQTT & Home Assistant') }),
      h('span', { id: 'mqtt-status-badge', className: 'badge', text: t('checking', 'Checking...') }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '13px', marginBottom: '14px' }, text: t('remote_mqtt_help', 'Mirror physical Harmony remote button presses to MQTT for Home Assistant automations.') }),
    h('form', { id: 'mqtt-form', onsubmit: (e) => saveMqtt(e) }, [
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: t('broker_host_ip', 'Broker Host / IP') }),
        h('input', { id: 'mqtt-host', type: 'text', className: 'form-input', placeholder: t('mqtt_host_placeholder', '192.168.1.100 or homeassistant.local') }),
      ]),
      h('div', { style: { display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px' } }, [
        h('div', { className: 'form-group' }, [
          h('label', { className: 'form-label', text: t('port', 'Port') }),
          h('input', { id: 'mqtt-port', type: 'number', className: 'form-input', value: 1883 }),
        ]),
        h('div', { className: 'form-group' }, [
          h('label', { className: 'form-label', text: t('base_topic', 'Base Topic') }),
          h('input', { id: 'mqtt-basetopic', type: 'text', className: 'form-input', value: 'harmony/hub' }),
        ]),
      ]),
      h('div', { style: { display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px' } }, [
        h('div', { className: 'form-group' }, [
          h('label', { className: 'form-label', text: t('username', 'Username') }),
          h('input', { id: 'mqtt-user', type: 'text', className: 'form-input' }),
        ]),
        h('div', { className: 'form-group' }, [
          h('label', { className: 'form-label', text: t('password', 'Password') }),
          h('input', { id: 'mqtt-pass', type: 'password', className: 'form-input', placeholder: t('placeholder_leave_blank', 'Leave blank to keep current') }),
        ]),
      ]),
      h('div', { className: 'card', style: { marginTop: '10px', padding: '12px', background: 'rgba(0,0,0,0.2)' } }, [
        h('div', { className: 'card-title', style: { fontSize: '13px', marginBottom: '8px' }, text: t('ha_mqtt_discovery', 'Home Assistant MQTT discovery') }),
        h('label', { style: { display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer', marginBottom: '6px' } }, [
          h('input', { id: 'mqtt-ha-discovery', type: 'checkbox', checked: true }),
          h('span', { style: { fontSize: '13px' }, text: t('ha_discovery_create_entities', 'Create Home Assistant entities automatically (activity select, sensors, buttons)') }),
        ]),
        h('div', { className: 'form-group' }, [
          h('label', { className: 'form-label', text: t('discovery_prefix_lbl', 'Discovery prefix') }),
          h('input', { id: 'mqtt-discovery-prefix', type: 'text', className: 'form-input', value: 'homeassistant' }),
        ]),
      ]),
      h('div', { className: 'help', style: { marginTop: '10px', fontSize: '12px' },
        text: t('mqtt_v2_help', 'MQTT v2: retained state topics (hub, activity_state, bt, rf, config, inventory) and the cmd channel under the base topic; remote button events on {topic}. The harmony-ng Home Assistant integration does not need the discovery entities.', { topic: '<base>/remote/<slot>/button' }) }),
      h('button', {
        type: 'submit',
        className: 'btn btn-primary',
        style: { marginTop: '14px' },
        text: t('save_settings', 'Save MQTT Configuration'),
      }),
    ]),
  ]);

  // Home Assistant Send Command Script (scripts.yaml) Card
  const scriptCard = h('details', { className: 'card', style: { marginTop: '10px' } }, [
    h('summary', { style: { fontSize: '13px', fontWeight: '700', color: 'var(--accent)', cursor: 'pointer' }, text: t('ha_send_command_script', 'Home Assistant Send Command Script (scripts.yaml)') }),
    h('div', { className: 'text-dim', style: { fontSize: '12px', marginTop: '8px' }, html: t('ha_script_copy_help', 'Copy and paste this script into your Home Assistant {file} to trigger any IR or Bluetooth command with queued concurrency, delays, and hold/pulse time:', { file: '<code>scripts.yaml</code>' }) }),
    h('pre', {
      className: 'mini',
      style: { marginTop: '8px', fontFamily: 'monospace', background: 'var(--bg-dark, rgba(0,0,0,0.3))', padding: '10px', borderRadius: 'var(--radius-sm, 6px)', border: '1px solid var(--border)', overflowX: 'auto', color: 'var(--text-bright, #fff)', fontSize: '11px', lineHeight: '1.45' },
      text: HA_SCRIPT_YAML,
    }),
    h('div', { className: 'text-dim', style: { fontSize: '11px', marginTop: '6px' }, html: t('ha_script_example_help', 'Example HA automation action: {action} with {device_id} (or by name {device}) and {command}.', { action: '<code>action: script.harmony_send_command</code>', device_id: '<code>device_id: "72918237"</code>', device: '<code>device: "Samsung TV"</code>', command: '<code>command: ["PowerToggle"]</code>' }) })
  ]);

  // Home Assistant Automation Action Builder Card
  let haQueue = [];
  let inventory = null;

  const builderCard = h('div', { className: 'card', style: { marginTop: '10px' } }, [
    h('div', { style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '8px' } }, [
      h('h3', { className: 'card-title', style: { margin: 0, fontSize: '15px', color: 'var(--accent)' }, text: t('ha_action_builder', 'Home Assistant Automation Action Builder') }),
      h('span', { className: 'badge', style: { fontSize: '10px' }, text: 'script.harmony_send_command' })
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '12px' }, html: t('ha_action_builder_help', 'Build ready-to-use {script} automation actions for Home Assistant (HA 2024.8+). Select a device, queue commands, adjust timing parameters, and copy the generated YAML.', { script: '<code>script.harmony_send_command</code>' }) }),
    h('div', { style: { display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px' } }, [
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', style: { fontSize: '12px', fontWeight: '600' }, text: t('target_device', 'Target Device') }),
        h('select', { id: 'ha-helper-dev', className: 'form-input form-input-sm', onchange: () => onDeviceChanged() })
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', style: { fontSize: '12px', fontWeight: '600' }, text: t('add_command_step', 'Add Command') }),
        h('div', { style: { display: 'flex', gap: '6px' } }, [
          h('select', { id: 'ha-helper-cmd', className: 'form-input form-input-sm', style: { flex: 1 } }),
          h('button', { type: 'button', className: 'btn btn-primary btn-sm', text: `+ ${t('add', 'Add')}`, onclick: () => addCommand() })
        ])
      ])
    ]),
    h('div', { style: { marginTop: '10px' } }, [
      h('div', { style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '4px' } }, [
        h('label', { style: { fontSize: '11px', fontWeight: '600', textTransform: 'uppercase', color: 'var(--text-dim)' } }, [
          `${t('command_sequence', 'Command Sequence')} (`,
          h('span', { id: 'ha-helper-count', text: '0' }),
          ')'
        ]),
        h('button', { type: 'button', className: 'btn btn-sm btn-secondary', style: { fontSize: '10px', color: 'var(--danger)', padding: '2px 6px' }, text: t('clear_sequence', 'Clear Sequence'), onclick: () => clearQueue() })
      ]),
      h('div', { id: 'ha-helper-queue', style: { minHeight: '48px', maxHeight: '180px', overflowY: 'auto', background: 'var(--bg-dark, rgba(0,0,0,0.2))', border: '1px solid var(--border)', borderRadius: 'var(--radius-sm, 6px)', padding: '6px' } })
    ]),
    h('div', { style: { display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: '8px', marginTop: '10px' } }, [
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', style: { fontSize: '11px' }, text: t('delay_sec', 'Delay (s)') }),
        h('input', { id: 'ha-helper-delay', type: 'number', className: 'form-input form-input-sm', value: 0.4, step: 0.1, min: 0, oninput: () => updateYaml() })
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', style: { fontSize: '11px' }, text: t('hold_s', 'Hold (s)') }),
        h('input', { id: 'ha-helper-hold', type: 'number', className: 'form-input form-input-sm', value: 0, step: 0.1, min: 0, oninput: () => updateYaml() })
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', style: { fontSize: '11px' }, text: t('repeats_lbl', 'Repeats') }),
        h('input', { id: 'ha-helper-repeats', type: 'number', className: 'form-input form-input-sm', value: 1, min: 1, max: 50, oninput: () => updateYaml() })
      ])
    ]),
    h('div', { style: { display: 'flex', flexWrap: 'wrap', gap: '14px', marginTop: '10px', fontSize: '11px' } }, [
      h('label', { style: { display: 'inline-flex', alignItems: 'center', gap: '6px', cursor: 'pointer' } }, [
        h('input', { id: 'ha-helper-indent', type: 'checkbox', checked: true, onchange: () => updateYaml() }),
        h('span', { text: t('indent_automation_sequence', 'Indent for automation sequence (6 spaces)') })
      ]),
      h('label', { style: { display: 'inline-flex', alignItems: 'center', gap: '6px', cursor: 'pointer' } }, [
        h('input', { id: 'ha-helper-use-id', type: 'checkbox', checked: true, onchange: () => updateYaml() }),
        h('span', { text: t('use_device_id_recommended', 'Use Device ID (recommended)') })
      ])
    ]),
    h('div', { style: { marginTop: '12px' } }, [
      h('div', { style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '4px' } }, [
        h('label', { style: { fontSize: '11px', fontWeight: '600', textTransform: 'uppercase', color: 'var(--text-dim)' }, text: t('generated_yaml', 'Generated YAML') }),
        h('div', { style: { display: 'flex', gap: '6px', alignItems: 'center' } }, [
          h('span', { id: 'ha-helper-status', className: 'text-dim', style: { fontSize: '11px' } }),
          h('button', { id: 'btn-ha-test', type: 'button', className: 'btn btn-secondary btn-sm', text: `▶ ${t('test_on_hub', 'Test on Hub')}`, onclick: () => testSequence() }),
          h('button', { id: 'btn-ha-copy', type: 'button', className: 'btn btn-primary btn-sm', text: `📋 ${t('copy_yaml', 'Copy YAML')}`, onclick: () => copyYaml() })
        ])
      ]),
      h('pre', {
        id: 'ha-helper-yaml',
        style: { margin: 0, fontFamily: 'monospace', background: 'var(--bg-dark, rgba(0,0,0,0.3))', padding: '10px', borderRadius: 'var(--radius-sm, 6px)', border: '1px solid var(--border)', overflowX: 'auto', color: 'var(--text-bright, #fff)', fontSize: '11px', lineHeight: '1.45' }
      })
    ])
  ]);

  function getDeviceList() {
    return inventory?.devices || [];
  }

  function getSelectedDevice() {
    const devSelect = container.querySelector('#ha-helper-dev');
    const devId = devSelect?.value;
    return getDeviceList().find(d => String(d.id) === String(devId)) || null;
  }

  async function loadInventory() {
    try {
      inventory = await api.getInventory();
    } catch {
      inventory = { devices: [] };
    }
    const devSelect = container.querySelector('#ha-helper-dev');
    if (!devSelect) return;
    const devs = getDeviceList();
    devSelect.innerHTML = devs.map(d => `<option value="${escapeHtml(d.id)}">${escapeHtml(d.name || t('device_fallback_name', 'Device {id}', { id: d.id }))} (ID: ${escapeHtml(d.id)})</option>`).join('');
    if (devs.length > 0) devSelect.value = String(devs[0].id);
    updateCommandsDropdown();
    renderQueue();
    updateYaml();
  }

  function updateCommandsDropdown() {
    const cmdSelect = container.querySelector('#ha-helper-cmd');
    if (!cmdSelect) return;
    const dev = getSelectedDevice();
    const cmds = dev?.commands || [];
    if (!cmds.length) {
      cmdSelect.innerHTML = `<option value="">${escapeHtml(t('no_commands_available', 'No commands available'))}</option>`;
      return;
    }
    cmdSelect.innerHTML = cmds.map(c => {
      const name = typeof c === 'string' ? c : (c.name || '');
      return `<option value="${escapeHtml(name)}">${escapeHtml(name)}</option>`;
    }).filter(opt => !opt.includes('value=""')).join('');
  }

  function onDeviceChanged() {
    updateCommandsDropdown();
    updateYaml();
  }

  function addCommand() {
    const cmdSelect = container.querySelector('#ha-helper-cmd');
    const cmd = cmdSelect?.value;
    if (cmd) {
      haQueue.push(cmd);
      renderQueue();
      updateYaml();
    }
  }

  function clearQueue() {
    haQueue = [];
    renderQueue();
    updateYaml();
  }

  function renderQueue() {
    const queueEl = container.querySelector('#ha-helper-queue');
    const countEl = container.querySelector('#ha-helper-count');
    if (countEl) countEl.textContent = haQueue.length;
    if (!queueEl) return;
    if (!haQueue.length) {
      queueEl.innerHTML = `<div class="text-dim" style="text-align:center; padding:10px; font-size:12px;">${escapeHtml(t('ha_queue_empty', 'No commands added yet. Pick a command above and click "+ Add".'))}</div>`;
      return;
    }
    queueEl.innerHTML = haQueue.map((cmd, idx) => `
      <div style="display:flex; justify-content:space-between; align-items:center; padding:4px 8px; background:var(--bg-card); border-radius:4px; margin-bottom:4px; border:1px solid var(--border); font-size:12px;">
        <div style="display:flex; align-items:center; gap:8px;">
          <span style="font-family:monospace; color:var(--text-dim); font-size:11px;">#${idx + 1}</span>
          <strong>${escapeHtml(cmd)}</strong>
        </div>
        <div style="display:flex; gap:4px;">
          <button type="button" class="btn btn-sm btn-ghost" data-up="${idx}" ${idx === 0 ? 'disabled style="opacity:0.3"' : ''}>▲</button>
          <button type="button" class="btn btn-sm btn-ghost" data-down="${idx}" ${idx === haQueue.length - 1 ? 'disabled style="opacity:0.3"' : ''}>▼</button>
          <button type="button" class="btn btn-sm btn-ghost" data-del="${idx}" style="color:var(--danger)">✕</button>
        </div>
      </div>
    `).join('');

    queueEl.querySelectorAll('[data-up]').forEach(btn => {
      btn.onclick = () => {
        const i = Number(btn.getAttribute('data-up'));
        if (i > 0) {
          const item = haQueue.splice(i, 1)[0];
          haQueue.splice(i - 1, 0, item);
          renderQueue();
          updateYaml();
        }
      };
    });
    queueEl.querySelectorAll('[data-down]').forEach(btn => {
      btn.onclick = () => {
        const i = Number(btn.getAttribute('data-down'));
        if (i < haQueue.length - 1) {
          const item = haQueue.splice(i, 1)[0];
          haQueue.splice(i + 1, 0, item);
          renderQueue();
          updateYaml();
        }
      };
    });
    queueEl.querySelectorAll('[data-del]').forEach(btn => {
      btn.onclick = () => {
        const i = Number(btn.getAttribute('data-del'));
        haQueue.splice(i, 1);
        renderQueue();
        updateYaml();
      };
    });
  }

  function updateYaml() {
    const yamlEl = container.querySelector('#ha-helper-yaml');
    if (!yamlEl) return;
    const dev = getSelectedDevice();
    const devName = dev?.name || dev?.id || 'Target Device';
    const targetDevId = dev?.id ? String(dev.id) : '';

    const delayVal = parseFloat(container.querySelector('#ha-helper-delay')?.value);
    const holdVal = parseFloat(container.querySelector('#ha-helper-hold')?.value);
    const repeatsVal = parseInt(container.querySelector('#ha-helper-repeats')?.value, 10);
    const useAutomationIndent = container.querySelector('#ha-helper-indent')?.checked;
    const useDeviceId = container.querySelector('#ha-helper-use-id')?.checked !== false;

    const p0 = useAutomationIndent ? '      ' : '';
    const p1 = useAutomationIndent ? '        ' : '  ';
    const p2 = `${p1}  `;
    const p3 = `${p2}  `;

    const lines = [];
    lines.push(`${p0}- action: script.harmony_send_command`);
    lines.push(`${p1}data:`);
    if (useDeviceId && targetDevId) {
      lines.push(`${p2}device_id: "${targetDevId}"  # ${devName}`);
    } else {
      lines.push(`${p2}device: "${devName}"`);
    }

    if (haQueue.length === 0) {
      lines.push(`${p2}command:`);
      lines.push(`${p3}- # Pick a command above and click "+ Add"`);
    } else {
      lines.push(`${p2}command:`);
      haQueue.forEach(cmd => {
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

    yamlEl.textContent = lines.join('\n');
  }

  async function copyYaml() {
    const yamlEl = container.querySelector('#ha-helper-yaml');
    const copyBtn = container.querySelector('#btn-ha-copy');
    const statusEl = container.querySelector('#ha-helper-status');
    if (!yamlEl?.textContent) return;
    try {
      if (!(await copyText(yamlEl.textContent))) throw new Error('clipboard unavailable');
      if (copyBtn) copyBtn.textContent = `✓ ${t('copied_excl', 'Copied!')}`;
      if (statusEl) statusEl.textContent = t('copied_to_clipboard', 'Copied to clipboard');
      setTimeout(() => {
        if (copyBtn) copyBtn.textContent = `📋 ${t('copy_yaml', 'Copy YAML')}`;
        if (statusEl) statusEl.textContent = '';
      }, 2000);
    } catch {
      toast.error(t('copy_clipboard_failed', 'Failed to copy to clipboard'));
    }
  }

  async function testSequence() {
    const dev = getSelectedDevice();
    const testBtn = container.querySelector('#btn-ha-test');
    const statusEl = container.querySelector('#ha-helper-status');
    if (!dev?.id) {
      toast.error(t('select_device_first_short', 'Select a device first'));
      return;
    }
    if (!haQueue.length) {
      toast.error(t('add_commands_first', 'Add commands first'));
      return;
    }
    const delaySecs = parseFloat(container.querySelector('#ha-helper-delay')?.value) || 0.4;
    const repeats = parseInt(container.querySelector('#ha-helper-repeats')?.value, 10) || 1;

    if (testBtn) testBtn.disabled = true;
    if (statusEl) statusEl.textContent = t('running_sequence', 'Running sequence...');
    try {
      for (let r = 0; r < repeats; r++) {
        for (let i = 0; i < haQueue.length; i++) {
          const cmd = haQueue[i];
          if (statusEl) statusEl.textContent = t('sequence_step_status', 'Cmd {n}/{total}: {command}', { n: i + 1, total: haQueue.length, command: cmd });
          await api.sendCommand(dev.id, cmd);
          if (i < haQueue.length - 1 || r < repeats - 1) {
            await new Promise(res => setTimeout(res, Math.max(50, delaySecs * 1000)));
          }
        }
      }
      if (statusEl) statusEl.textContent = t('sequence_complete', 'Sequence complete');
      setTimeout(() => { if (statusEl) statusEl.textContent = ''; }, 2500);
    } catch (err) {
      toast.error(err.message);
      if (statusEl) statusEl.textContent = t('execution_failed', 'Execution failed');
    } finally {
      if (testBtn) testBtn.disabled = false;
    }
  }

  async function loadMqtt() {
    const badge = container.querySelector('#mqtt-status-badge');
    try {
      mqttData = await api.getMqttStatus();
      if (badge) {
        if (mqttData.connected) {
          badge.className = 'badge badge-success';
          badge.textContent = t('connected', 'Connected');
        } else {
          badge.className = 'badge badge-danger';
          badge.textContent = t('disconnected', 'Disconnected');
        }
      }

      const cfg = mqttData.config || {};
      const hostEl = container.querySelector('#mqtt-host');
      const portEl = container.querySelector('#mqtt-port');
      const topicEl = container.querySelector('#mqtt-basetopic');
      const userEl = container.querySelector('#mqtt-user');

      if (hostEl) hostEl.value = cfg.host || mqttData.broker || '';
      if (portEl) portEl.value = cfg.port || 1883;
      if (topicEl) topicEl.value = cfg.baseTopic || 'harmony/hub';
      if (userEl) userEl.value = cfg.username || '';
      const haEl = container.querySelector('#mqtt-ha-discovery');
      const prefixEl = container.querySelector('#mqtt-discovery-prefix');
      if (haEl) haEl.checked = cfg.haDiscovery !== false;
      if (prefixEl) prefixEl.value = cfg.discoveryPrefix || 'homeassistant';
    } catch (e) {
      if (badge) {
        badge.className = 'badge badge-danger';
        badge.textContent = t('error', 'Error');
      }
    }
  }

  async function saveMqtt(e) {
    e.preventDefault();
    const payload = {
      host: container.querySelector('#mqtt-host')?.value.trim() || '',
      port: Number(container.querySelector('#mqtt-port')?.value) || 1883,
      baseTopic: container.querySelector('#mqtt-basetopic')?.value.trim() || 'harmony/hub',
      username: container.querySelector('#mqtt-user')?.value.trim() || '',
      password: container.querySelector('#mqtt-pass')?.value || '',
      haDiscovery: container.querySelector('#mqtt-ha-discovery')?.checked ? 1 : 0,
      discoveryPrefix: container.querySelector('#mqtt-discovery-prefix')?.value.trim() || 'homeassistant',
    };

    try {
      toast.info(t('saving_mqtt_config', 'Saving MQTT config...'));
      await api.saveMqtt(payload);
      toast.success(t('mqtt_config_saved', 'MQTT configuration saved!'));
      loadMqtt();
    } catch (err) {
      toast.error(err.message);
    }
  }

  container.appendChild(card);
  container.appendChild(scriptCard);
  container.appendChild(builderCard);
  loadMqtt();
  loadInventory();
  /* pushed by the Hub on connect, loss and settings changes: update the badge only */
  const off = store.on('mqttStatus', (st) => {
    if (!container.isConnected) { off(); return; }
    const badge = container.querySelector('#mqtt-status-badge');
    if (!badge || !st) return;
    badge.className = st.connected ? 'badge badge-success' : 'badge badge-danger';
    badge.textContent = st.connected ? t('connected', 'Connected') : (st.enabled ? t('disconnected', 'Disconnected') : t('disabled', 'Disabled'));
  });
  return container;
}

export default createView;
