// Sequence Step Editor for Activity Start/Stop sequences
import { h } from '../utils/dom.js';
import { t } from '../i18n/index.js';

export function createSequenceEditor(steps = [], availableDevices = [], commandsMap = {}, onChange) {
  const container = h('div', { className: 'sequence-editor', style: { display: 'flex', flexDirection: 'column', gap: '8px' } });
  let stepList = [...steps];

  function notify() {
    if (onChange) onChange(stepList);
  }

  function render() {
    container.innerHTML = '';

    if (stepList.length === 0) {
      container.appendChild(h('div', { className: 'text-dim', style: { padding: '8px 0', fontSize: '13px' }, text: 'No sequence steps added.' }));
    }

    stepList.forEach((step, idx) => {
      const isDelay = step.type === 'Delay';

      const devSelect = h('select', {
        className: 'form-select',
        style: { flex: 1, padding: '6px 10px', fontSize: '13px' },
        onchange: (e) => {
          step.deviceId = e.target.value;
          // Refresh commands for step
          updateCmds();
          notify();
        },
      });

      availableDevices.forEach(d => {
        const opt = h('option', { value: d.id, text: d.name });
        if (String(step.deviceId) === String(d.id)) opt.selected = true;
        devSelect.appendChild(opt);
      });

      const cmdSelect = h('select', {
        className: 'form-select',
        style: { flex: 1, padding: '6px 10px', fontSize: '13px' },
        onchange: (e) => {
          step.command = e.target.value;
          notify();
        },
      });

      function updateCmds() {
        cmdSelect.innerHTML = '';
        const cmds = commandsMap[step.deviceId || availableDevices[0]?.id] || [];
        cmds.forEach(c => {
          const cName = String(c.name || c);
          const opt = h('option', { value: cName, text: cName });
          if (step.command === cName) opt.selected = true;
          cmdSelect.appendChild(opt);
        });
        if (!step.command && cmds[0]) {
          step.command = String(cmds[0].name || cmds[0]);
        }
      }
      updateCmds();

      const delayInput = h('input', {
        type: 'number',
        className: 'form-input',
        style: { width: '80px', padding: '6px 8px', fontSize: '13px' },
        value: step.delay !== undefined ? step.delay : (isDelay ? 500 : 0),
        placeholder: 'ms',
        oninput: (e) => {
          step.delay = Number(e.target.value) || 0;
          notify();
        },
      });

      const moveUpBtn = h('button', {
        className: 'btn btn-secondary btn-sm',
        text: '↑',
        disabled: idx === 0,
        onclick: () => {
          const temp = stepList[idx - 1];
          stepList[idx - 1] = stepList[idx];
          stepList[idx] = temp;
          render();
          notify();
        },
      });

      const moveDownBtn = h('button', {
        className: 'btn btn-secondary btn-sm',
        text: '↓',
        disabled: idx === stepList.length - 1,
        onclick: () => {
          const temp = stepList[idx + 1];
          stepList[idx + 1] = stepList[idx];
          stepList[idx] = temp;
          render();
          notify();
        },
      });

      const removeBtn = h('button', {
        className: 'btn btn-danger btn-sm',
        text: '×',
        onclick: () => {
          stepList.splice(idx, 1);
          render();
          notify();
        },
      });

      const row = h('div', {
        className: 'card',
        style: { display: 'flex', alignItems: 'center', gap: '8px', padding: '8px 10px', margin: 0 },
      }, [
        h('span', { className: 'badge', text: `${idx + 1}` }),
        isDelay ? h('span', { style: { fontWeight: 'bold', fontSize: '13px' }, text: 'Delay (ms):' }) : devSelect,
        isDelay ? null : cmdSelect,
        delayInput,
        moveUpBtn,
        moveDownBtn,
        removeBtn,
      ]);

      container.appendChild(row);
    });

    const addBar = h('div', { style: { display: 'flex', gap: '8px', marginTop: '4px' } }, [
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: `+ ${t('add_command_step', 'Add Command')}`,
        onclick: () => {
          const firstDev = availableDevices[0];
          const firstCmd = firstDev ? (commandsMap[firstDev.id]?.[0]?.name || '') : '';
          stepList.push({
            type: 'Command',
            deviceId: firstDev ? String(firstDev.id) : '0',
            command: firstCmd,
            delay: 0,
            order: stepList.length,
          });
          render();
          notify();
        },
      }),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: `+ ${t('add_delay_step', 'Add Delay')}`,
        onclick: () => {
          stepList.push({
            type: 'Delay',
            deviceId: '0',
            command: '',
            delay: 500,
            order: stepList.length,
          });
          render();
          notify();
        },
      }),
    ]);

    container.appendChild(addBar);
  }

  render();
  return container;
}

export default createSequenceEditor;
