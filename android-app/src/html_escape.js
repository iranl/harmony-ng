// Escape a value for HTML text or a quoted attribute (same rules as escapeHtml in
// app/app_icons_ui.js). Every name that comes from the Hub, an IR database or another
// host goes through this before it is put into innerHTML.
export function escapeHtml(s) {
  if (s === null || s === undefined) return '';
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}
