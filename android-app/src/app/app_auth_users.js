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
import { t, setLanguage, getInitialLanguage } from '../i18n.js';
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
import { HarmonyAppSettingsSystem } from './app_settings_system.js';

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

      const admin = users.find(u => u.role === 'admin');
      if (admin && adminUser) adminUser.value = admin.username;

      const nonAdmins = users.filter(u => u.role === 'user');
      if (!listEl) return;

      if (nonAdmins.length === 0) {
        listEl.innerHTML = '<div class="subtle mini" style="padding: 4px 0;">No non-admin accounts configured yet.</div>';
        return;
      }

      let html = '<div style="display: flex; flex-direction: column; gap: 6px;">';
      for (const u of nonAdmins) {
        html += `
          <div style="display: flex; align-items: center; justify-content: space-between; padding: 6px 10px; background: rgba(255,255,255,0.04); border-radius: 4px; border: 1px solid var(--border);">
            <div>
              <span style="font-weight: 600; font-size: 13px;">${u.username}</span>
              <span class="badge" style="margin-left: 8px; font-size: 10px; background: rgba(59, 130, 246, 0.15); color: var(--accent);">Restricted User</span>
            </div>
            <button type="button" class="btn btn-xs btn-danger btn-del-user" data-username="${u.username}">Delete</button>
          </div>`;
      }
      html += '</div>';
      listEl.innerHTML = html;

      listEl.querySelectorAll('.btn-del-user').forEach(btn => {
        btn.addEventListener('click', async () => {
          const uname = btn.getAttribute('data-username');
          if (!confirm(t("delete_user_confirm").replace("{name}", uname))) return;
          try {
            await this.client.deleteUser(uname);
            await this.loadUserAccounts();
          } catch (err) {
            alert(t("failed_delete_user") + err.message);
          }
        });
      });
    } catch (err) {
      if (listEl) listEl.innerHTML = `<div class="subtle mini" style="color:var(--danger)">Failed to load users: ${err.message}</div>`;
    }
  }

  async addNonAdminUser() {
    const uInp = document.getElementById('newUserName');
    const pInp = document.getElementById('newUserPassword');
    const stat = document.getElementById('newUserStatus');
    const username = uInp?.value.trim();
    const password = pInp?.value;

    if (!username || !password) {
      if (stat) stat.innerHTML = '<span style="color:var(--danger)">Username and password required</span>';
      return;
    }

    if (stat) stat.textContent = 'Adding user...';
    try {
      await this.client.saveUser({ username, password, role: 'user' });
      if (uInp) uInp.value = '';
      if (pInp) pInp.value = '';
      if (stat) stat.innerHTML = '<span style="color:var(--success)">User added!</span>';
      setTimeout(() => { if (stat) stat.textContent = ''; }, 3000);
      await this.loadUserAccounts();
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  async saveWebUiAuth() {
    const status = document.getElementById('authSaveStatus');
    const on = document.getElementById('authEnabledCheck')?.checked ? 1 : 0;
    const u = document.getElementById('authUsername')?.value.trim();
    const p = document.getElementById('authPassword')?.value;
    try {
      await this.client.saveSystemAction('auth', { authEnabled: on, authUsername: u, authPassword: p });
      if (status) status.innerHTML = '<span style="color:var(--success)">Admin account & sign-in saved!</span>';
      await this.loadUserAccounts();
      this.checkAuthStatus();
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  async loadPersonalAccessTokens() {
    const listEl = document.getElementById('tokensListContainer');
    if (!this.client || typeof this.client.getTokens !== 'function' || !listEl) return;

    try {
      const data = await this.client.getTokens();
      if (!data || !data.ok) {
        listEl.innerHTML = '<div class="subtle mini">Unable to retrieve tokens.</div>';
        return;
      }
      const tokens = data.tokens || [];
      if (tokens.length === 0) {
        listEl.innerHTML = '<div class="subtle mini" style="padding: 4px 0;">No Personal Access Tokens generated yet.</div>';
        return;
      }

      let html = '<div style="display: flex; flex-direction: column; gap: 6px;">';
      for (const t of tokens) {
        const createdDate = t.created_at ? new Date(t.created_at * 1000).toLocaleDateString() : 'Unknown';
        const lastUsed = t.last_used_at ? new Date(t.last_used_at * 1000).toLocaleString() : 'Never';
        html += `
          <div style="display: flex; align-items: center; justify-content: space-between; padding: 6px 10px; background: rgba(255,255,255,0.04); border-radius: 4px; border: 1px solid var(--border);">
            <div>
              <div style="font-weight: 600; font-size: 13px;">${t.name || 'Unnamed Token'}</div>
              <div class="mini muted" style="font-size: 10px; margin-top: 2px;">
                <span>ID: ${t.id}</span> • <span>Role: ${t.role}</span> • <span>Created: ${createdDate}</span> • <span>Last used: ${lastUsed}</span>
              </div>
            </div>
            <button type="button" class="btn btn-xs btn-danger btn-del-pat" data-id="${t.id}" data-name="${t.name || t.id}">Revoke</button>
          </div>`;
      }
      html += '</div>';
      listEl.innerHTML = html;

      listEl.querySelectorAll('.btn-del-pat').forEach(btn => {
        btn.addEventListener('click', async () => {
          const tid = btn.getAttribute('data-id');
          const tname = btn.getAttribute('data-name');
          if (!confirm(`Revoke Personal Access Token "${tname}"? Third-party integrations using this token will stop working.`)) return;
          try {
            await this.client.deleteToken(tid);
            await this.loadPersonalAccessTokens();
          } catch (err) {
            alert(`Failed to revoke token: ${err.message}`);
          }
        });
      });
    } catch (err) {
      listEl.innerHTML = `<div class="subtle mini" style="color:var(--danger)">Failed to load tokens: ${err.message}</div>`;
    }
  }

  async createPersonalAccessToken() {
    const nameInp = document.getElementById('newTokenName');
    const roleSel = document.getElementById('newTokenRole');
    const box = document.getElementById('newlyCreatedTokenBox');
    const outInp = document.getElementById('newlyCreatedTokenInput');
    const name = nameInp?.value.trim() || 'Personal Access Token';
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
      alert(`Failed to create token: ${err.message}`);
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

    if (form) {
      form.addEventListener('submit', async (e) => {
        e.preventDefault();
        const u = document.getElementById('loginUsername')?.value.trim();
        const p = document.getElementById('loginPassword')?.value;
        if (errBox) errBox.style.display = 'none';
        try {
          await this.client.login(u, p);
          if (modal) modal.style.display = 'none';
          this.checkAuthStatus();
        } catch (err) {
          if (errBox) {
            errBox.textContent = err.message || 'Invalid username or password';
            errBox.style.display = 'block';
          }
        }
      });
    }

    if (btnSession) {
      btnSession.addEventListener('click', async () => {
        if (this.client?.token) {
          if (confirm('Log out from Harmony Hub?')) {
            await this.client.logout();
            this.checkAuthStatus();
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
      btnCopyPat.addEventListener('click', () => {
        const inp = document.getElementById('newlyCreatedTokenInput');
        if (inp && inp.value) {
          navigator.clipboard?.writeText(inp.value);
          btnCopyPat.textContent = 'Copied!';
          setTimeout(() => { btnCopyPat.textContent = 'Copy'; }, 2000);
        }
      });
    }

    this.checkAuthStatus();
  }

  showLoginModal() {
    const modal = document.getElementById('loginModalOverlay');
    const errBox = document.getElementById('loginErrorMsg');
    if (errBox) errBox.style.display = 'none';
    if (modal) modal.style.display = 'flex';
  }

  async checkAuthStatus() {
    const btnSession = document.getElementById('btnAuthSession');
    const userDisplay = document.getElementById('authUserDisplay');
    const btnAction = document.getElementById('btnAuthAction');
    if (!this.client || typeof this.client.getAuthMe !== 'function') return;

    try {
      const me = await this.client.getAuthMe();
      if (me && me.auth_enabled) {
        if (btnSession) btnSession.style.display = 'inline-flex';
        if (userDisplay) userDisplay.textContent = me.username || 'User';
        if (btnAction) btnAction.textContent = this.client.token ? 'Logout' : 'Login';
      } else if (btnSession) {
        btnSession.style.display = 'none';
      }
    } catch (e) {
      if (this.client?.token && btnSession) {
        btnSession.style.display = 'inline-flex';
        if (userDisplay) userDisplay.textContent = 'Auth required';
        if (btnAction) btnAction.textContent = 'Login';
      }
    }
  }

}
