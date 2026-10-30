// BT Virtual Keyboard & Mouse Tool View for Setup
import { h } from '../../utils/dom.js';
import { store } from '../../store.js';
import { createBtInputView } from '../../remote/bt-input.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-btkeyboard', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  const devices = store.get('devices') || [];
  const btDev = devices.find(d => d.type === 'BT' || d.isBt) || devices[0];

  if (!btDev) {
    container.appendChild(h('div', { className: 'card text-dim', text: 'No Bluetooth devices paired. Pair a Bluetooth host under Bluetooth Hosts first.' }));
    return container;
  }

  const btView = createBtInputView(btDev);
  container.appendChild(btView);
  return container;
}

export default createView;
