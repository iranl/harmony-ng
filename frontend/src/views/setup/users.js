// Users, Auth & API Tokens View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';
import { createModal } from '../../components/modal.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-users', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  const currentUser = localStorage.getItem('harmony_auth_user') || 'admin';
  const currentToken = localStorage.getItem('harmony_auth_token');

  // Auth Status Card
  const authCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('auth_status', 'Session & Authentication') }),
      h('span', { className: `badge ${currentToken ? 'badge-success' : 'badge-accent'}`, text: currentToken ? 'Authenticated' : 'Standard' }),
    ]),
    h('div', { style: { display: 'flex', alignItems: 'center', justifyContent: 'space-between' } }, [
      h('div', {}, [
        h('div', { style: { fontWeight: 'bold' }, text: `User: ${currentUser}` }),
        h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: currentToken ? 'Bearer token active' : 'No authentication token set' }),
      ]),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: currentToken ? 'Log Out' : 'Log In',
        onclick: () => {
          if (currentToken) {
            api.logout();
            toast.info('Logged out');
            setTimeout(() => window.location.reload(), 300);
          } else {
            openLoginModal();
          }
        },
      }),
    ]),
  ]);

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
    h('div', { id: 'users-list-mount', className: 'text-dim', text: 'Loading users...' }),
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
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '10px' }, text: 'API tokens for Home Assistant, scripts, or external automation.' }),
    h('div', { id: 'tokens-list-mount', className: 'text-dim', text: 'Loading tokens...' }),
  ]);

  async function loadUsers() {
    const mount = container.querySelector('#users-list-mount');
    if (!mount) return;
    try {
      const users = await api.getUsers();
      mount.innerHTML = '';
      const list = Array.isArray(users) ? users : (users?.users || []);
      if (list.length === 0) {
        mount.textContent = 'No user accounts configured.';
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
              await api.deleteUser(u.username || u);
              toast.info('User deleted');
              loadUsers();
            },
          }) : null,
        ]);
        listEl.appendChild(row);
      });
      mount.appendChild(listEl);
    } catch {
      mount.textContent = 'Authentication endpoint not enabled on Hub.';
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
        mount.textContent = 'No active API tokens.';
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
            h('div', { className: 'text-dim', style: { fontSize: '11px' }, text: `Role: ${tok.role} | Created: ${tok.created_at || 'Recently'}` }),
          ]),
          h('button', {
            className: 'btn btn-danger btn-sm',
            text: 'Revoke',
            onclick: async () => {
              await api.deleteToken(tok.id);
              toast.info('Token revoked');
              loadTokens();
            },
          }),
        ]);
        listEl.appendChild(row);
      });
      mount.appendChild(listEl);
    } catch {
      mount.textContent = 'API tokens managed via configuration.';
    }
  }

  function openLoginModal() {
    const userInput = h('input', { type: 'text', className: 'form-input', placeholder: 'admin' });
    const passInput = h('input', { type: 'password', className: 'form-input' });
    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: 'Username' }),
      userInput,
      h('label', { className: 'form-label', text: 'Password' }),
      passInput,
    ]);

    createModal({
      title: 'Log In to Harmony Hub',
      content,
      actions: [
        { label: 'Cancel' },
        {
          label: 'Log In',
          variant: 'primary',
          onClick: async () => {
            try {
              await api.login(userInput.value.trim(), passInput.value);
              toast.success('Logged in successfully!');
              setTimeout(() => window.location.reload(), 300);
              return true;
            } catch (e) {
              toast.error(e.message);
              return false;
            }
          },
        },
      ],
    });
  }

  function openAddUserModal() {
    const userInput = h('input', { type: 'text', className: 'form-input', placeholder: 'newuser' });
    const passInput = h('input', { type: 'password', className: 'form-input' });
    const roleSelect = h('select', { className: 'form-select' }, [
      h('option', { value: 'user', text: 'User (Control only)' }),
      h('option', { value: 'admin', text: 'Admin (Full access)' }),
    ]);

    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: 'Username' }),
      userInput,
      h('label', { className: 'form-label', text: 'Password' }),
      passInput,
      h('label', { className: 'form-label', text: 'Role' }),
      roleSelect,
    ]);

    createModal({
      title: 'Create User Account',
      content,
      actions: [
        { label: 'Cancel' },
        {
          label: 'Create',
          variant: 'primary',
          onClick: async () => {
            const username = userInput.value.trim();
            const password = passInput.value;
            if (!username || !password) return toast.warn('All fields required');
            await api.saveUser({ username, password, role: roleSelect.value });
            toast.success(`User ${username} created!`);
            loadUsers();
            return true;
          },
        },
      ],
    });
  }

  function openCreateTokenModal() {
    const nameInput = h('input', { type: 'text', className: 'form-input', placeholder: 'Home Assistant Token' });
    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: 'Token Description' }),
      nameInput,
    ]);

    createModal({
      title: 'Create API Token',
      content,
      actions: [
        { label: 'Cancel' },
        {
          label: 'Generate',
          variant: 'primary',
          onClick: async () => {
            const name = nameInput.value.trim();
            if (!name) return toast.warn('Token name required');
            const res = await api.createToken(name, 'admin');
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
      title: 'API Token Generated',
      content: h('div', {}, [
        h('p', { className: 'text-dim', style: { fontSize: '13px', marginBottom: '8px' }, text: 'Copy this token now. It will not be shown again.' }),
        tokArea,
      ]),
      actions: [
        {
          label: 'Copy & Close',
          variant: 'primary',
          onClick: () => {
            navigator.clipboard?.writeText(tokenStr);
            toast.success('Token copied to clipboard');
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
