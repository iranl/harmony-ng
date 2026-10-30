// Reusable Modal Component
import { h } from '../utils/dom.js';

export function createModal({ title = '', content, actions = [] }) {
  let backdrop = null;

  const close = () => {
    if (backdrop && backdrop.parentNode) {
      backdrop.classList.remove('modal-visible');
      setTimeout(() => backdrop?.remove(), 200);
    }
  };

  const dialogActions = actions.map(act => {
    return h('button', {
      className: `btn ${act.variant ? `btn-${act.variant}` : 'btn-secondary'}`,
      text: act.label,
      onclick: async () => {
        if (act.onClick) {
          const res = await act.onClick(close);
          if (res !== false) close();
        } else {
          close();
        }
      },
    });
  });

  backdrop = h('div', { className: 'modal-backdrop' }, [
    h('div', { className: 'modal-dialog' }, [
      h('div', { className: 'modal-header' }, [
        h('h3', { className: 'modal-title', text: title }),
        h('button', { className: 'modal-close', text: '×', onclick: close }),
      ]),
      h('div', { className: 'modal-body' }, [content]),
      h('div', { className: 'modal-actions' }, dialogActions),
    ]),
  ]);

  document.body.appendChild(backdrop);
  requestAnimationFrame(() => backdrop.classList.add('modal-visible'));
  return { close, backdrop };
}

export default createModal;
