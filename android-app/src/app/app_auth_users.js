import { HarmonyClient } from '../api.js';
import { getSavedHubs, saveHub, deleteHub, getActiveHub, setActiveHubId } from '../storage.js';
import { RemoteEngine } from '../remote-engine.js';
import {
  getHarmonyManufacturers,
  searchHarmonyManufacturers,
  fetchHarmonyModels,
  searchHarmonyModels,
  fetchHarmonyDeviceDetail,
  fetchHarmonyCodeset,
  mapHarmonyDeviceType,
  buildDeviceFromHarmony,
  parseIrText,
  loadIrdSource,
  rankIrdEntries,
  fetchIrdFile,
  sourceLabel,
  safeImportName
} from '../irdb.js';
import { t, setLanguage, getInitialLanguage, tf, tfHtml } from '../i18n.js';
import { copyText } from '../clipboard.js';
import iconMetadata from '../icon-metadata.json';
import {
  initIconCache,
  hasCachedIcon,
  getCachedIconUrl,
  cacheIconFromUrl,
  syncIconsFromHub,
  syncIconsFromOta,
  getCachedIconsCount,
  clearIconCache,
  getAllCachedKeys
} from '../icon-cache.js';
import {
  APP_VERSION,
  stockIconsAvailable,
  stockIconBaseUrl,
  failedIconUrls,
  setStockIconBaseUrl,
  setStockIconsAvailable,
  escapeHtml,
  autoInvertDarkIcon,
  showToast,
  showConfirm,
  isCustomIcon,
  isDarkMonochromeIcon,
  getStockIconUrl,
  getActivityIconSvg,
  getRawActivityIconSvg,
  getScreenBtnIconSvg,
  getRawScreenBtnIconSvg,
  getDeviceIconSvg,
  getRawDeviceIconSvg
} from './app_icons_ui.js';
import { credentialsError } from './form_limits.js';
import { HarmonyAppSettingsSystem } from './app_settings_system.js';
import { askToCheck, checkItems } from './decision_dialogs.js';

export class HarmonyAppAuthUsers extends HarmonyAppSettingsSystem {
  async loadUserAccounts() {
    const listEl = document.getElementById('usersListContainer');
    const chk = document.getElementById('authEnabledCheck');
    const adminUser = document.getElementById('authUsername');
    if (!this.client || typeof this.client.getUsers !== 'function') return;

    try {
      const data = await this.client.getUsers();
      if (!data || !data.ok) return;

      if (chk) chk.checked = Boolean(data.auth_enabled);
      const users = data.users || [];
      this.renderSwitchCheckNotice(data, listEl);

      const admin = users.find(u => u.role === 'admin');
      if (admin && adminUser) adminUser.value = admin.username;

      const nonAdmins = users.filter(u => u.role === 'user');
      if (!listEl) return;

      if (nonAdmins.length === 0) {
        listEl.innerHTML = `<div class="subtle mini" style="padding: 4px 0;">${t('no_non_admin_configured')}</div>`;
        return;
      }

      let html = '<div style="display: flex; flex-direction: column; gap: 6px;">';
      for (const u of nonAdmins) {
        html += `
          <div style="display: flex; align-items: center; justify-content: space-between; padding: 6px 10px; background: rgba(255,255,255,0.04); border-radius: 4px; border: 1px solid var(--border);">
            <div>
              <span style="font-weight: 600; font-size: 13px;">${escapeHtml(u.username)}</span>
              <span class="badge" style="margin-left: 8px; font-size: 10px; background: rgba(59, 130, 246, 0.15); color: var(--accent);">${t('restricted_user_badge')}</span>
            </div>
            <button type="button" class="btn btn-xs btn-danger btn-del-user" data-username="${escapeHtml(u.username)}">${t('delete')}</button>
          </div>`;
      }
      html += '</div>';
      listEl.innerHTML = html;

      listEl.querySelectorAll('.btn-del-user').forEach(btn => {
        btn.addEventListener('click', async () => {
          const uname = btn.getAttribute('data-username');
          if (!await showConfirm(tf("delete_user_confirm", { name: uname }), { title: t('delete_user'), okText: t('delete') })) return;
          try {
            await this.client.deleteUser(uname);
            await this.loadUserAccounts();
          } catch (err) {
            showToast(t("failed_delete_user") + err.message, 'error');
          }
        });
      });
    } catch (err) {
      if (listEl) listEl.innerHTML = `<div class="subtle mini" style="color:var(--danger)">${tfHtml('failed_load_users', { error: err.message })}</div>`;
    }
  }

