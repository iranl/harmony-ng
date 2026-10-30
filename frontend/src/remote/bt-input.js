// Virtual Bluetooth Keyboard and Mouse Trackpad module
import { h } from '../utils/dom.js';
import { t } from '../i18n/index.js';
import { api } from '../api/index.js';
import { toast } from '../components/toast.js';

export function createBtInputView(targetDevice, options = {}) {
  const container = h('div', { className: 'bt-input-container' });
  let activeSubTab = 'keyboard'; // 'keyboard' | 'mouse'

  const devName = targetDevice?.name || 'Bluetooth Device';
  const targetBdaddr = targetDevice?.bdaddr || targetDevice?.btHost || '';

  const header = h('div', { className: 'bt-header card-header' }, [
    h('div', { className: 'bt-device-badge badge badge-accent' }, [
      h('span', { text: `Target: ${devName}` }),
    ]),
    h('div', { style: { display: 'flex', gap: '8px', alignItems: 'center' } }, [
      h('button', {
        className: 'btn btn-sm btn-secondary',
        text: t('keyboard_tab', 'Keyboard'),
        onclick: () => switchTab('keyboard'),
      }),
      h('button', {
        className: 'btn btn-sm btn-secondary',
        text: t('mouse_tab', 'Mouse'),
        onclick: () => switchTab('mouse'),
      }),
      h('button', {
        className: 'btn btn-sm btn-danger',
        text: t('release_all', 'Release All'),
        onclick: async () => {
          api.sendWs({ action: 'bt_key_release_all', target: targetBdaddr });
          toast.info(t('keys_released', 'Released all held keys'));
        },
      }),
    ]),
  ]);

  const contentArea = h('div', { className: 'bt-content-area' });

  function switchTab(tab) {
    activeSubTab = tab;
    renderContent();
  }

  function renderKeyboard() {
    contentArea.innerHTML = '';

    // Text bar
    const textInput = h('input', {
      type: 'text',
      className: 'form-input',
      placeholder: t('placeholder_kb_text', 'Type text to send to device...'),
    });

    const sendBtn = h('button', {
      className: 'btn btn-primary',
      text: t('send', 'Send'),
      onclick: async (e) => {
        e.preventDefault();
        const txt = textInput.value;
        if (!txt) return;
        await api.sendBtText(txt, targetBdaddr);
        textInput.value = '';
        toast.success(t('text_sent', 'Text sent'));
      },
    });

    const textForm = h('div', { className: 'card', style: { display: 'flex', gap: '8px' } }, [
      textInput,
      sendBtn,
    ]);

    // D-Pad and Navigation
    const navCard = h('div', { className: 'card', style: { textAlign: 'center' } }, [
      h('div', { className: 'card-title', text: 'Navigation' }),
      h('div', { style: { display: 'grid', gridTemplateColumns: 'repeat(3, 60px)', gap: '8px', justifyContent: 'center', margin: '12px auto' } }, [
        h('div'),
        h('button', { className: 'btn btn-secondary', text: '▲', onclick: () => api.sendBtKey('up', 'tap', targetBdaddr) }),
        h('div'),
        h('button', { className: 'btn btn-secondary', text: '◀', onclick: () => api.sendBtKey('left', 'tap', targetBdaddr) }),
        h('button', { className: 'btn btn-primary', text: 'OK', onclick: () => api.sendBtKey('enter', 'tap', targetBdaddr) }),
        h('button', { className: 'btn btn-secondary', text: '▶', onclick: () => api.sendBtKey('right', 'tap', targetBdaddr) }),
        h('div'),
        h('button', { className: 'btn btn-secondary', text: '▼', onclick: () => api.sendBtKey('down', 'tap', targetBdaddr) }),
        h('div'),
      ]),
      h('div', { style: { display: 'flex', justifyContent: 'center', gap: '8px', marginTop: '10px' } }, [
        h('button', { className: 'btn btn-sm btn-secondary', text: 'Back', onclick: () => api.sendBtKey('escape', 'tap', targetBdaddr) }),
        h('button', { className: 'btn btn-sm btn-secondary', text: 'Home', onclick: () => api.sendBtKey('home', 'tap', targetBdaddr) }),
        h('button', { className: 'btn btn-sm btn-secondary', text: 'Menu', onclick: () => api.sendBtKey('menu', 'tap', targetBdaddr) }),
      ]),
    ]);

    // Virtual keyboard rows
    const keys = [
      ['Esc', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '⌫'],
      ['Tab', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'],
      ['Caps', 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'Enter'],
      ['Shift', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', 'Space'],
    ];

    const kbRows = keys.map(row => {
      return h('div', { className: 'kb-row', style: { display: 'flex', gap: '4px', justifyContent: 'center', marginBottom: '6px' } },
        row.map(k => {
          let code = k.toLowerCase();
          if (k === '⌫') code = 'backspace';
          if (k === 'Space') code = 'space';
          if (k === 'Esc') code = 'escape';
          return h('button', {
            className: 'btn btn-secondary kb-key',
            style: { padding: '8px 10px', minWidth: k === 'Space' ? '120px' : (k.length > 1 ? '48px' : '32px'), fontSize: '13px' },
            text: k,
            onclick: () => api.sendBtKey(code, 'tap', targetBdaddr),
          });
        })
      );
    });

    const kbCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-title', text: 'Keyboard', style: { marginBottom: '10px' } }),
      ...kbRows,
    ]);

    contentArea.appendChild(textForm);
    contentArea.appendChild(navCard);
    contentArea.appendChild(kbCard);
  }

  function renderMouse() {
    contentArea.innerHTML = '';

    const pad = h('div', {
      className: 'mouse-trackpad card',
      style: {
        height: '240px',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        background: 'rgba(0,0,0,0.4)',
        border: '2px dashed var(--border)',
        borderRadius: 'var(--radius-md)',
        cursor: 'crosshair',
        touchAction: 'none',
      },
    }, [
      h('span', { className: 'text-dim', text: 'Trackpad (Drag to move mouse, tap to click)' }),
    ]);

    let lastX = 0;
    let lastY = 0;
    let isTracking = false;

    pad.addEventListener('pointerdown', (e) => {
      isTracking = true;
      lastX = e.clientX;
      lastY = e.clientY;
      pad.setPointerCapture(e.pointerId);
    });

    pad.addEventListener('pointermove', (e) => {
      if (!isTracking) return;
      const dx = Math.round((e.clientX - lastX) * 1.5);
      const dy = Math.round((e.clientY - lastY) * 1.5);
      lastX = e.clientX;
      lastY = e.clientY;

      if (dx !== 0 || dy !== 0) {
        api.sendBtMouse({ dx, dy, target: targetBdaddr });
      }
    });

    pad.addEventListener('pointerup', (e) => {
      isTracking = false;
      try { pad.releasePointerCapture(e.pointerId); } catch {}
    });

    const clickBar = h('div', { style: { display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px', marginTop: '12px' } }, [
      h('button', {
        className: 'btn btn-secondary',
        style: { height: '54px' },
        text: 'Left Click',
        onclick: () => api.sendBtMouse({ action: 'click', button: 'left', target: targetBdaddr }),
      }),
      h('button', {
        className: 'btn btn-secondary',
        style: { height: '54px' },
        text: 'Right Click',
        onclick: () => api.sendBtMouse({ action: 'click', button: 'right', target: targetBdaddr }),
      }),
    ]);

    contentArea.appendChild(pad);
    contentArea.appendChild(clickBar);
  }

  function renderContent() {
    if (activeSubTab === 'keyboard') {
      renderKeyboard();
    } else {
      renderMouse();
    }
  }

  container.appendChild(header);
  container.appendChild(contentArea);
  renderContent();
  return container;
}

export default createBtInputView;
