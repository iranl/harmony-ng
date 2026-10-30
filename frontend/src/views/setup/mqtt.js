// MQTT & Home Assistant Configuration View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-mqtt', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  let mqttData = null;

  const card = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('mqtt_config', 'MQTT & Home Assistant') }),
      h('span', { id: 'mqtt-status-badge', className: 'badge', text: 'Checking...' }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '13px', marginBottom: '14px' }, text: t('elite_mqtt_help', 'Mirror physical Harmony remote button presses to MQTT for Home Assistant automations.') }),
    h('form', { id: 'mqtt-form', onsubmit: (e) => saveMqtt(e) }, [
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: 'Broker Host / IP' }),
        h('input', { id: 'mqtt-host', type: 'text', className: 'form-input', placeholder: '192.168.1.100 or homeassistant.local' }),
      ]),
      h('div', { style: { display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px' } }, [
        h('div', { className: 'form-group' }, [
          h('label', { className: 'form-label', text: 'Port' }),
          h('input', { id: 'mqtt-port', type: 'number', className: 'form-input', value: 1883 }),
        ]),
        h('div', { className: 'form-group' }, [
          h('label', { className: 'form-label', text: 'Base Topic' }),
          h('input', { id: 'mqtt-basetopic', type: 'text', className: 'form-input', value: 'harmony/hub' }),
        ]),
      ]),
      h('div', { style: { display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px' } }, [
        h('div', { className: 'form-group' }, [
          h('label', { className: 'form-label', text: 'Username' }),
          h('input', { id: 'mqtt-user', type: 'text', className: 'form-input' }),
        ]),
        h('div', { className: 'form-group' }, [
          h('label', { className: 'form-label', text: 'Password' }),
          h('input', { id: 'mqtt-pass', type: 'password', className: 'form-input' }),
        ]),
      ]),
      h('div', { className: 'card', style: { marginTop: '10px', padding: '12px', background: 'rgba(0,0,0,0.2)' } }, [
        h('div', { className: 'card-title', style: { fontSize: '13px', marginBottom: '8px' }, text: 'Home Assistant & Remotes' }),
        h('label', { style: { display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer', marginBottom: '6px' } }, [
          h('input', { id: 'mqtt-ha-discovery', type: 'checkbox', checked: true }),
          h('span', { style: { fontSize: '13px' }, text: 'Home Assistant Auto-Discovery' }),
        ]),
        h('label', { style: { display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer', marginBottom: '6px' } }, [
          h('input', { id: 'mqtt-slot1-events', type: 'checkbox', checked: true }),
          h('span', { style: { fontSize: '13px' }, text: t('enable_elite_mqtt', 'Mirror Harmony remote button presses to MQTT') }),
        ]),
        h('label', { style: { display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer' } }, [
          h('input', { id: 'mqtt-bt-events', type: 'checkbox' }),
          h('span', { style: { fontSize: '13px' }, text: 'Mirror B25 BT remote button presses to MQTT' }),
        ]),
      ]),
      h('button', {
        type: 'submit',
        className: 'btn btn-primary',
        style: { marginTop: '14px' },
        text: t('save_settings', 'Save MQTT Configuration'),
      }),
    ]),
  ]);

  async function loadMqtt() {
    const badge = container.querySelector('#mqtt-status-badge');
    try {
      mqttData = await api.getMqttStatus();
      if (badge) {
        if (mqttData.connected) {
          badge.className = 'badge badge-success';
          badge.textContent = 'Connected';
        } else {
          badge.className = 'badge badge-danger';
          badge.textContent = 'Disconnected';
        }
      }

      const cfg = mqttData.config || {};
      const hostEl = container.querySelector('#mqtt-host');
      const portEl = container.querySelector('#mqtt-port');
      const topicEl = container.querySelector('#mqtt-basetopic');
      const userEl = container.querySelector('#mqtt-user');
      const haEl = container.querySelector('#mqtt-ha-discovery');
      const slot1El = container.querySelector('#mqtt-slot1-events');
      const btEl = container.querySelector('#mqtt-bt-events');

      if (hostEl) hostEl.value = cfg.host || mqttData.broker || '';
      if (portEl) portEl.value = cfg.port || 1883;
      if (topicEl) topicEl.value = cfg.baseTopic || 'harmony/hub';
      if (userEl) userEl.value = cfg.username || '';
      if (haEl) haEl.checked = cfg.haDiscovery !== false;
      if (slot1El) slot1El.checked = cfg.slot1RemoteEvents !== false;
      if (btEl) btEl.checked = Boolean(cfg.btRemoteEvents);
    } catch (e) {
      if (badge) {
        badge.className = 'badge badge-danger';
        badge.textContent = 'Error';
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
      slot1RemoteEvents: container.querySelector('#mqtt-slot1-events')?.checked ? 1 : 0,
      btRemoteEvents: container.querySelector('#mqtt-bt-events')?.checked ? 1 : 0,
    };

    try {
      toast.info('Saving MQTT config...');
      await api.saveMqtt(payload);
      toast.success('MQTT configuration saved!');
      loadMqtt();
    } catch (err) {
      toast.error(err.message);
    }
  }

  container.appendChild(card);
  loadMqtt();
  return container;
}

export default createView;
