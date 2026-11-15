// Dialogs for two refusals and questions of the Hub:
// - the accounts and access tokens made while authentication was off, listed by the Hub in the
//   reply that turns authentication on ("unconfirmed"): keep or delete each one
// - a device delete refused because activities, devices or remote keys still use it (409 "users")
import { t } from '../i18n.js';

function el(tag, attrs = {}, children = []) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'text') n.textContent = v;
    else if (k === 'style') n.setAttribute('style', v);
    else n.setAttribute(k, v);
  }
  for (const c of children) if (c) n.appendChild(c);
  return n;
}

// The Hub's "unconfirmed" object as a flat list of {kind, id, label}, without the account logged in
export function unconfirmedItems(unconfirmed, currentUser = '') {
  const items = [];
  for (const u of unconfirmed?.users || []) {
    if (currentUser && u.username === currentUser) continue;
    items.push({ kind: 'user', id: u.username, label: `${t('auth_unconfirmed_account', 'Account')}: ${u.username} (${u.role})` });
  }
  for (const tk of unconfirmed?.tokens || []) {
    const owner = tk.owner ? `, ${tk.owner}` : '';
    items.push({ kind: 'token', id: tk.id, label: `${t('auth_unconfirmed_token', 'Access token')}: ${tk.name || tk.id} (${tk.role}${owner})` });
  }
  return items;
}

// Asks to keep or delete each listed account and token. Keep = POST /api/auth/confirm, delete =
// the usual routes (a token already gone with its account, 404, counts as deleted). Closing the
// dialog decides nothing: they stay listed as not confirmed (Users page).
export function askUnconfirmed(client, unconfirmed, currentUser = '') {
  const items = unconfirmedItems(unconfirmed, currentUser);
  if (!items.length || typeof document === 'undefined') return Promise.resolve();
  return new Promise((resolve) => {
    const status = el('p', { style: 'font-size: 12px; margin: 8px 0 0; color: var(--danger); min-height: 1em;' });
    const list = el('div', { style: 'display: flex; flex-direction: column; gap: 6px; margin-top: 10px;' });
    const backdrop = el('div', { class: 'modal-sheet-backdrop', style: 'display: flex; z-index: 1000004;' });
    const close = () => {
      backdrop.remove();
      resolve();
    };
    const act = async (item, keep) => {
      status.textContent = '';
      try {
        if (keep) {
          await client.confirmAuthItems(item.kind === 'user' ? [item.id] : [], item.kind === 'token' ? [item.id] : []);
        } else {
          try {
            if (item.kind === 'user') await client.deleteUser(item.id);
            else await client.deleteToken(item.id);
          } catch (err) {
            if (err.status !== 404) throw err;
          }
        }
        item.row.remove();
        if (!list.children.length) close();
      } catch (err) {
        status.textContent = `${item.label}: ${err.message}`;
      }
    };
    for (const item of items) {
      const keepBtn = el('button', { type: 'button', class: 'btn btn-xs btn-secondary', text: t('auth_keep', 'Keep') });
      const delBtn = el('button', { type: 'button', class: 'btn btn-xs btn-danger', text: t('delete', 'Delete') });
      item.row = el('div', { style: 'display: flex; align-items: center; gap: 8px;' }, [
        el('span', { style: 'flex: 1; font-size: 13px; word-break: break-word;', text: item.label }), keepBtn, delBtn
      ]);
      keepBtn.addEventListener('click', () => act(item, true));
      delBtn.addEventListener('click', () => act(item, false));
      list.appendChild(item.row);
    }
    const keepAll = el('button', { type: 'button', class: 'btn btn-sm btn-secondary', text: t('auth_keep_all', 'Keep all') });
    const deleteAll = el('button', { type: 'button', class: 'btn btn-sm btn-danger', text: t('auth_delete_all', 'Delete all') });
    const later = el('button', { type: 'button', class: 'btn btn-sm btn-ghost', text: t('auth_decide_later', 'Decide later') });
    keepAll.addEventListener('click', async () => {
      for (const item of items.filter((i) => i.row.isConnected)) await act(item, true);
    });
    deleteAll.addEventListener('click', async () => {
      for (const item of items.filter((i) => i.row.isConnected)) await act(item, false);
    });
    later.addEventListener('click', close);
    backdrop.appendChild(el('div', { class: 'modal-sheet-dialog', style: 'max-width: 480px;' }, [
      el('div', { class: 'modal-sheet-header' }, [
        el('h3', { style: 'font-size: 16px; margin: 0;', text: t('auth_unconfirmed_title', 'Made while authentication was off') })
      ]),
      el('div', { class: 'modal-sheet-body', style: 'padding-top: 10px; padding-bottom: 12px;' }, [
        el('p', { style: 'font-size: 13px; line-height: 1.5; margin: 0;', text: t('auth_unconfirmed_intro', 'These accounts and access tokens were made while authentication was off, when anybody on the network could make them. Keep the ones you know and delete the others.') }),
        list,
        status
      ]),
      el('div', { class: 'modal-sheet-footer', style: 'display: flex; justify-content: flex-end; gap: 8px; flex-wrap: wrap;' }, [later, deleteAll, keepAll])
    ]));
    document.body.appendChild(backdrop);
  });
}

// Text for a device delete refused with 409: what still uses the device
export function deviceUsersText(users) {
  const lines = [];
  for (const u of users || []) {
    if (u.type === 'activity') {
      lines.push(t('device_in_use_activity', 'Activity {name} ({id}): {fields}')
        .replace('{name}', u.name || '').replace('{id}', u.id).replace('{fields}', (u.fields || []).join(', ')));
    } else if (u.type === 'device') {
      lines.push(t('device_in_use_device', 'Device {name} ({id}): Bluetooth trigger')
        .replace('{name}', u.name || '').replace('{id}', u.id));
    } else if (u.type === 'remote_map' || u.type === 'bt_remote_map') {
      const keys = (u.keys || []).join(', ') + ((u.count || 0) > (u.keys || []).length ? ', ...' : '');
      const tmpl = u.type === 'remote_map'
        ? t('device_in_use_map', 'Remote slot {slot}: {count} keys ({keys})')
        : t('device_in_use_bt_map', 'Bluetooth remote: {count} keys ({keys})');
      lines.push(tmpl.replace('{slot}', u.slot).replace('{count}', u.count).replace('{keys}', keys));
    }
  }
  lines.push('');
  lines.push(t('device_in_use_hint', 'Remove the device from these first, then delete it.'));
  return lines.join('\n');
}

// The users list of a refused device delete, or null for any other error
export function deviceUsersOf(err) {
  return err && err.status === 409 && Array.isArray(err.data?.users) ? err.data.users : null;
}
