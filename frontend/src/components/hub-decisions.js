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
  // the Hub could not read its token file: its tokens are not in the list, say so
  const tokensUnreadable = Boolean(toCheck?.tokens_unreadable);
  if (!items.length && !tokensUnreadable) return null;
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
      ...(tokensUnreadable ? [h('p', { className: 'auth-tokens-unreadable', style: { fontSize: '12px', marginBottom: '10px', color: 'var(--danger)' }, text: t('auth_tokens_unreadable', 'The Hub cannot read its access token file, so its tokens are not listed. An admin can move the file aside with POST /api/auth/tokens-reset; every token then has to be created again.') })] : []),
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
  if (g.type === 'device_page') where = t('device_in_use_page', 'the {name} page', { name: g.name || g.id });
  else if (g.type === 'no_activity') where = t('device_in_use_default_page', 'the default page');
  return g.count === 1
    ? t('device_in_use_key_in', '1 key in {name}', { name: where })
    : t('device_in_use_keys_in', '{count} keys in {name}', { count: g.count, name: where });
}

// The 409 "users" of a refused device delete as lines people can read
export function deviceUsersLines(users) {
  const lines = [];
  const acts = (users || []).filter((u) => u.type === 'activity');
  if (acts.length) {
    const fields = [];
    for (const a of acts) for (const f of a.fields || []) if (!fields.includes(fieldWord(f))) fields.push(fieldWord(f));
    lines.push(t('device_in_use_activities', 'Activities: {names} ({fields})',
      { names: acts.map((a) => a.name || a.id).join(', '), fields: fields.join(', ') }));
  }
  const devs = (users || []).filter((u) => u.type === 'device');
  if (devs.length) {
    lines.push(t('device_in_use_bt_trigger', 'Bluetooth trigger of {names}', { names: devs.map((d) => d.name || d.id).join(', ') }));
  }
  for (const u of users || []) {
    if (u.type !== 'remote_map' && u.type !== 'bt_remote_map') continue;
    const groups = Array.isArray(u.groups) && u.groups.length
      ? u.groups.map(keysIn).join(', ')
      : t('device_in_use_keys', '{count} keys', { count: u.count }); /* a Hub without groups */
    lines.push(u.type === 'remote_map'
      ? t('device_in_use_slot', 'Remote slot {slot}: {groups}', { slot: u.slot, groups })
      : t('device_in_use_bt_remote', 'Bluetooth remote: {groups}', { groups }));
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
      h('p', { style: { marginBottom: '8px' }, text: t('device_in_use_intro', 'This device is still used:') }),
      h('ul', { style: { margin: '0 0 8px 18px', fontSize: '13px' } }, deviceUsersLines(users).map((l) => h('li', { text: l }))),
      h('p', { className: 'text-dim', style: { fontSize: '12px' }, text: t('device_in_use_hint', 'Remove the device from these first, then delete it.') }),
    ]),
    actions: [{ label: 'OK', variant: 'primary' }],
  });
  return true;
}
