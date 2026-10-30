// Reusable Progress Banner for Activity Transitions, OTA, and System Updates
import { h } from '../utils/dom.js';

export function createProgressBanner({ title = '', message = '', progress = null, onCancel = null }) {
  const container = h('div', { className: 'transition-banner' }, [
    h('div', { style: { display: 'flex', flexDirection: 'column', gap: '2px' } }, [
      h('div', { style: { fontWeight: 'bold', fontSize: '13px' }, text: title }),
      message ? h('div', { style: { fontSize: '11px', opacity: '0.9' }, text: message }) : null,
      progress !== null ? h('div', {
        style: {
          height: '4px',
          width: '120px',
          background: 'rgba(255,255,255,0.3)',
          borderRadius: '2px',
          overflow: 'hidden',
          marginTop: '4px',
        },
      }, [
        h('div', {
          style: {
            height: '100%',
            width: `${Math.min(100, Math.max(0, progress))}%`,
            background: '#ffffff',
            transition: 'width 0.2s ease',
          },
        }),
      ]) : null,
    ]),
    onCancel ? h('button', {
      className: 'btn btn-danger btn-sm',
      text: 'Cancel',
      onclick: onCancel,
    }) : null,
  ]);

  return container;
}

export default createProgressBanner;
