// IR Lab Bulk Command Testing View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { store } from '../../store.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-irlab', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  const devices = store.get('devices') || [];
  let run = null; // the batch run in progress: { aborted }

  const targetSelect = h('select', { className: 'form-select' },
    devices.map(d => h('option', { value: d.id, text: d.name }))
  );

  const delayInput = h('input', { type: 'number', className: 'form-input', value: 800, min: 0, max: 10000 });

  const commandsListArea = h('textarea', {
    className: 'form-textarea',
    rows: 6,
    placeholder: t('irlab_commands_placeholder', "Paste the device's command names (one per line)..."),
  });

  const card = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('ir_lab', 'IR Lab Bulk Command Tester') }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '13px', marginBottom: '12px' }, text: t('irlab_desc', 'Send multiple commands sequentially to locate discreet codes (power, inputs, aspect ratio).') }),
    h('div', { className: 'form-group' }, [
      h('label', { className: 'form-label', text: t('target_device', 'Target Device') }),
      targetSelect,
    ]),
    h('div', { className: 'form-group' }, [
      h('label', { className: 'form-label', text: t('irlab_delay_label', 'Delay Between Signals (ms)') }),
      delayInput,
    ]),
    h('div', { className: 'form-group' }, [
      h('label', { className: 'form-label', text: t('irlab_commands_label', 'Commands Sequence (One per line)') }),
      commandsListArea,
    ]),
    h('div', { style: { display: 'flex', gap: '10px', marginTop: '12px' } }, [
      h('button', {
        className: 'btn btn-primary',
        text: `▶ ${t('irlab_run', 'Run Batch Send')}`,
        onclick: async () => {
          const lines = commandsListArea.value.split('\n').map(l => l.trim()).filter(Boolean);
          if (lines.length === 0) return toast.warn(t('irlab_enter_commands', 'Enter commands to test'));
          if (run) return toast.warn(t('irlab_already_running', 'A batch run is already going'));
          // The Hub has no batch route: send one command at a time from here, like the legacy IR Lab
          const typed = delayInput.value === '' ? NaN : Number(delayInput.value);
          const delayMs = Number.isFinite(typed) ? Math.min(Math.max(typed, 0), 10000) : 800;
          const deviceId = targetSelect.value;
          const thisRun = (run = { aborted: false });
          let failed = 0;
          toast.info(t('irlab_starting', 'Starting batch run of {count} commands...', { count: lines.length }));
          try {
            for (let i = 0; i < lines.length && !thisRun.aborted; i++) {
              try {
                await api.sendCommand(deviceId, lines[i]);
              } catch (e) {
                failed++;
                toast.error(`${lines[i]}: ${e?.message || e}`);
              }
              if (i < lines.length - 1 && !thisRun.aborted) await new Promise(r => setTimeout(r, delayMs));
            }
          } finally {
            run = null; // also after an error here, or the next run would be refused for ever
          }
          if (thisRun.aborted) return;
          if (failed) toast.warn(t('irlab_finished_failed', 'Batch finished, {failed} of {count} failed', { failed, count: lines.length }));
          else toast.success(t('irlab_finished', 'Batch execution finished'));
        },
      }),
      h('button', {
        className: 'btn btn-danger',
        text: `⏹ ${t('irlab_abort', 'Abort Run')}`,
        onclick: () => {
          if (!run) return;
          run.aborted = true;
          toast.warn(t('irlab_aborted', 'Batch run aborted'));
        },
      }),
    ]),
  ]);

  container.appendChild(card);
  return container;
}

export default createView;
