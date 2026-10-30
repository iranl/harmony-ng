// Modal confirm dialog component for Harmony Remote v3
import { h } from '../utils/dom.js';
import { t } from '../i18n/index.js';

export function confirmModal(options = {}) {
  const {
    title = t('confirm_title', 'Confirm Action'),
    message = '',
    confirmText = t('confirm_btn', 'Confirm'),
    cancelText = t('cancel_btn', 'Cancel'),
    danger = false,
  } = typeof options === 'string' ? { message: options } : options;

  return new Promise((resolve) => {
    let backdrop = null;

    const cleanup = () => {
      if (backdrop && backdrop.parentNode) {
        backdrop.classList.remove('modal-visible');
        setTimeout(() => {
          if (backdrop && backdrop.parentNode) {
            backdrop.parentNode.removeChild(backdrop);
          }
        }, 200);
      }
    };

    const handleConfirm = () => {
      cleanup();
      resolve(true);
    };

    const handleCancel = () => {
      cleanup();
      resolve(false);
    };

    backdrop = h('div', { className: 'modal-backdrop' }, [
      h('div', { className: 'modal-dialog' }, [
        h('div', { className: 'modal-header' }, [
          h('h3', { className: 'modal-title', text: title }),
          h('button', { className: 'modal-close', text: '×', onclick: handleCancel }),
        ]),
        h('div', { className: 'modal-body' }, [
          h('p', { className: 'modal-message', text: message }),
        ]),
        h('div', { className: 'modal-actions' }, [
          h('button', {
            className: 'btn btn-secondary',
            text: cancelText,
            onclick: handleCancel,
          }),
          h('button', {
            className: `btn ${danger ? 'btn-danger' : 'btn-primary'}`,
            text: confirmText,
            onclick: handleConfirm,
          }),
        ]),
      ]),
    ]);

    document.body.appendChild(backdrop);
    requestAnimationFrame(() => {
      backdrop.classList.add('modal-visible');
    });
  });
}

export const confirm = { ask: confirmModal };
export default confirmModal;
