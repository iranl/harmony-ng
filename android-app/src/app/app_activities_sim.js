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
      summaryEl.innerHTML = '<strong>Already on this activity</strong>: No transition required.';
      timelineEl.innerHTML = '<div class="muted mini">No operations required.</div>';
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
        ...(act.startSteps || act.startSequence || []),
        ...(act.stopSteps || act.stopSequence || [])
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
    const fromStop = (fromAct?.stopSteps || fromAct?.stopSequence || []).map(s => ({
      ...s,
      type: s.type || s.Type || 'Command',
      command: s.command || s.Command || s.CommandName || '',
      delay: s.delay !== undefined ? s.delay : (s.delay_ms !== undefined ? s.delay_ms : (s.delayMs !== undefined ? s.delayMs : (s.Delay !== undefined ? s.Delay : 0)))
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
        phase: `Exit ${fromAct?.name || 'Leaving'}`
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
        phase: 'Leaving Stop Handover'
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
            source: `Stop tail ${fromAct?.name || ''}`
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
            phase: `Power Off ${dev.name || dev.id}`
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
            phase: `Power Off ${dev.name || dev.id}`
          });
          tMain += dl;
        }
      }
    });

    // 3. Power ON arriving devices (not present in old activity)
    const enteringDevs = enteringDevIds
      .map(id => allDevs.find(d => String(d.id) === String(id)))
      .filter(d => d && !d.is_power_always_on && !d.isPowerAlwaysOn);

    const toStartRaw = (toAct?.startSteps || toAct?.startSequence || []).map(s => ({
      ...s,
      type: s.type || s.Type || 'Command',
      command: s.command || s.Command || s.CommandName || '',
      delay: s.delay !== undefined ? s.delay : (s.delay_ms !== undefined ? s.delay_ms : (s.delayMs !== undefined ? s.delayMs : (s.Delay !== undefined ? s.Delay : 0)))
    }));

    let maxWarmup = 0;
    enteringDevs.forEach(dev => {
      const inStartSeq = toStartRaw.some(s => {
        if (String(s.deviceId) !== String(dev.id)) return false;
        const c = (s.command || s.Command || s.CommandName || '').toLowerCase();
        return c === 'poweron' || c === 'powertoggle' || c === 'power';
      });
      if (inStartSeq) return;

      const pOnSeq = dev.powerOnSequence || dev.power_on_sequence || dev.PowerOnActions || [];
      if (Array.isArray(pOnSeq) && pOnSeq.length > 0) {
        pOnSeq.forEach(s => {
          const isDl = (s.type || '').toLowerCase() === 'delay';
          const del = s.delay !== undefined ? s.delay : (s.delayMs || 0);
          const dl = isDl ? (del || 50) : (del > 0 ? del : (dev.inter_device_delay || dev.interDeviceDelay || 250));
          mainSteps.push({
            ...s,
            deviceId: s.deviceId || dev.id,
            type: isDl ? 'Delay' : 'Command',
            delay: dl,
            startT: tMain,
            phase: `Power On ${dev.name || dev.id}`
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
          const dl = dev.inter_device_delay || dev.interDeviceDelay || 250;
          mainSteps.push({
            type: 'Command',
            deviceId: dev.id,
            command: pwrCmd.name || pwrCmd,
            delay: dl,
            startT: tMain,
            phase: `Power On ${dev.name || dev.id}`
          });
          tMain += dl;
        }
      }
      const pwrDelay = dev.power_on_delay !== undefined ? dev.power_on_delay : (dev.powerOnDelay || 0);
      if (pwrDelay > maxWarmup) maxWarmup = pwrDelay;
    });

    if (maxWarmup > 0) {
      mainSteps.push({
        type: 'Delay',
        deviceId: '0',
        command: '',
        delay: maxWarmup,
        startT: tMain,
        phase: `Warmup (${maxWarmup} ms)`
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
        phase: `Enter ${toAct?.name || 'Target'}`
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
      phase: 'Switchpoint'
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
            source: `Start tail ${toAct?.name || ''}`
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
      const devName = dev ? (dev.name || dev.model || `Device ${st.deviceId}`) : (st.deviceId && st.deviceId !== '0' ? `Device ${st.deviceId}` : '');

      if (isSwitched) {
        const switchedSec = (switchedAtMs / 1000).toFixed(1);
        return `
          <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:10px 12px;margin:8px 0;background:rgba(34,197,94,0.15);border:1px solid rgba(34,197,94,0.4);border-left:4px solid #22c55e;border-radius:var(--radius-sm);">
            <div style="display:flex;align-items:center;gap:8px;">
              <span style="font-size:14px;">⚡</span>
              <strong style="font-size:12px;color:#22c55e;letter-spacing:0.5px;">ACTIVITY SWITCHED</strong>
              <span style="font-size:12px;color:var(--text-muted);">&bull; Remote UI & App switch to "${toAct ? toAct.name : 'PowerOff'}"</span>
            </div>
            <span class="badge ok" style="font-weight:700;font-size:11px;">T: ${switchedSec}s (${switchedAtMs.toLocaleString()} ms)</span>
          </div>
        `;
      }

      if (isSwMarker) {
        const sec = (st.startT / 1000).toFixed(1);
        return `
          <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:6px 8px;margin-bottom:4px;background:rgba(59,130,246,0.1);border-left:3px solid #3b82f6;">
            <span style="font-size:12px;color:#3b82f6;">&bull; [${st.phase}] Leaving activity switched (tail continues in background at T: ${sec}s)</span>
            <span class="badge info">Handover</span>
          </div>
        `;
      }

      actionCount++;
      const startSec = (st.startT / 1000).toFixed(1);
      const endSec = ((st.startT + st.delay) / 1000).toFixed(1);

      if (isDelay) {
        return `
          <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:6px 8px;margin-bottom:4px;">
            <span style="font-size:12px;">${actionCount}. [${st.phase}] Wait ${st.delay} ms (T: ${startSec}s &rarr; ${endSec}s)</span>
            <span class="badge warn">Delay</span>
          </div>
        `;
      }

      return `
        <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:6px 8px;margin-bottom:4px;">
          <span style="font-size:12px;">${actionCount}. [${st.phase}] Send <strong>${st.command}</strong> to ${devName} (${st.delay} ms, T: ${startSec}s)</span>
          <span class="badge ok">${st.type}</span>
        </div>
      `;
    };

    let renderedTimeline = mainSteps.map(renderMainRow).join('');

    if (bgSteps.length > 0) {
      renderedTimeline += `
        <div style="margin:14px 0 6px 0;padding:6px 10px;background:rgba(147,51,234,0.1);border-left:3px solid #9333ea;border-radius:var(--radius-sm);display:flex;justify-content:space-between;align-items:center;">
          <span style="font-size:12px;font-weight:700;color:#c084fc;">DEFERRED BACKGROUND QUEUE (executing concurrently)</span>
          <span class="badge" style="background:#9333ea;color:#fff;">${bgSteps.length} background command(s)</span>
        </div>
      `;
      renderedTimeline += bgSteps.map((bg) => {
        actionCount++;
        const targetSec = (bg.targetMs / 1000).toFixed(1);
        const dev = allDevs.find(d => String(d.id) === String(bg.deviceId));
        const devName = dev ? (dev.name || dev.model || `Device ${bg.deviceId}`) : (bg.deviceId && bg.deviceId !== '0' ? `Device ${bg.deviceId}` : '');
        return `
          <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:6px 8px;margin-bottom:4px;background:rgba(255,255,255,0.02);">
            <span style="font-size:12px;">${actionCount}. [${bg.source}] Send <strong>${bg.command}</strong> to ${devName} (T: ${targetSec}s, ${bg.targetMs.toLocaleString()} ms)</span>
            <span class="badge" style="background:rgba(147,51,234,0.25);color:#d8b4fe;border:1px solid rgba(147,51,234,0.4);">Deferred</span>
          </div>
        `;
      }).join('');
    }

    const switchedSec = (switchedAtMs / 1000).toFixed(1);
    const trueEndSec = (trueEndMs / 1000).toFixed(1);

    summaryEl.innerHTML = `
      <strong>${fromAct ? fromAct.name : 'PowerOff'} &rarr; ${toAct ? toAct.name : 'PowerOff'}</strong>: 
      ${actionCount} steps &bull; 
      <span style="color:#22c55e;font-weight:700;">Activity Switched at: ${switchedSec}s (${switchedAtMs.toLocaleString()} ms)</span> &bull; 
      <span style="color:var(--text);font-weight:600;">True End: ${trueEndSec}s (${trueEndMs.toLocaleString()} ms) (all enqueued sent)</span>
    `;
    timelineEl.innerHTML = renderedTimeline;
  }

}
