// Users, Auth & API Tokens View
import { h, copyText } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';
import { createModal } from '../../components/modal.js';
import { openLoginModal, credentialsError, MAX_USERNAME_BYTES } from '../../components/login-modal.js';

// One window listener for every Users view: a new view replaces the renderer instead of adding
// a listener per visit (each kept its detached view alive)
let renderCurrentAuthCard = null;
let unauthorizedListening = false;

export function createView() {
  const container = h('div', { className: 'setup-subview-users', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  // Session card: drawn from the client's token, again when the Hub refuses it (401)
  const authCard = h('div', { className: 'card' });
  function renderAuthCard() {
    const currentUser = localStorage.getItem('harmony_auth_user') || 'admin';
    const currentToken = api.token;
    authCard.replaceChildren(
      h('div', { className: 'card-header' }, [
        h('h3', { className: 'card-title', text: t('auth_status', 'Session & Authentication') }),
        h('span', { className: `badge ${currentToken ? 'badge-success' : 'badge-accent'}`, text: currentToken ? t('auth_badge_authenticated', 'Authenticated') : t('auth_badge_standard', 'Standard') }),
      ]),
      h('div', { style: { display: 'flex', alignItems: 'center', justifyContent: 'space-between' } }, [
        h('div', {}, [
          h('div', { style: { fontWeight: 'bold' }, text: currentToken ? t('auth_user_label', 'User: {name}', { name: currentUser }) : t('not_logged_in', 'Not logged in') }),
          h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: currentToken ? t('bearer_token_active', 'Bearer token active') : t('no_auth_token_set', 'No authentication token set') }),
        ]),
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: currentToken ? t('log_out', 'Log Out') : t('log_in', 'Log In'),
          onclick: async (e) => {
            if (currentToken) {
              e.target.disabled = true;
              await api.logout();
              toast.info(t('logged_out', 'Logged out'));
              setTimeout(() => window.location.reload(), 300);
            } else {
              openLoginModal();
            }
          },
        }),
      ]),
    );
  }
  renderAuthCard();
  renderCurrentAuthCard = () => {
    if (container.isConnected) renderAuthCard();
  };
  if (!unauthorizedListening) {
    unauthorizedListening = true;
    window.addEventListener('harmony:unauthorized', () => renderCurrentAuthCard?.());
  }

  // Users Card
  const usersCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('users', 'User Accounts') }),
      h('button', {
        className: 'btn btn-primary btn-sm',
        text: `+ ${t('add_user', 'Add User')}`,
        onclick: () => openAddUserModal(),
      }),
    ]),
    h('div', { id: 'users-list-mount', className: 'text-dim', text: t('loading_users', 'Loading users...') }),
  ]);

  // API Tokens Card
  const tokensCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('api_tokens', 'API Tokens') }),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: `+ ${t('generate_token', 'Create Token')}`,
        onclick: () => openCreateTokenModal(),
      }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '10px' }, text: t('api_tokens_desc', 'API tokens for Home Assistant, scripts, or external automation.') }),
    h('div', { id: 'tokens-list-mount', className: 'text-dim', text: t('loading_tokens', 'Loading tokens...') }),
  ]);

  async function loadUsers() {
    const mount = container.querySelector('#users-list-mount');
    if (!mount) return;
    try {
      const users = await api.getUsers();
      mount.innerHTML = '';
      const list = Array.isArray(users) ? users : (users?.users || []);
      if (list.length === 0) {
        mount.textContent = t('no_user_accounts', 'No user accounts configured.');
        return;
      }
      const listEl = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px' } });
      list.forEach(u => {
        const row = h('div', {
          className: 'card',
          style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '8px 12px', margin: 0 },
        }, [
          h('div', {}, [
            h('span', { style: { fontWeight: 'bold' }, text: u.username || u }),
            h('span', { className: 'badge', style: { marginLeft: '8px' }, text: u.role || 'user' }),
          ]),
          u.username !== 'admin' ? h('button', {
            className: 'btn btn-danger btn-sm',
            text: '×',
            onclick: async () => {
              const uname = u.username || u;
              const ok = await confirm.ask({
                title: t('delete_user', 'Delete User'),
                message: t('delete_user_confirm', 'Delete non-admin user "{name}"?', { name: uname }),
                danger: true,
              });
              if (!ok) return;
              try {
                await api.deleteUser(uname);
                toast.info(t('user_deleted', 'User deleted'));
              } catch (e) {
                toast.error(e.message);
              }
              loadUsers();
            },
          }) : null,
        ]);
        listEl.appendChild(row);
      });
      mount.appendChild(listEl);
    } catch (e) {
      mount.textContent = t('cannot_load_users', 'Cannot load the user accounts: {error}', { error: e.message });
    }
  }

  async function loadTokens() {
    const mount = container.querySelector('#tokens-list-mount');
    if (!mount) return;
    try {
      const tokens = await api.getTokens();
      mount.innerHTML = '';
      const list = Array.isArray(tokens) ? tokens : (tokens?.tokens || []);
      if (list.length === 0) {
        mount.textContent = t('no_active_tokens', 'No active API tokens.');
        return;
      }
      const listEl = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px' } });
      list.forEach(tok => {
        const row = h('div', {
          className: 'card',
          style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '8px 12px', margin: 0 },
        }, [
          h('div', {}, [
            h('div', { style: { fontWeight: 'bold' }, text: tok.name }),
            h('div', { className: 'text-dim', style: { fontSize: '11px' }, text: t('token_role_created', 'Role: {role} | Created: {created}', { role: tok.role, created: tok.created_at || t('recently', 'Recently') }) }),
          ]),
          // the account the token belongs to (and ends with)
          h('div', { className: 'text-dim', style: { fontSize: '11px' }, text: t('token_owner', 'Owner: {owner}', { owner: tok.owner || '-' }) }),
          h('button', {
            className: 'btn btn-danger btn-sm',
            text: t('revoke', 'Revoke'),
            onclick: async () => {
              const ok = await confirm.ask({
                title: t('revoke_token', 'Revoke Token'),
                message: t('revoke_pat_confirm', 'Revoke Personal Access Token "{name}"? Third-party integrations using this token will stop working.', { name: tok.name }),
                danger: true,
              });
              if (!ok) return;
              try {
                await api.deleteToken(tok.id);
                toast.info(t('token_revoked', 'Token revoked'));
              } catch (e) {
                toast.error(e.message);
              }
              loadTokens();
            },
          }),
        ]);
        listEl.appendChild(row);
      });
      mount.appendChild(listEl);
    } catch (e) {
      mount.textContent = t('cannot_load_tokens', 'Cannot load the API tokens: {error}', { error: e.message });
    }
  }

  function openAddUserModal() {
    const userInput = h('input', { type: 'text', className: 'form-input', placeholder: 'newuser', maxlength: MAX_USERNAME_BYTES });  // i18n-ok: example value
    const passInput = h('input', { type: 'password', className: 'form-input' });
    const roleSelect = h('select', { className: 'form-select' }, [
      h('option', { value: 'user', text: t('role_user_control_only', 'User (Control only)') }),
      h('option', { value: 'admin', text: t('role_admin_full_access', 'Admin (Full access)') }),
    ]);

    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: t('username', 'Username') }),
      userInput,
      h('label', { className: 'form-label', text: t('password', 'Password') }),
      passInput,
      h('label', { className: 'form-label', text: t('role', 'Role') }),
      roleSelect,
    ]);

    createModal({
      title: t('create_user_account', 'Create User Account'),
      content,
      actions: [
        { label: t('cancel', 'Cancel') },
        {
          label: t('create', 'Create'),
          variant: 'primary',
          onClick: async () => {
            const username = userInput.value.trim();
            const password = passInput.value;
            if (!username || !password) { toast.warn(t('all_fields_required', 'All fields required')); return false; }
            const problem = credentialsError(username, password);
            if (problem) { toast.warn(problem); return false; }
            await api.saveUser({ username, password, role: roleSelect.value });
            toast.success(t('user_created', 'User {name} created!', { name: username }));
            loadUsers();
            return true;
          },
        },
      ],
    });
  }

  function openCreateTokenModal() {
    const nameInput = h('input', { type: 'text', className: 'form-input', placeholder: 'Home Assistant Token' });  // i18n-ok: example value
    // Same choice as the legacy UI: a control-only token for scripts that only press keys
    const roleSelect = h('select', { className: 'form-select' }, [
      h('option', { value: 'admin', text: t('role_admin_full_access', 'Admin (Full access)') }),
      h('option', { value: 'user', text: t('role_operator_control_only', 'Operator (Control only)') }),
    ]);
    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: t('token_description', 'Token Description') }),
      nameInput,
      h('label', { className: 'form-label', text: t('role', 'Role') }),
      roleSelect,
    ]);

    createModal({
      title: t('create_api_token', 'Create API Token'),
      content,
      actions: [
        { label: t('cancel', 'Cancel') },
        {
          label: t('generate', 'Generate'),
          variant: 'primary',
          onClick: async () => {
            const name = nameInput.value.trim();
            if (!name) { toast.warn(t('token_name_required', 'Token name required')); return false; }
            const res = await api.createToken(name, roleSelect.value);
            if (res && res.token) {
              promptShowToken(res.token);
            }
            loadTokens();
            return true;
          },
        },
      ],
    });
  }

  function promptShowToken(tokenStr) {
    const tokArea = h('textarea', {
      className: 'form-textarea',
      rows: 3,
      readonly: true,
      value: tokenStr,
    });

    createModal({
      title: t('api_token_generated', 'API Token Generated'),
      content: h('div', {}, [
        h('p', { className: 'text-dim', style: { fontSize: '13px', marginBottom: '8px' }, text: t('copy_token_now', 'Copy this token now. It will not be shown again.') }),
        tokArea,
      ]),
      actions: [
        {
          label: t('copy_and_close', 'Copy & Close'),
          variant: 'primary',
          onClick: async () => {
            if (await copyText(tokenStr)) {
              toast.success(t('token_copied', 'Token copied to clipboard'));
              return true;
            }
            /* keep the dialog open: the token is shown only once */
            toast.error(t('clipboard_copy_by_hand', 'Clipboard not available: select the token and copy it by hand'));
            tokArea.focus();
            tokArea.select();
            return false;
          },
        },
      ],
    });
  }

  container.appendChild(authCard);
  container.appendChild(usersCard);
  container.appendChild(tokensCard);

  loadUsers();
  loadTokens();
  return container;
}

export default createView;
