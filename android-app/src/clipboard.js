/*
 * Copy text to the clipboard from the legacy web UI.
 *
 * navigator.clipboard exists only in a secure context (HTTPS or localhost); the Hub serves the UI
 * over plain HTTP, so it is undefined there. Fall back to a selection and execCommand('copy'),
 * which browsers still allow from a click handler. Resolves to true when something was copied.
 */
export async function copyText(text) {
  if (!text) return false;
  /* Only in a secure context: on plain HTTP a Clipboard API that exists but rejects would cost the
   * click's user activation while awaiting, and the fallback below would then fail too. */
  if (typeof window !== 'undefined' && window.isSecureContext &&
      typeof navigator !== 'undefined' && navigator.clipboard && navigator.clipboard.writeText) {
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
  /* off-screen, so that focusing it does not scroll the page on a phone */
  ta.style.position = 'fixed';
  ta.style.top = '-9999px';
  ta.style.left = '-9999px';
  ta.style.opacity = '0';
  ta.setAttribute('aria-hidden', 'true');
  ta.setAttribute('tabindex', '-1');
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
