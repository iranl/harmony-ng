// "Forget" on a Bluetooth host (legacy UI): ask, then POST /api/bt-forget-host.
// Returns true when the Hub forgot the host. Kept apart from app_bluetooth.js so it can be tested.
export function forgetConfirmText(t, name, addr) {
  return t('bt_forget_host_confirm', 'Forget {name} ({addr})?').split('{name}').join(name).split('{addr}').join(addr);
}

export async function forgetBtHostFlow({ client, addr, name, t, showConfirm, showToast }) {
  const label = name || addr;
  if (!await showConfirm(forgetConfirmText(t, label, addr), { title: t('bt_forget_host_title', 'Forget Bluetooth host'), okText: t('bt_forget_host', 'Forget') })) return false;
  try {
    await client.forgetBtHost(addr);
    showToast(t('bt_host_forgotten', 'Bluetooth host forgotten'), 'success');
    return true;
  } catch (err) {
    showToast(t('bt_forget_failed', 'Forget failed: ') + err.message, 'error');
    return false;
  }
}
