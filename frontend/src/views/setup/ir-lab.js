// IR Lab Bulk Command Testing View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { store } from '../../store.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-irlab', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  const devices = store.get('devices') || [];
  let currentRunId = null;

  const targetSelect = h('select', { className: 'form-select' },
    devices.map(d => h('option', { value: d.id, text: d.name }))
  );

  const delayInput = h('input', { type: 'number', className: 'form-input', value: 800 });

  const commandsListArea = h('textarea', {
    className: 'form-textarea',
    rows: 6,
    placeholder: 'Paste command names or hex codes (one per line)...',
  });

  const card = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('ir_lab', 'IR Lab Bulk Command Tester') }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '13px', marginBottom: '12px' }, text: 'Send multiple commands sequentially to locate discreet codes (power, inputs, aspect ratio).' }),
    h('div', { className: 'form-group' }, [
      h('label', { className: 'form-label', text: 'Target Device' }),
      targetSelect,
    ]),
    h('div', { className: 'form-group' }, [
      h('label', { className: 'form-label', text: 'Delay Between Signals (ms)' }),
      delayInput,
    ]),
    h('div', { className: 'form-group' }, [
      h('label', { className: 'form-label', text: 'Commands Sequence (One per line)' }),
      commandsListArea,
    ]),
    h('div', { style: { display: 'flex', gap: '10px', marginTop: '12px' } }, [
      h('button', {
        className: 'btn btn-primary',
        text: '▶ Run Batch Send',
        onclick: async () => {
          const lines = commandsListArea.value.split('\n').map(l => l.trim()).filter(Boolean);
          if (lines.length === 0) return toast.warn('Enter commands to test');
          currentRunId = `run_${Date.now()}`;
          try {
            toast.info(`Starting batch run of ${lines.length} commands...`);
            await api.sendBatchIr({
              deviceId: targetSelect.value,
              commands: lines,
              delayMs: Number(delayInput.value) || 800,
              runId: currentRunId,
            });
            toast.success('Batch execution finished');
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
      h('button', {
        className: 'btn btn-danger',
        text: '⏹ Abort Run',
        onclick: async () => {
          if (!currentRunId) return;
          try {
            await api.cancelBatchIr(currentRunId);
            toast.warn('Batch run aborted');
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
    ]),
  ]);

  container.appendChild(card);
  return container;
}

export default createView;
