/*
 * Copy text to the clipboard from the legacy web UI.
 *
 * navigator.clipboard exists only in a secure context (HTTPS or localhost); the Hub serves the UI
 * over plain HTTP, so it is undefined there. Fall back to a selection and execCommand('copy'),
 * which browsers still allow from a click handler. Resolves to true when something was copied.
 */
export async function copyText(text) {
  if (!text) return false;
  if (typeof navigator !== 'undefined' && navigator.clipboard && navigator.clipboard.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (err) {
      /* permission denied or not a secure context after all: try the fallback */
    }
  }
  if (typeof document === 'undefined' || !document.body) return false;
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.setAttribute('readonly', '');
  ta.style.position = 'fixed';
  ta.style.top = '0';
  ta.style.left = '0';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.focus();
  ta.select();
  ta.setSelectionRange(0, text.length);
  let ok = false;
  try {
    ok = document.execCommand('copy');
  } catch (err) {
    ok = false;
  }
  document.body.removeChild(ta);
  return ok;
}
