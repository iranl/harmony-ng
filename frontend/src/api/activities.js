// Activities API methods for Harmony Hub

// Test Live answers once the Hub has sent every step: the client waits for their delays (the Hub
// waits at most 3 s per step) and holds (at most 10 s) on top of the usual 12 s
export function testSequenceTimeoutMs(steps) {
  let ms = 12000;
  try {
    const arr = Array.isArray(steps) ? steps : JSON.parse(String(steps));
    if (Array.isArray(arr)) {
      for (const s of arr) {
        ms += 100 + Math.min(Math.max(Number(s && s.delay) || 0, 0), 3000) +
          Math.min(Math.max(Number(s && s.hold) || 0, 0), 10000);
      }
    }
  } catch (_) { /* not JSON: the usual timeout */ }
  return ms;
}

export function createActivitiesApi(client) {
  return {
    async getActivities(options = {}) {
      const [data, status] = await Promise.all([
        client.request('/api/activities', options),
        client.request('/api/status', options).catch(() => null)
      ]);
      if (!data) return { activities: [], currentId: '-1', currentName: 'PowerOff' };
      const rawActs = data.activities || (Array.isArray(data) ? data : []);
      const normalized = rawActs.map((a) => {
        const normStart = (a.startSequence || []).map((s, idx) => ({
          id: s.id !== undefined ? s.id : idx,
          type: s.type || 'Command',
          deviceId: String(s.deviceId || '0'),
          command: s.command || '',
          delay: Number(s.delay !== undefined ? s.delay : 0),
          ...(Number(s.hold) > 0 ? { hold: Number(s.hold) } : {}),
          order: s.order !== undefined ? s.order : idx
        }));
        const normStop = (a.stopSequence || []).map((s, idx) => ({
          id: s.id !== undefined ? s.id : idx,
          type: s.type || 'Command',
          deviceId: String(s.deviceId || '0'),
          command: s.command || '',
          delay: Number(s.delay !== undefined ? s.delay : 0),
          ...(Number(s.hold) > 0 ? { hold: Number(s.hold) } : {}),
          order: s.order !== undefined ? s.order : idx
        }));
        const devIds = Array.isArray(a.deviceIds) ? a.deviceIds.map(String).filter(Boolean) : [];

        return {
          ...a,
          id: String(a.id || ''),
          name: a.name || 'Activity',
          type: a.type || '',
          icon: a.icon || '',
          order: Number(a.order || 0),
          isDefault: Boolean(a.isDefault),
          deviceIds: devIds,
          startSequence: normStart,
          stopSequence: normStop
        };
      });

      const curId = String((status && (status.activity || status.currentId)) || data.currentId || data.activityId || '-1');
      const curAct = normalized.find(a => String(a.id) === curId);
      return {
        activities: normalized,
        currentId: curId,
        currentName: curAct ? curAct.name : (curId === '-1' ? 'PowerOff' : curId)
      };
    },

    async startActivity(activityId) {
      if (client.isWsConnected()) {
        try {
          const j = await client.sendWsCommand('start_activity', { id: String(activityId) });
          if (j.error && !j.ok) {
            throw new Error(j.error);
          }
          return j;
        } catch (err) {
          if (err.message && (err.message.includes('busy') || err.message.includes('Interlock'))) {
            throw err;
          }
        }
      }
      return client.postJson('/api/activity-start', { id: String(activityId) });
    },

    async stopActivity() {
      if (client.isWsConnected()) {
        try {
          const j = await client.sendWsCommand('stop_activity', {});
          if (j.error && !j.ok) {
            throw new Error(j.error);
          }
          return j;
        } catch (err) {
          if (err.message && (err.message.includes('busy') || err.message.includes('Interlock'))) {
            throw err;
          }
        }
      }
      return client.postJson('/api/activity-stop', {});
    },

    async cancelActivity() {
      if (client.isWsConnected()) {
        try {
          const j = await client.sendWsCommand('cancel_activity', {});
          if (j.ok) return j;
        } catch {
          // Fall back to HTTP
        }
      }
      return client.postJson('/api/activity-cancel', {});
    },

    async saveActivity(data) {
      return client.postJson('/api/activity-save', data);
    },

    async deleteActivity(id) {
      return client.postJson('/api/activity-delete', { id: String(id) });
    },

    async testActivitySequence(steps) {
      // JSON text: the Hub reads a sequence sent as text as JSON when it starts with [
      const text = typeof steps === 'string' ? steps : JSON.stringify(steps);
      return client.postJson('/api/activity-test-sequence', { steps: text },
        { timeout: testSequenceTimeoutMs(steps) });
    }
  };
}