  // The accounts and tokens to check that the Hub answered when authentication was turned on,
  // shown once there is a login from after the switch to act with
  async showSwitchCheck() {
    const pending = this.pendingSwitchCheck;
    if (!pending || !this.client?.token) return;
    // the switch ended every session: only a login made after it can delete anything
    let session = null;
    try {
      session = await this.client.getAuthMe();
    } catch {
      session = null;
    }
    if (!session?.authenticated) {
      this.showLoginModal(); /* the list stays pending until the login succeeds */
      return;
    }
    if (session.role !== 'admin') return; /* a user account cannot delete them: the list waits for an admin */
    this.pendingSwitchCheck = null;
    const me = (typeof localStorage !== 'undefined' && localStorage.getItem('harmony_auth_user')) || '';
    this.lastSwitchCheck = pending;
    await askToCheck(this.client, pending, me);
    await this.loadUserAccounts();
  }

  // Users page: the result of the last switch in this session, to open again
  renderSwitchCheckNotice(usersData, listEl) {
    if (!listEl?.parentNode) return;
    let box = document.getElementById('switchCheckNotice');
    if (!box) {
      box = document.createElement('div');
      box.id = 'switchCheckNotice';
      box.style.cssText = 'margin: 6px 0;';
      listEl.parentNode.insertBefore(box, listEl);
    }
    box.replaceChildren();
    if (!usersData?.auth_enabled || !this.lastSwitchCheck) return;
    const me = (typeof localStorage !== 'undefined' && localStorage.getItem('harmony_auth_user')) || '';
    if (!checkItems(this.lastSwitchCheck, me).length) return;
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'btn btn-xs btn-secondary';
    btn.textContent = t('auth_check_title', 'Check accounts and access tokens');
    btn.addEventListener('click', async () => {
      await askToCheck(this.client, this.lastSwitchCheck, me);
      await this.loadUserAccounts();
    });
    box.appendChild(btn);
  }

