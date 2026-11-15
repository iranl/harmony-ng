// Two questions of the Hub shown to the user:
// - accounts and access tokens made while authentication was off ("confirmed": false in
//   GET /api/users and GET /api/auth/tokens): keep (POST /api/auth/confirm) or delete each one
// - a device delete refused because activities, devices or remote keys still use it (409 "users")
import { h } from '../utils/dom.js';
import { t } from '../i18n/index.js';
import { toast } from './toast.js';
import { createModal } from './modal.js';

/* A card listing the accounts and tokens waiting to be kept (made or changed while authentication
 * was off; files from older firmware only after the next switch from off to on), while
 * authentication is on, never the account logged in. Hidden when there is nothing to ask about;
 * onChange runs after a keep or delete (to redraw other lists). */
export function createUnconfirmedCard(api, onChange) {
  const card = h('div', { className: 'card', style: { display: 'none' } });

  async function load() {
    let users = [];
    let tokens = [];
    let authOn = false;
    try {
      const u = await api.getUsers();
      authOn = Boolean(u?.auth_enabled);
      const me = (typeof localStorage !== 'undefined' && localStorage.getItem('harmony_auth_user')) || '';
      users = (u?.users || []).filter((x) => x.confirmed === false && x.username !== me);
      const tk = await api.getTokens();
      tokens = (tk?.tokens || []).filter((x) => x.confirmed === false);
    } catch {
      card.style.display = 'none';
      return;
    }
    const items = [
      ...users.map((u) => ({ kind: 'user', id: u.username, label: `${t('auth_unconfirmed_account', 'Account')}: ${u.username} (${u.role})` })),
      ...tokens.map((tk) => ({ kind: 'token', id: tk.id, label: `${t('auth_unconfirmed_token', 'Access token')}: ${tk.name || tk.id} (${tk.role})` })),
    ];
    if (!authOn || items.length === 0) {
      card.style.display = 'none';
      card.replaceChildren();
      return;
    }

    const act = async (list, keep) => {
      // every item gets its turn; a token already gone with its account (404) counts as deleted
      for (const item of list) {
        try {
          if (keep) {
            await api.confirmAuthItems(item.kind === 'user' ? [item.id] : [], item.kind === 'token' ? [item.id] : []);
          } else if (item.kind === 'user') {
            await api.deleteUser(item.id);
          } else {
            await api.deleteToken(item.id);
          }
        } catch (e) {
          if (keep || e.status !== 404) toast.error(`${item.label}: ${e.message}`);
        }
      }
      await load();
      if (onChange) onChange();
    };

    card.style.display = '';
    card.replaceChildren(
      h('div', { className: 'card-header' }, [
        h('h3', { className: 'card-title', text: t('auth_unconfirmed_title', 'Made while authentication was off') }),
      ]),
      h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '10px' }, text: t('auth_unconfirmed_intro', 'These accounts and access tokens were made while authentication was off, when anybody on the network could make them. Keep the ones you know and delete the others.') }),
      h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px' } }, items.map((item) => h('div', {
        className: 'card',
        style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', gap: '8px', padding: '8px 12px', margin: 0 },
      }, [
        h('span', { style: { wordBreak: 'break-word' }, text: item.label }),
        h('div', { style: { display: 'flex', gap: '6px' } }, [
          h('button', { className: 'btn btn-secondary btn-sm', text: t('auth_keep', 'Keep'), onclick: () => act([item], true) }),
          h('button', { className: 'btn btn-danger btn-sm', text: t('delete', 'Delete'), onclick: () => act([item], false) }),
        ]),
      ]))),
      h('div', { style: { display: 'flex', gap: '8px', justifyContent: 'flex-end', marginTop: '10px' } }, [
        h('button', { className: 'btn btn-danger btn-sm', text: t('auth_delete_all', 'Delete all'), onclick: () => act(items, false) }),
        h('button', { className: 'btn btn-secondary btn-sm', text: t('auth_keep_all', 'Keep all'), onclick: () => act(items, true) }),
      ]),
    );
  }

  return { card, load };
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
