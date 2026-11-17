// Login dialog, shared by the Users page and the app-wide handler for a refused login (401)
import { h } from '../utils/dom.js';
import { api } from '../api/index.js';
import { toast } from './toast.js';
import { createModal } from './modal.js';
import { t } from '../i18n/index.js';

export const MAX_USERNAME_BYTES = 31; // the Hub keeps user names shorter than 32 bytes
export const MAX_PASSWORD_BYTES = 128;

const byteLength = s => new TextEncoder().encode(String(s || '')).length;

// Why the Hub would refuse this user name or password, or null when it is fine
export function credentialsError(username, password) {
  if (!username) return t('username_required', 'User name required');
  if (byteLength(username) > MAX_USERNAME_BYTES) return t('username_too_long', 'User name too long (at most {max} bytes)', { max: MAX_USERNAME_BYTES });
  if (/[:\r\n]/.test(username)) return t('username_bad_chars', 'User name must not contain ":" or a line break');
  if (password !== undefined && byteLength(password) > MAX_PASSWORD_BYTES) return t('password_too_long', 'Password too long (at most {max} bytes)', { max: MAX_PASSWORD_BYTES });
  return null;
}

let openDialog = null;

export function openLoginModal({ message = '' } = {}) {
  if (openDialog && openDialog.backdrop.parentNode) return openDialog;
  const userInput = h('input', { type: 'text', className: 'form-input', placeholder: 'admin', maxlength: MAX_USERNAME_BYTES, autocomplete: 'username' });
  const passInput = h('input', { type: 'password', className: 'form-input', autocomplete: 'current-password' });
  const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
    message ? h('p', { className: 'text-dim', style: { fontSize: '13px' }, text: message }) : null,
    h('label', { className: 'form-label', text: t('username', 'Username') }),
    userInput,
    h('label', { className: 'form-label', text: t('password', 'Password') }),
    passInput,
  ]);

  const dialog = createModal({
    title: t('login_to_hub', 'Log In to Harmony Hub'),
    content,
    actions: [
      { label: t('cancel', 'Cancel') },
      {
        label: t('log_in', 'Log In'),
        variant: 'primary',
        onClick: async () => {
          try {
            await api.login(userInput.value.trim(), passInput.value);
            toast.success(t('logged_in', 'Logged in successfully!'));
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
  openDialog = dialog;
  return dialog;
}

// A refused login anywhere in the app opens the login dialog once; after the user closes it
// the Users page still has Log In
export function installUnauthorizedHandler() {
  let asked = false;
  window.addEventListener('harmony:unauthorized', () => {
    if (asked) return;
    asked = true;
    openLoginModal({ message: t('login_required_msg', 'The Hub asks for a login: the saved session is no longer valid or none is set.') });
  });
}
