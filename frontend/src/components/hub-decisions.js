// Two answers of the Hub shown to the user:
// - the accounts and access tokens to check, listed in the reply that turns authentication on
//   ("to_check"): keep them all, or delete the ones not recognised
// - a device delete refused because activities, devices or remote keys still use it (409 "users")
import { h } from '../utils/dom.js';
import { t } from '../i18n/index.js';
import { toast } from './toast.js';
import { createModal } from './modal.js';

/* After a request that turned authentication on: the accounts and access tokens to check (the
 * reply's "to_check"). With canDelete: Delete per item (one already gone, 404, counts as deleted)
 * and Keep all to close; without (no login yet): the names and how to go on. Never the account
 * logged in. Whatever is not deleted keeps working. */
export function showSwitchCheck(api, toCheck, canDelete = true) {
  const me = (typeof localStorage !== 'undefined' && localStorage.getItem('harmony_auth_user')) || '';
  const gone = t('auth_check_owner_gone', 'account gone');
  const items = [
    ...(toCheck?.users || []).filter((u) => u.username !== me)
      .map((u) => ({ kind: 'user', id: u.username, label: `${t('auth_check_account', 'Account')}: ${u.username} (${u.role})` })),
    ...(toCheck?.tokens || [])
      .map((tk) => ({ kind: 'token', id: tk.id, label: `${t('auth_check_token', 'Access token')}: ${tk.name || tk.id} (${tk.role}${tk.owner ? `, ${tk.owner}` : ''}${tk.owner_missing ? `, ${gone}` : ''})` })),
  ];
  if (!items.length) return null;
  const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px' } });
  for (const item of items) {
    const row = h('div', {
      className: 'card',
      style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', gap: '8px', padding: '8px 12px', margin: 0 },
    }, [
      h('span', { style: { wordBreak: 'break-word' }, text: item.label }),
      canDelete ? h('button', {
        className: 'btn btn-danger btn-sm',
        text: t('delete', 'Delete'),
        onclick: async () => {
          try {
            if (item.kind === 'user') await api.deleteUser(item.id);
            else await api.deleteToken(item.id);
          } catch (e) {
            if (e.status !== 404) {
              toast.error(`${item.label}: ${e.message}`);
              return;
            }
          }
          row.remove();
        },
      }) : null,
    ]);
    list.appendChild(row);
  }
  return createModal({
    title: t('auth_check_title', 'Check accounts and access tokens'),
    content: h('div', {}, [
      h('p', { className: 'text-dim', style: { fontSize: '13px', marginBottom: '10px' }, text: t('auth_check_intro', 'Authentication is on. Check the accounts and access tokens that exist on the Hub and delete any you do not recognise.') }),
      canDelete ? null : h('p', { style: { fontSize: '13px', marginBottom: '10px' }, text: t('auth_check_login_first', 'Log in with an admin account on the Users page: this list is shown again there, with Delete.') }),
      list,
    ]),
    actions: [{ label: canDelete ? t('auth_keep_all', 'Keep all') : 'OK', variant: 'primary' }],
  });
}

const PENDING_KEY = 'harmony_auth_switch_check';

function readPending() {
  try {
    const raw = typeof sessionStorage !== 'undefined' ? sessionStorage.getItem(PENDING_KEY) : null;
    return raw ? JSON.parse(raw) : null;
  } catch {
    return null;
  }
}

/* The list stays in sessionStorage until an admin login exists to act on it: the switch ended
 * every session, and the Users page login reloads the page. Shows it then (once), the names only
 * while nobody is logged in, nothing for a user-role login (the list waits for an admin). */
export async function showPendingSwitchCheck(api) {
  const pending = readPending();
  if (!pending) return null;
  let me = null;
  try {
    me = await api.getAuthMe();
  } catch {
    me = null;
  }
  if (!me?.authenticated) return showSwitchCheck(api, pending, false);
  if (me.role !== 'admin') return null;
  try {
    sessionStorage.removeItem(PENDING_KEY);
  } catch { /* nothing to remove */ }
  return showSwitchCheck(api, pending, true);
}

/* Once per page: the restore reply's list (event from api/system.js) is kept and shown; a list kept
 * from before a reload is shown now */
export function installSwitchCheck(api) {
  if (typeof window === 'undefined' || window.__harmonySwitchCheckInstalled) return;
  window.__harmonySwitchCheckInstalled = true;
  window.addEventListener('harmony:auth-switch-check', (e) => {
    try {
      sessionStorage.setItem(PENDING_KEY, JSON.stringify(e.detail || {}));
    } catch { /* shown once below anyway */ }
    showPendingSwitchCheck(api);
  });
  showPendingSwitchCheck(api);
}

/* Text lines for the users of a device whose delete was refused */
export function deviceUsersLines(users) {
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
  return lines;
}

/* Shows what still uses the device when the Hub refused its delete (409 with "users"). Returns
 * false for any other error, which the caller reports itself. */
export function showDeviceInUse(err) {
  const users = err && err.status === 409 && Array.isArray(err.data?.users) ? err.data.users : null;
  if (!users) return false;
  createModal({
    title: t('device_in_use_title', 'Device in use'),
    content: h('div', {}, [
      h('p', { style: { marginBottom: '8px' }, text: err.message }),
      h('ul', { style: { margin: '0 0 8px 18px', fontSize: '13px' } }, deviceUsersLines(users).map((l) => h('li', { text: l }))),
      h('p', { className: 'text-dim', style: { fontSize: '12px' }, text: t('device_in_use_hint', 'Remove the device from these first, then delete it.') }),
    ]),
    actions: [{ label: 'OK', variant: 'primary' }],
  });
  return true;
}
