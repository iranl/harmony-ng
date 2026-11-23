// Pairing of a plugin device from its config fields (the manifest's "pairing" block).
//  - listen: the plugin hears its device itself; the Hub answers at once and the status is polled
//    until the plugin reports what it found. Those values fill the fields.
//  - confirm: the user accepts a prompt on the device (a TV) at the "host" field's address;
//    polled the same way.
//  - pin_display: the device shows a code; the host comes from the "host" field.
// The user saves the device afterwards, as for any field.
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';

async function ask(id, action, host = '', pin = '') {
  try {
    return await api.pairPlugin(id, action, host, pin);
  } catch (e) {
    return { ok: false, stage: 'error', message: e.message };
  }
}

function failed(res) {
  return res?.ok === false || res?.stage === 'error' || Boolean(res?.error);
}

function hostOf(inputs, show) {
  const host = inputs.host?.value.trim() || '';
  if (!host) show(t('plg_pair_host_required', 'Please enter device IP address or host'));
  return host;
}

// listen and confirm: start, then poll "status" until done, error or the timeout
async function runPolled(plug, inputs, show, confirm) {
  const host = confirm ? hostOf(inputs, show) : '';
  if (confirm && !host) return;
  let res = await ask(plug.id, 'start', host);
  const secs = Number(res?.timeout_s) || Number(plug.manifest?.pairing?.timeout_s) || 30;
  const end = Date.now() + (secs + 2) * 1000;
  for (;;) {
    if (failed(res)) {
      const error = res?.message || res?.error || '';
      show(confirm ? t('plg_pairing_failed', 'Pairing failed: {error}', { error })
        : t('plg_listen_failed', 'Nothing found. {error}', { error }));
      return;
    }
    if (res?.stage === 'done') {
      Object.entries(res.config || {}).forEach(([key, val]) => {
        if (inputs[key]) inputs[key].value = String(val);
      });
      show(res.message || (confirm ? t('plg_paired_toast', 'Device paired successfully!')
        : t('plg_listen_done', 'Found. The fields are filled in: save the device.')));
      return;
    }
    const left = Math.round((end - Date.now()) / 1000);
    if (left <= 0) {
      show(confirm ? t('plg_pairing_failed', 'Pairing failed: {error}', { error: t('plg_confirm_timeout', 'the device was not accepted in time.') })
        : t('plg_listen_failed', 'Nothing found. {error}', { error: '' }));
      return;
    }
    show(res?.message ? `${res.message} (${left} s)`
      : (confirm ? t('plg_confirm_running', 'Accept the request on the device: {s} s left', { s: left })
        : t('plg_listen_running', 'Listening for the device: {s} s left', { s: left })));
    await new Promise(r => setTimeout(r, 1000));
    res = await ask(plug.id, 'status');
  }
}

async function runPin(plug, inputs, show, area) {
  const host = hostOf(inputs, show);
  if (!host) return;
  show(t('plg_connecting', 'Connecting...'));
  const res = await ask(plug.id, 'start', host);
  if (failed(res)) {
    show(t('plg_pairing_failed', 'Pairing failed: {error}', { error: res?.message || res?.error || '' }));
    return;
  }
  const pinInput = h('input', { type: 'text', className: 'form-input', maxLength: Number(plug.manifest?.pairing?.pin_length) || 6 });
  const okBtn = h('button', {
    className: 'btn btn-primary btn-sm', text: t('plg_verify_code', 'Verify Code'),
    onclick: async () => {
      okBtn.disabled = true;
      const r = await ask(plug.id, 'pin', host, pinInput.value.trim().toUpperCase());
      okBtn.disabled = false;
      if (failed(r)) {
        show(t('plg_pairing_failed', 'Pairing failed: {error}', { error: r?.message || r?.error || '' }));
        return;
      }
      area.replaceChildren();
      show(r?.message || t('plg_paired_toast', 'Device paired successfully!'));
    },
  });
  show(res?.message || t('plg_pair_prompt', 'Enter the code shown on your device:'));
  area.replaceChildren(pinInput, okBtn);
}

// A row with the pair button and its status, or null when the plugin has no pairing.
// inputs: { key: <input> } of the device's config fields.
export function pluginPairingRow(plug, inputs) {
  const pairing = plug?.manifest?.pairing;
  if (!pairing?.supported) return null;
  const status = h('span', { className: 'text-dim', style: { fontSize: '12px' }, text: pairing.description || pairing.instructions || '' });
  const area = h('div', { style: { display: 'flex', gap: '6px' } });
  const show = (txt) => { status.textContent = txt; };
  const btn = h('button', {
    type: 'button', className: 'btn btn-secondary btn-sm',
    text: pairing.button_label || t('plg_pair_device', 'Pair Device'),
    onclick: async () => {
      btn.disabled = true;
      try {
        if (pairing.type === 'listen' || pairing.type === 'confirm') {
          await runPolled(plug, inputs, show, pairing.type === 'confirm');
        } else {
          await runPin(plug, inputs, show, area);
        }
      } finally {
        btn.disabled = false;
      }
    },
  });
  return h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px', marginTop: '8px' } }, [
    h('label', { className: 'form-label', text: pairing.title || t('plg_device_pairing', 'Device Pairing') }),
    h('div', { style: { display: 'flex', alignItems: 'center', gap: '8px', flexWrap: 'wrap' } }, [btn, status]),
    area,
  ]);
}
