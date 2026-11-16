// Login dialog, shared by the Users page and the app-wide handler for a refused login (401)
import { h } from '../utils/dom.js';
import { api } from '../api/index.js';
import { toast } from './toast.js';
import { createModal } from './modal.js';

export const MAX_USERNAME_BYTES = 31; // the Hub keeps user names shorter than 32 bytes
export const MAX_PASSWORD_BYTES = 128;

const byteLength = s => new TextEncoder().encode(String(s || '')).length;

// Why the Hub would refuse this user name or password, or null when it is fine
export function credentialsError(username, password) {
  if (!username) return 'User name required';
  if (byteLength(username) > MAX_USERNAME_BYTES) return `User name too long (at most ${MAX_USERNAME_BYTES} bytes)`;
  if (/[:\r\n]/.test(username)) return 'User name must not contain ":" or a line break';
  if (password !== undefined && byteLength(password) > MAX_PASSWORD_BYTES) return `Password too long (at most ${MAX_PASSWORD_BYTES} bytes)`;
  return null;
}

let openDialog = null;

export function openLoginModal({ message = '' } = {}) {
  if (openDialog && openDialog.backdrop.parentNode) return openDialog;
  const userInput = h('input', { type: 'text', className: 'form-input', placeholder: 'admin', maxlength: MAX_USERNAME_BYTES, autocomplete: 'username' });
  const passInput = h('input', { type: 'password', className: 'form-input', autocomplete: 'current-password' });
  const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
    message ? h('p', { className: 'text-dim', style: { fontSize: '13px' }, text: message }) : null,
    h('label', { className: 'form-label', text: 'Username' }),
    userInput,
    h('label', { className: 'form-label', text: 'Password' }),
    passInput,
  ]);

  const dialog = createModal({
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
    openLoginModal({ message: 'The Hub asks for a login: the saved session is no longer valid or none is set.' });
  });
}
