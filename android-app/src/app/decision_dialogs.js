// Dialogs for two answers of the Hub:
// - the accounts and access tokens to check, listed in the reply that turns authentication on
//   ("to_check"): keep them all, or delete the ones not recognised
// - a device delete refused because activities, devices or remote keys still use it (409 "users")
import { t, tf } from '../i18n.js';

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

const TOKENS_UNREADABLE_TEXT = 'The Hub cannot read its access token file, so its tokens are not listed. An admin can move the file aside with POST /api/auth/tokens-reset; every token then has to be created again.';

// The Hub's "to_check" object (the reply that turned authentication on) as a flat list of
// {kind, id, label}, without the account logged in
export function checkItems(toCheck, currentUser = '') {
  const items = [];
  for (const u of toCheck?.users || []) {
    if (currentUser && u.username === currentUser) continue;
    items.push({ kind: 'user', id: u.username, label: `${t('auth_check_account', 'Account')}: ${u.username} (${u.role})` });
  }
  for (const tk of toCheck?.tokens || []) {
    const owner = (tk.owner ? `, ${tk.owner}` : '') + (tk.owner_missing ? `, ${t('auth_check_owner_gone', 'account gone')}` : '');
    items.push({ kind: 'token', id: tk.id, label: `${t('auth_check_token', 'Access token')}: ${tk.name || tk.id} (${tk.role}${owner})` });
  }
  return items;
}

// Shows the accounts and tokens to check after authentication was turned on: Delete per item (the
// usual routes; one already gone, e.g. a token deleted with its account, 404, counts as deleted),
// Keep all closes. Nothing else changes: whatever is not deleted keeps working.
export function askToCheck(client, toCheck, currentUser = '') {
  const items = checkItems(toCheck, currentUser);
  // the Hub could not read its token file: its tokens are not in the list, say so
  const tokensUnreadable = Boolean(toCheck && toCheck.tokens_unreadable);
  if ((!items.length && !tokensUnreadable) || typeof document === 'undefined') return Promise.resolve();
  return new Promise((resolve) => {
    const status = el('p', { style: 'font-size: 12px; margin: 8px 0 0; color: var(--danger); min-height: 1em;' });
    const list = el('div', { style: 'display: flex; flex-direction: column; gap: 6px; margin-top: 10px;' });
    const backdrop = el('div', { class: 'modal-sheet-backdrop', style: 'display: flex; z-index: 1000004;' });
    const close = () => {
      backdrop.remove();
      resolve();
    };
    const remove = async (item) => {
      status.textContent = '';
      try {
        try {
          if (item.kind === 'user') await client.deleteUser(item.id);
          else await client.deleteToken(item.id);
        } catch (err) {
          if (err.status !== 404) throw err;
        }
        item.row.remove();
        if (!list.children.length) close();
      } catch (err) {
        status.textContent = `${item.label}: ${err.message}`;
      }
    };
    for (const item of items) {
      const delBtn = el('button', { type: 'button', class: 'btn btn-xs btn-danger', text: t('delete', 'Delete') });
      item.row = el('div', { style: 'display: flex; align-items: center; gap: 8px;' }, [
        el('span', { style: 'flex: 1; font-size: 13px; word-break: break-word;', text: item.label }), delBtn
      ]);
      delBtn.addEventListener('click', () => remove(item));
      list.appendChild(item.row);
    }
    const keepAll = el('button', { type: 'button', class: 'btn btn-sm btn-primary', text: t('auth_keep_all', 'Keep all') });
    keepAll.addEventListener('click', close);
    backdrop.appendChild(el('div', { class: 'modal-sheet-dialog', style: 'max-width: 480px;' }, [
      el('div', { class: 'modal-sheet-header' }, [
        el('h3', { style: 'font-size: 16px; margin: 0;', text: t('auth_check_title', 'Check accounts and access tokens') })
      ]),
      el('div', { class: 'modal-sheet-body', style: 'padding-top: 10px; padding-bottom: 12px;' }, [
        el('p', { style: 'font-size: 13px; line-height: 1.5; margin: 0;', text: t('auth_check_intro', 'Authentication is on. Check the accounts and access tokens that exist on the Hub and delete any you do not recognise.') }),
        ...(tokensUnreadable ? [el('p', { class: 'auth-tokens-unreadable', style: 'font-size: 12px; line-height: 1.5; margin: 8px 0 0; color: var(--danger);', text: t('auth_tokens_unreadable', TOKENS_UNREADABLE_TEXT) })] : []),
        list,
        status
      ]),
      el('div', { class: 'modal-sheet-footer', style: 'display: flex; justify-content: flex-end; gap: 8px;' }, [keepAll])
    ]));
    document.body.appendChild(backdrop);
  });
}

const FIELD_WORDS = {
  deviceIds: ['device_field_device_list', 'device list'],
  startSequence: ['device_field_start_steps', 'start steps'],
  stopSequence: ['device_field_stop_steps', 'stop steps'],
  powerOnSequence: ['device_field_power_steps', 'power steps'],
  powerOffSequence: ['device_field_power_steps', 'power steps'],
};

function fieldWord(f) {
  const k = FIELD_WORDS[f];
  return k ? t(k[0], k[1]) : f;
}

// "46 keys in TV (DTT)", "1 key in the Receiver page"
function keysIn(g) {
  let where = g.name || g.id;
  if (g.type === 'device_page') where = tf('device_in_use_page', { name: g.name || g.id }, 'the {name} page');
  else if (g.type === 'no_activity') where = t('device_in_use_default_page', 'the default page');
  return g.count === 1
    ? tf('device_in_use_key_in', { name: where }, '1 key in {name}')
    : tf('device_in_use_keys_in', { count: g.count, name: where }, '{count} keys in {name}');
}

// The 409 "users" of a refused device delete as lines people can read
export function deviceUsersLines(users) {
  const lines = [];
  const acts = (users || []).filter((u) => u.type === 'activity');
  if (acts.length) {
    const fields = [];
    for (const a of acts) for (const f of a.fields || []) if (!fields.includes(fieldWord(f))) fields.push(fieldWord(f));
    lines.push(tf('device_in_use_activities', { names: acts.map((a) => a.name || a.id).join(', '), fields: fields.join(', ') },
      'Activities: {names} ({fields})'));
  }
  const devs = (users || []).filter((u) => u.type === 'device');
  if (devs.length) {
    lines.push(tf('device_in_use_bt_trigger', { names: devs.map((d) => d.name || d.id).join(', ') }, 'Bluetooth trigger of {names}'));
  }
  for (const u of users || []) {
    if (u.type !== 'remote_map' && u.type !== 'bt_remote_map') continue;
    const groups = Array.isArray(u.groups) && u.groups.length
      ? u.groups.map(keysIn).join(', ')
      : tf('device_in_use_keys', { count: u.count }, '{count} keys'); /* a Hub without groups */
    lines.push(u.type === 'remote_map'
      ? tf('device_in_use_slot', { slot: u.slot, groups }, 'Remote slot {slot}: {groups}')
      : tf('device_in_use_bt_remote', { groups }, 'Bluetooth remote: {groups}'));
  }
  return lines;
}

// Text for a device delete refused with 409: what still uses the device, and what to do
export function deviceUsersText(users) {
  return [t('device_in_use_intro', 'This device is still used:'), ...deviceUsersLines(users), '',
    t('device_in_use_hint', 'Remove the device from these first, then delete it.')].join('\n');
}

// The users list of a refused device delete, or null for any other error
export function deviceUsersOf(err) {
  return err && err.status === 409 && Array.isArray(err.data?.users) ? err.data.users : null;
}
