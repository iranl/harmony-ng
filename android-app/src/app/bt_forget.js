// "Forget" on a Bluetooth host (legacy UI): ask, then POST /api/bt-forget-host.
// Returns true when the Hub forgot the host. Kept apart from app_bluetooth.js so it can be tested.
export function forgetConfirmText(ui, name, addr) {
  return ui.t('bt_forget_host_confirm', 'Forget {name} ({addr})?').split('{name}').join(name).split('{addr}').join(addr);
}

// ui carries client, addr, name and the helpers t, showConfirm and showToast, passed in so that a
// test can give its own; they are called through ui.
export async function forgetBtHostFlow(ui) {
  const { client, addr, name } = ui;
  const label = name || addr;
  if (!await ui.showConfirm(forgetConfirmText(ui, label, addr), { title: ui.t('bt_forget_host_title', 'Forget Bluetooth host'), okText: ui.t('bt_forget_host', 'Forget') })) return false;
  try {
    await client.forgetBtHost(addr);
    ui.showToast(ui.t('bt_host_forgotten', 'Bluetooth host forgotten'), 'success');
    return true;
  } catch (err) {
    ui.showToast(ui.t('bt_forget_failed', 'Forget failed: ') + err.message, 'error');
    return false;
  }
}
