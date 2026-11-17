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
import { HarmonyAppActivitiesEditor } from './app_activities_editor.js';

export class HarmonyAppActivitiesSim extends HarmonyAppActivitiesEditor {
  simulateActivityTransition() {
    const fromId = String(document.getElementById('previewFromAct')?.value || '-1');
    const toId = String(document.getElementById('previewToAct')?.value || '-1');
    const resultsBox = document.getElementById('previewResultsBox');
    const summaryEl = document.getElementById('previewSummary');
    const timelineEl = document.getElementById('previewTimeline');

    if (!resultsBox || !summaryEl || !timelineEl) return;

    if (fromId === toId) {
      resultsBox.style.display = 'block';
      summaryEl.innerHTML = `<strong>${t('sim_already_on_activity')}</strong>: ${t('sim_no_transition_required')}`;
      timelineEl.innerHTML = `<div class="muted mini">${t('no_operations_required')}</div>`;
      return;
    }

    const allActs = this.activitiesData.activities || [];
    const fromAct = allActs.find(a => String(a.id) === fromId) || (fromId === '-1' ? { id: '-1', name: 'PowerOff' } : null);
    const toAct = allActs.find(a => String(a.id) === toId) || (toId === '-1' ? { id: '-1', name: 'PowerOff' } : null);
    const allDevs = this.inventory.devices || [];

    const getActDevIds = (act) => {
      if (!act) return [];
      const set = new Set();
      if (Array.isArray(act.deviceIds)) {
        act.deviceIds.forEach(id => set.add(String(id)));
      }
      if (Array.isArray(act.participatingDeviceIds)) {
        act.participatingDeviceIds.forEach(id => set.add(String(id)));
      }
      const seq = [
        ...(act.startSequence || []),
        ...(act.stopSequence || [])
      ];
      seq.forEach(s => {
        if (s.deviceId && String(s.deviceId) !== '0') set.add(String(s.deviceId));
      });
      return Array.from(set);
    };

    const fromDevIds = getActDevIds(fromAct);
    const toDevIds = getActDevIds(toAct);

    const leavingDevIds = fromDevIds.filter(id => !toDevIds.includes(id));
    const enteringDevIds = toDevIds.filter(id => !fromDevIds.includes(id));

    const mainSteps = [];
    const bgSteps = [];
    let tMain = 0;
    let tStep1Switched = 0;
    let actionCount = 0;

    // 1. Stop sequence of leaving activity (PowerOff is a regular activity)
    const fromStop = (fromAct?.stopSequence || []).map(s => ({
      ...s,
      type: s.type || 'Command',
      command: s.command || '',
      delay: s.delay !== undefined ? s.delay : 0
    }));

    const stopSwitchedIdx = fromStop.findIndex(s => {
      const t = (s.type || '').toLowerCase();
      const c = (s.command || '').toLowerCase();
      return t === 'switched' || t === 'activityswitched' || c === 'switched';
    });

    const stopHeadLimit = (stopSwitchedIdx >= 0) ? stopSwitchedIdx : fromStop.length;
    for (let k = 0; k < stopHeadLimit; k++) {
      const s = fromStop[k];
      const isDl = (s.type || '').toLowerCase() === 'delay';
      const dl = isDl ? (s.delay || 50) : (s.delay > 0 ? s.delay : 250);
      mainSteps.push({
        ...s,
        type: isDl ? 'Delay' : 'Command',
        delay: dl,
        startT: tMain,
        phase: tf('sim_phase_exit', { name: fromAct?.name || t('sim_phase_leaving') })
      });
      tMain += dl;
    }

    if (stopSwitchedIdx >= 0) {
      tStep1Switched = tMain;
      mainSteps.push({
        type: 'SwitchedMarker',
        command: 'Switched',
        delay: 0,
        startT: tMain,
        phase: t('sim_phase_leaving_handover')
      });

      let curOffset = 0;
      for (let k = stopSwitchedIdx + 1; k < fromStop.length; k++) {
        const s = fromStop[k];
        const isDl = (s.type || '').toLowerCase() === 'delay';
        const dl = isDl ? (s.delay || 50) : (s.delay > 0 ? s.delay : 250);
        if (isDl) {
          curOffset += dl;
        } else {
          bgSteps.push({
            ...s,
            type: 'Command',
            delay: dl,
            relOffset: curOffset,
            anchor: 1,
            source: tf('sim_source_stop_tail', { name: fromAct?.name || '' })
          });
          curOffset += dl;
        }
      }
    }

    // 2. Power OFF departing devices (not present in new activity)
    const departingDevs = leavingDevIds
      .map(id => allDevs.find(d => String(d.id) === String(id)))
      .filter(d => d && !d.is_power_always_on && !d.isPowerAlwaysOn);

    departingDevs.reverse().forEach(dev => {
      const pOffSeq = dev.powerOffSequence || dev.power_off_sequence || dev.PowerOffActions || [];
      if (Array.isArray(pOffSeq) && pOffSeq.length > 0) {
        pOffSeq.forEach(s => {
          const isDl = (s.type || '').toLowerCase() === 'delay';
          const del = s.delay !== undefined ? s.delay : (s.delayMs || 0);
          const dl = isDl ? (del || 50) : (del > 0 ? del : (dev.inter_device_delay || dev.interDeviceDelay || 250));
          mainSteps.push({
            ...s,
            deviceId: s.deviceId || dev.id,
            type: isDl ? 'Delay' : 'Command',
            delay: dl,
            startT: tMain,
            phase: tf('sim_phase_power_off', { name: dev.name || dev.id })
          });
          tMain += dl;
        });
      } else {
        const cmds = dev.commands || this.deviceCommandsMap[dev.id] || [];
        const pwrCmd = cmds.find(c => {
          const n = (c.name || c).toLowerCase();
          return n === 'poweroff' || n === 'powertoggle' || n === 'power';
        });
        if (pwrCmd) {
          const dl = dev.inter_device_delay || dev.interDeviceDelay || 250;
          mainSteps.push({
            type: 'Command',
            deviceId: dev.id,
            command: pwrCmd.name || pwrCmd,
            delay: dl,
            startT: tMain,
            phase: tf('sim_phase_power_off', { name: dev.name || dev.id })
          });
          tMain += dl;
        }
      }
    });

    // 3. Power ON arriving devices (not present in old activity)
    const enteringDevs = enteringDevIds
      .map(id => allDevs.find(d => String(d.id) === String(id)))
      .filter(d => d && !d.isPowerAlwaysOn);

    const toStartRaw = (toAct?.startSequence || []).map(s => ({
      ...s,
      type: s.type || 'Command',
      command: s.command || '',
      delay: s.delay !== undefined ? s.delay : 0
    }));

    let maxWarmup = 0;
    enteringDevs.forEach(dev => {
      const inStartSeq = toStartRaw.some(s => {
        if (String(s.deviceId) !== String(dev.id)) return false;
        const c = (s.command || '').toLowerCase();
        return c === 'poweron' || c === 'powertoggle' || c === 'power';
      });
      if (inStartSeq) return;

      const pOnSeq = dev.powerOnSequence || [];
      if (Array.isArray(pOnSeq) && pOnSeq.length > 0) {
        pOnSeq.forEach(s => {
          const isDl = (s.type || '').toLowerCase() === 'delay';
          const del = s.delay !== undefined ? s.delay : 0;
          const dl = isDl ? (del || 50) : (del > 0 ? del : (dev.interDeviceDelay !== undefined ? dev.interDeviceDelay : 250));
          mainSteps.push({
            ...s,
            deviceId: s.deviceId || dev.id,
            type: isDl ? 'Delay' : 'Command',
            delay: dl,
            startT: tMain,
            phase: tf('sim_phase_power_on', { name: dev.name || dev.id })
          });
          tMain += dl;
        });
      } else {
        const cmds = dev.commands || this.deviceCommandsMap[dev.id] || [];
        const pwrCmd = cmds.find(c => {
          const n = (c.name || c).toLowerCase();
          return n === 'poweron' || n === 'powertoggle' || n === 'power';
        });
        if (pwrCmd) {
          const dl = dev.interDeviceDelay !== undefined ? dev.interDeviceDelay : 250;
          mainSteps.push({
            type: 'Command',
            deviceId: dev.id,
            command: pwrCmd.name || pwrCmd,
            delay: dl,
            startT: tMain,
            phase: tf('sim_phase_power_on', { name: dev.name || dev.id })
          });
          tMain += dl;
        }
      }
      const pwrDelay = dev.powerOnDelay || 0;
      if (pwrDelay > maxWarmup) maxWarmup = pwrDelay;
    });

    if (maxWarmup > 0) {
      mainSteps.push({
        type: 'Delay',
        deviceId: '0',
        command: '',
        delay: maxWarmup,
        startT: tMain,
        phase: tf('sim_phase_warmup', { ms: maxWarmup })
      });
      tMain += maxWarmup;
    }

    // 4. Start sequence of arriving activity
    const startSwitchedIdx = toStartRaw.findIndex(s => {
      const t = (s.type || '').toLowerCase();
      const c = (s.command || '').toLowerCase();
      return t === 'switched' || t === 'activityswitched' || c === 'switched';
    });

    const startHeadLimit = (startSwitchedIdx >= 0) ? startSwitchedIdx : toStartRaw.length;
    for (let k = 0; k < startHeadLimit; k++) {
      const s = toStartRaw[k];
      const isDl = (s.type || '').toLowerCase() === 'delay';
      const dl = isDl ? (s.delay || 50) : (s.delay > 0 ? s.delay : 250);
      mainSteps.push({
        ...s,
        type: isDl ? 'Delay' : 'Command',
        delay: dl,
        startT: tMain,
        phase: tf('sim_phase_enter', { name: toAct?.name || t('sim_phase_target') })
      });
      tMain += dl;
    }

    const switchedAtMs = tMain;
    mainSteps.push({
      type: 'Switched',
      deviceId: '0',
      command: 'Switched',
      delay: 0,
      startT: switchedAtMs,
      phase: t('sim_phase_switchpoint')
    });

    if (startSwitchedIdx >= 0) {
      let curOffset = 0;
      for (let k = startSwitchedIdx + 1; k < toStartRaw.length; k++) {
        const s = toStartRaw[k];
        const isDl = (s.type || '').toLowerCase() === 'delay';
        const dl = isDl ? (s.delay || 50) : (s.delay > 0 ? s.delay : 250);
        if (isDl) {
          curOffset += dl;
        } else {
          bgSteps.push({
            ...s,
            type: 'Command',
            delay: dl,
            relOffset: curOffset,
            anchor: 4,
            source: tf('sim_source_start_tail', { name: toAct?.name || '' })
          });
          curOffset += dl;
        }
      }
    }

    // Resolve background step absolute execution times
    bgSteps.forEach(bg => {
      const base = (bg.anchor === 1) ? tStep1Switched : switchedAtMs;
      bg.targetMs = base + bg.relOffset;
    });

    const allEndTimes = [switchedAtMs, ...bgSteps.map(bg => bg.targetMs + bg.delay)];
    const trueEndMs = Math.max(...allEndTimes);

    resultsBox.style.display = 'block';

    const renderMainRow = (st) => {
      const isDelay = st.type === 'Delay';
      const isSwitched = st.type === 'Switched';
      const isSwMarker = st.type === 'SwitchedMarker';
      const dev = allDevs.find(d => String(d.id) === String(st.deviceId));
      const devName = dev ? (dev.name || dev.model || tf('device_n', { id: st.deviceId })) : (st.deviceId && st.deviceId !== '0' ? tf('device_n', { id: st.deviceId }) : '');

      if (isSwitched) {
        const switchedSec = (switchedAtMs / 1000).toFixed(1);
        return `
          <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:10px 12px;margin:8px 0;background:rgba(34,197,94,0.15);border:1px solid rgba(34,197,94,0.4);border-left:4px solid #22c55e;border-radius:var(--radius-sm);">
            <div style="display:flex;align-items:center;gap:8px;">
              <span style="font-size:14px;">⚡</span>
              <strong style="font-size:12px;color:#22c55e;letter-spacing:0.5px;">${t('sim_activity_switched_caps')}</strong>
              <span style="font-size:12px;color:var(--text-muted);">&bull; ${escapeHtml(tf('sim_remote_app_switch_to', { name: toAct ? toAct.name : 'PowerOff' }))}</span>
            </div>
            <span class="badge ok" style="font-weight:700;font-size:11px;">T: ${switchedSec}s (${switchedAtMs.toLocaleString()} ms)</span>
          </div>
        `;
      }

      if (isSwMarker) {
        const sec = (st.startT / 1000).toFixed(1);
        return `
          <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:6px 8px;margin-bottom:4px;background:rgba(59,130,246,0.1);border-left:3px solid #3b82f6;">
            <span style="font-size:12px;color:#3b82f6;">&bull; [${escapeHtml(st.phase)}] ${tfHtml('sim_leaving_switched', { sec: sec })}</span>
            <span class="badge info">${t('sim_badge_handover')}</span>
          </div>
        `;
      }

      actionCount++;
      const startSec = (st.startT / 1000).toFixed(1);
      const endSec = ((st.startT + st.delay) / 1000).toFixed(1);

      if (isDelay) {
        return `
          <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:6px 8px;margin-bottom:4px;">
            <span style="font-size:12px;">${actionCount}. [${escapeHtml(st.phase)}] ${tfHtml('sim_wait_row', { delay: st.delay, start: startSec, end: endSec })}</span>
            <span class="badge warn">${t('step_type_delay')}</span>
          </div>
        `;
      }

      return `
        <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:6px 8px;margin-bottom:4px;">
          <span style="font-size:12px;">${actionCount}. [${escapeHtml(st.phase)}] ${tf('sim_send_row', { command: `<strong>${escapeHtml(st.command)}</strong>`, device: escapeHtml(devName), delay: st.delay, start: startSec })}</span>
          <span class="badge ok">${st.type === 'Command' ? t('step_type_command') : st.type}</span>
        </div>
      `;
    };

    let renderedTimeline = mainSteps.map(renderMainRow).join('');

    if (bgSteps.length > 0) {
      renderedTimeline += `
        <div style="margin:14px 0 6px 0;padding:6px 10px;background:rgba(147,51,234,0.1);border-left:3px solid #9333ea;border-radius:var(--radius-sm);display:flex;justify-content:space-between;align-items:center;">
          <span style="font-size:12px;font-weight:700;color:#c084fc;">${t('sim_deferred_queue')}</span>
          <span class="badge" style="background:#9333ea;color:#fff;">${tfHtml('sim_background_commands', { count: bgSteps.length })}</span>
        </div>
      `;
      renderedTimeline += bgSteps.map((bg) => {
        actionCount++;
        const targetSec = (bg.targetMs / 1000).toFixed(1);
        const dev = allDevs.find(d => String(d.id) === String(bg.deviceId));
        const devName = dev ? (dev.name || dev.model || tf('device_n', { id: bg.deviceId })) : (bg.deviceId && bg.deviceId !== '0' ? tf('device_n', { id: bg.deviceId }) : '');
        return `
          <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:6px 8px;margin-bottom:4px;background:rgba(255,255,255,0.02);">
            <span style="font-size:12px;">${actionCount}. [${escapeHtml(bg.source)}] ${tf('sim_send_bg_row', { command: `<strong>${escapeHtml(bg.command)}</strong>`, device: escapeHtml(devName), start: targetSec, ms: bg.targetMs.toLocaleString() })}</span>
            <span class="badge" style="background:rgba(147,51,234,0.25);color:#d8b4fe;border:1px solid rgba(147,51,234,0.4);">${t('sim_badge_deferred')}</span>
          </div>
        `;
      }).join('');
    }

    const switchedSec = (switchedAtMs / 1000).toFixed(1);
    const trueEndSec = (trueEndMs / 1000).toFixed(1);

    summaryEl.innerHTML = `
      <strong>${escapeHtml(fromAct ? fromAct.name : 'PowerOff')} &rarr; ${escapeHtml(toAct ? toAct.name : 'PowerOff')}</strong>: 
      ${tf('sim_steps_count', { count: actionCount })} &bull; 
      <span style="color:#22c55e;font-weight:700;">${tfHtml('sim_switched_at', { sec: switchedSec, ms: switchedAtMs.toLocaleString() })}</span> &bull; 
      <span style="color:var(--text);font-weight:600;">${tfHtml('sim_true_end', { sec: trueEndSec, ms: trueEndMs.toLocaleString() })}</span>
    `;
    timelineEl.innerHTML = renderedTimeline;
  }

}