  async addNonAdminUser() {
    const uInp = document.getElementById('newUserName');
    const pInp = document.getElementById('newUserPassword');
    const stat = document.getElementById('newUserStatus');
    const username = uInp?.value.trim();
    const password = pInp?.value;

    if (!username || !password) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">${t('user_pwd_required')}</span>`;
      return;
    }
    const problem = credentialsError(username, password);
    if (problem) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">${escapeHtml(problem)}</span>`;
      return;
    }

    if (stat) stat.textContent = t('adding_user');
    try {
      await this.client.saveUser({ username, password, role: 'user' });
      if (uInp) uInp.value = '';
      if (pInp) pInp.value = '';
      if (stat) stat.innerHTML = `<span style="color:var(--success)">${t('user_added')}</span>`;
      setTimeout(() => { if (stat) stat.textContent = ''; }, 3000);
      await this.loadUserAccounts();
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">${tfHtml('error_msg', { error: err.message })}</span>`;
    }
  }

  async saveWebUiAuth() {
    const status = document.getElementById('authSaveStatus');
    const on = document.getElementById('authEnabledCheck')?.checked ? 1 : 0;
    const u = document.getElementById('authUsername')?.value.trim();
    const p = document.getElementById('authPassword')?.value;
    // A name the Hub cannot keep would turn on authentication for an admin that can never log in
    const problem = u ? credentialsError(u, p) : null;
    if (problem) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">${escapeHtml(problem)}</span>`;
      return;
    }
    try {
      const authReply = await this.client.saveSystemAction('auth', { authEnabled: on, authUsername: u, authPassword: p });
      if (status) {
        status.innerHTML = `<span style="color:var(--success)">${on ? t('auth_enabled_admin_saved') : t('auth_disabled_saved')}</span>`;
        setTimeout(() => { if (status) status.textContent = ''; }, 3500);
      }
      const passInp = document.getElementById('authPassword');
      if (passInp) passInp.value = '';
      this.authEnabled = Boolean(on);

      if (on) {
        if (p && u) {
          try {
            await this.client.login(u, p);
          } catch (loginErr) {
            this.showLoginModal();
          }
        } else if (!this.client.token) {
          this.showLoginModal();
        }
        // the accounts and tokens to check: shown now, or right after the login when one is needed
        this.pendingSwitchCheck = authReply?.to_check || null;
        await this.showSwitchCheck();
      }

      await this.loadUserAccounts();
      this.checkAuthStatus();
      this.applyRoleRestrictions();
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">${tfHtml('error_msg', { error: err.message })}</span>`;
      if (err?.status === 401 || (err.message && (err.message.includes('Authentication') || err.message.includes('401')))) {
        this.showLoginModal();
      }
    }
  }

  async loadPersonalAccessTokens() {
    const listEl = document.getElementById('tokensListContainer');
    if (!this.client || typeof this.client.getTokens !== 'function' || !listEl) return;

    try {
      const data = await this.client.getTokens();
      if (!data || !data.ok) {
        listEl.innerHTML = `<div class="subtle mini">${t('unable_retrieve_tokens')}</div>`;
        return;
      }
      const tokens = data.tokens || [];
      if (tokens.length === 0) {
        listEl.innerHTML = `<div class="subtle mini" style="padding: 4px 0;">${t('no_pat_yet')}</div>`;
        return;
      }

      let html = '<div style="display: flex; flex-direction: column; gap: 6px;">';
      for (const tok of tokens) {
        const createdDate = tok.created_at ? new Date(tok.created_at * 1000).toLocaleDateString() : t('unknown');
        const lastUsed = tok.last_used_at ? new Date(tok.last_used_at * 1000).toLocaleString() : t('never');
        html += `
          <div style="display: flex; align-items: center; justify-content: space-between; padding: 6px 10px; background: rgba(255,255,255,0.04); border-radius: 4px; border: 1px solid var(--border);">
            <div>
              <div style="font-weight: 600; font-size: 13px;">${escapeHtml(tok.name || t('unnamed_token'))}</div>
              <div class="mini muted" style="font-size: 10px; margin-top: 2px;">
                <span>${escapeHtml(tf('id_value', { id: tok.id }))}</span> • <span>${escapeHtml(tf('token_meta_role', { role: tok.role }))}</span> • <span>${tfHtml('token_meta_created', { date: createdDate })}</span> • <span>${tfHtml('token_meta_last_used', { date: lastUsed })}</span>
              </div>
            </div>
            <button type="button" class="btn btn-xs btn-danger btn-del-pat" data-id="${escapeHtml(tok.id)}" data-name="${escapeHtml(tok.name || tok.id)}">${t('revoke')}</button>
          </div>`;
      }
      html += '</div>';
      listEl.innerHTML = html;
      showTokenOwners(listEl, tokens);

      listEl.querySelectorAll('.btn-del-pat').forEach(btn => {
        btn.addEventListener('click', async () => {
          const tid = btn.getAttribute('data-id');
          const tname = btn.getAttribute('data-name');
          if (!await showConfirm(tf('revoke_token_confirm', { name: tname }), { title: t('revoke_token'), okText: t('revoke') })) return;
          try {
            await this.client.deleteToken(tid);
            await this.loadPersonalAccessTokens();
          } catch (err) {
            showToast(t('fail_revoke_token', 'Failed to revoke token: ') + err.message, 'error');
          }
        });
      });
    } catch (err) {
      listEl.innerHTML = `<div class="subtle mini" style="color:var(--danger)">${tfHtml('failed_load_tokens', { error: err.message })}</div>`;
    }
  }

  async createPersonalAccessToken() {
    const nameInp = document.getElementById('newTokenName');
    const roleSel = document.getElementById('newTokenRole');
    const box = document.getElementById('newlyCreatedTokenBox');
    const outInp = document.getElementById('newlyCreatedTokenInput');
    const name = nameInp?.value.trim() || 'Personal Access Token';  // i18n-ok: default token name stored on the Hub
    const role = roleSel?.value || 'admin';

    try {
      const res = await this.client.createToken(name, role);
      if (res && res.ok && res.token) {
        if (outInp) outInp.value = res.token;
        if (box) box.style.display = 'block';
        if (nameInp) nameInp.value = '';
        await this.loadPersonalAccessTokens();
      }
    } catch (err) {
      showToast(t('fail_create_token', 'Failed to create token: ') + err.message, 'error');
    }
  }

  initAuthUI() {
    const modal = document.getElementById('loginModalOverlay');
    const form = document.getElementById('loginForm');
    const btnClose = document.getElementById('btnLoginClose');
    const btnSession = document.getElementById('btnAuthSession');
    const errBox = document.getElementById('loginErrorMsg');

    window.addEventListener('harmony:unauthorized', () => {
      this.showLoginModal();
    });

    if (btnClose) {
      btnClose.addEventListener('click', () => {
        if (modal) modal.style.display = 'none';
      });
    }

    modal?.addEventListener('click', (e) => {
      if (e.target === modal) {
        modal.style.display = 'none';
      }
    });

    if (form) {
      form.addEventListener('submit', async (e) => {
        e.preventDefault();
        const u = document.getElementById('loginUsername')?.value.trim();
        const p = document.getElementById('loginPassword')?.value;
        if (errBox) errBox.style.display = 'none';
        try {
          const res = await this.client.login(u, p);
          if (this.client.isNativeApp && this.client.hub && this.client.hub.id) this.activeHub = this.client.hub;
          if (res && res.old_token_kept) showToast(t('app_token_not_deleted', 'The previous login could not be deleted on the Hub: an admin can delete it in the token list.'), 'warning', 6000);
          if (modal) modal.style.display = 'none';
          const pInp = document.getElementById('loginPassword');
          if (pInp) pInp.value = '';
          this.recordSuccess();
          this.setHubStatus(true);
          await this.checkAuthStatus();
          await this.showSwitchCheck();
          await this.refreshData();
          if (this.currentTab === 'system') {
            await this.loadUserAccounts();
          }
        } catch (err) {
          if (errBox) {
            errBox.textContent = err.message || t('invalid_username_password');
            errBox.style.display = 'block';
          }
        }
      });
    }

    if (btnSession) {
      btnSession.addEventListener('click', async () => {
        if (this.client?.token) {
          if (await showConfirm(t('logout_confirm'), { title: t('logout'), okText: t('logout'), danger: false })) {
            const res = await this.client.logout();
            if (this.client.isNativeApp && this.client.hub && this.client.hub.id) this.activeHub = this.client.hub;
            if (res && res.revoked === false) showToast(t('app_token_not_deleted', 'The previous login could not be deleted on the Hub: an admin can delete it in the token list.'), 'warning', 6000);
            this.checkAuthStatus();
            await this.refreshData();
            this.showLoginModal();
          }
        } else {
          this.showLoginModal();
        }
      });
    }

    const btnCreatePat = document.getElementById('btnCreateToken');
    if (btnCreatePat) {
      btnCreatePat.addEventListener('click', () => this.createPersonalAccessToken());
    }

    const btnCopyPat = document.getElementById('btnCopyNewToken');
    if (btnCopyPat) {
      btnCopyPat.addEventListener('click', async () => {
        const inp = document.getElementById('newlyCreatedTokenInput');
        if (inp && inp.value) {
          const ok = await copyText(inp.value);
          btnCopyPat.textContent = ok ? t('copied_excl') : t('select_copy_by_hand');
          if (!ok) {
            inp.focus();
            inp.select();
          }
          setTimeout(() => { btnCopyPat.textContent = t('copy_btn'); }, ok ? 2000 : 4000);
        }
      });
    }

    this.checkAuthStatus();
  }

  showLoginModal() {
    const modal = document.getElementById('loginModalOverlay');
    const errBox = document.getElementById('loginErrorMsg');
    if (errBox) errBox.style.display = 'none';
    if (modal) {
      modal.style.display = 'flex';
      const u = document.getElementById('loginUsername');
      if (u && !u.value) {
        setTimeout(() => u.focus(), 60);
      } else {
        const p = document.getElementById('loginPassword');
        if (p) setTimeout(() => p.focus(), 60);
      }
    }
  }

  async checkAuthStatus() {
    const btnSession = document.getElementById('btnAuthSession');
    const userDisplay = document.getElementById('authUserDisplay');
    const btnAction = document.getElementById('btnAuthAction');
    if (!this.client || typeof this.client.getAuthMe !== 'function') return;

    try {
      const me = await this.client.getAuthMe();
      if (me && me.auth_enabled) {
        this.authEnabled = true;
        if (btnSession) btnSession.style.display = 'inline-flex';
        if (userDisplay) userDisplay.textContent = me.username || t('role_user');
        if (btnAction) btnAction.textContent = this.client.token ? t('logout') : t('login_action');
        const hasToken = Boolean(this.client.token);
        // the app's token reports itself as "pat:<token name>": show the account it belongs to
        if (userDisplay && hasToken && (me.username || '').startsWith('pat:') && this.client.hub?.username) userDisplay.textContent = this.client.hub.username;
        if (!hasToken) {
          this.showLoginModal();
        } else {
          const modal = document.getElementById('loginModalOverlay');
          if (modal) modal.style.display = 'none';
        }
      } else {
        this.authEnabled = false;
        if (btnSession) btnSession.style.display = 'none';
        const modal = document.getElementById('loginModalOverlay');
        if (modal) modal.style.display = 'none';
      }
    } catch (e) {
      if (e?.status === 401 || (e?.message && (e.message.includes('Authentication') || e.message.includes('401')))) {
        this.authEnabled = true;
        if (btnSession) btnSession.style.display = 'inline-flex';
        if (userDisplay) userDisplay.textContent = t('auth_required_short');
        if (btnAction) btnAction.textContent = t('login_action');
        this.showLoginModal();
      }
    }
  }

}

// Adds each token's owner (the account it belongs to and ends with) to its row in the token list
export function showTokenOwners(listEl, tokens) {
  listEl.querySelectorAll('.btn-del-pat').forEach(btn => {
    const tok = tokens.find(x => x.id === btn.getAttribute('data-id'));
    const meta = btn.parentNode && btn.parentNode.querySelector('.mini.muted');
    if (!tok || !meta) return;
    const span = document.createElement('span');
    span.textContent = ` • ${tf('token_owner', { owner: tok.owner || '-' }, 'Owner: {owner}')}`;
    meta.appendChild(span);
  });
}
