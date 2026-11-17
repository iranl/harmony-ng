// Limits the Hub enforces on what the forms send, checked before sending so the user sees why
// (the Hub checks the same limits; this only gives the reason up front).

export const MAX_USERNAME_BYTES = 31; // the Hub keeps user names shorter than 32 bytes
export const MAX_PASSWORD_BYTES = 128;
export const DEVICE_DELAY_MAX_MS = 10000; // interDeviceDelay, interKeyDelay, holdInterKeyDelay

import { t, tf } from '../i18n.js';

const byteLength = s => new TextEncoder().encode(String(s || '')).length;

// Why the Hub would refuse this user name or password, or null when it is fine
export function credentialsError(username, password) {
  if (!username) return t('user_name_required');
  if (byteLength(username) > MAX_USERNAME_BYTES) return tf('user_name_too_long', { max: MAX_USERNAME_BYTES });
  if (/[:\r\n]/.test(username)) return t('user_name_bad_chars');
  if (password && byteLength(password) > MAX_PASSWORD_BYTES) return tf('password_too_long', { max: MAX_PASSWORD_BYTES });
  return null;
}

// WPA pass phrase: 8 to 63 characters, or 64 hex digits. An open network takes none; an empty
// field is left to the Hub (it may keep the saved password), as in the new UI.
export function wifiPassphraseError(password, { open = false } = {}) {
  if (open || !password) return null;
  if (password.length >= 8 && password.length <= 63) return null;
  if (/^[0-9a-fA-F]{64}$/.test(password)) return null;
  return t('wifi_password_length');
}

export function deviceDelayError(label, value) {
  if (value === undefined || Number.isNaN(value)) return null;
  if (!Number.isInteger(value) || value < 0 || value > DEVICE_DELAY_MAX_MS) return tf('device_delay_range', { label: label, max: DEVICE_DELAY_MAX_MS });
  return null;
}
