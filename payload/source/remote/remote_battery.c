#include "remote_battery.h"

/* (mV, %) on battery, rising; linear between the points */
static const struct { int mv; int pct; } k_curve[] = {
    { 3550, 0 },   /* stock shutdown */
    { 3600, 8 },
    { 3650, 18 },
    { 3700, 30 },  /* stock icon: low below */
    { 3750, 45 },
    { 3800, 60 },
    { 3850, 75 },  /* stock icon: full from here */
    { 3900, 90 },  /* stock factory test: fully charged from 3.9 V */
    { 3950, 100 },
};

int remote_battery_pct_of_mv(int mv) {
    const int n = (int)(sizeof(k_curve) / sizeof(k_curve[0]));
    if (mv <= k_curve[0].mv) return 0;
    if (mv >= k_curve[n - 1].mv) return 100;
    for (int i = 1; i < n; i++) {
        if (mv <= k_curve[i].mv) {
            int x0 = k_curve[i - 1].mv, x1 = k_curve[i].mv;
            int y0 = k_curve[i - 1].pct, y1 = k_curve[i].pct;
            return y0 + (mv - x0) * (y1 - y0) / (x1 - x0);
        }
    }
    return 100;
}

remote_batt_report_t g_batt_report = { false, 0, -1, -1, false };

void remote_battery_request_report(remote_batt_report_t *r) {
    r->requested = true;
}

bool remote_battery_report_due(remote_batt_report_t *r, uint64_t now_ms, int pct, bool charging, bool screen_on) {
    bool since_ok = r->sent;
    bool interval = !since_ok || now_ms - r->last_ms >= REMOTE_BATT_REPORT_INTERVAL_MS;
    bool chg_changed = since_ok && (int)charging != r->last_chg;
    int d = pct - r->last_pct;
    if (d < 0) d = -d;
    bool change_due = since_ok && screen_on && d >= 2 && now_ms - r->last_ms >= REMOTE_BATT_REPORT_CHANGE_MS;
    if (!(interval || chg_changed || change_due || r->requested)) return false;
    r->sent = true;
    r->requested = false;
    r->last_ms = now_ms;
    r->last_pct = pct;
    r->last_chg = (int)charging;
    return true;
}

void remote_battery_filter_init(remote_batt_filter_t *f) {
    f->pct = 0xFF;
    f->last_drop_ms = 0;
    f->last_batt_mv = 0;
    f->chg_offset_mv = REMOTE_BATT_CHARGE_OFFSET_MV;
    f->chg_known = false;
    f->chg_stable = false;
    f->chg_pending = false;
    f->chg_pending_since = 0;
}

bool remote_battery_charging(remote_batt_filter_t *f, bool raw, uint64_t now_ms, bool *changed) {
    if (changed) *changed = false;
    if (!f->chg_known) {
        f->chg_known = true;
        f->chg_stable = raw;
        f->chg_pending = raw;
        return raw;
    }
    if (raw == f->chg_stable) {
        f->chg_pending = raw; /* a bounce that came back: forget it */
        return f->chg_stable;
    }
    if (raw != f->chg_pending) {
        f->chg_pending = raw;
        f->chg_pending_since = now_ms;
    }
    if (now_ms - f->chg_pending_since >= REMOTE_BATT_CHG_DEBOUNCE_MS) {
        f->chg_stable = raw;
        if (changed) *changed = true;
    }
    return f->chg_stable;
}

uint8_t remote_battery_filter_step(remote_batt_filter_t *f, int avg_mv, bool charging,
                                  bool chg_transition, bool rested, uint64_t now_ms) {
    if (charging && chg_transition && f->last_batt_mv > 0) {
        /* the first reading with the charger on, against the last one without it */
        int off = avg_mv - f->last_batt_mv;
        if (off < REMOTE_BATT_CHARGE_OFFSET_MIN_MV) off = REMOTE_BATT_CHARGE_OFFSET_MIN_MV;
        if (off > REMOTE_BATT_CHARGE_OFFSET_MAX_MV) off = REMOTE_BATT_CHARGE_OFFSET_MAX_MV;
        f->chg_offset_mv = off;
    }
    /* only a rested reading: the ones just before a charging start may already carry the
     * charge current (the state is debounced) or the screen's load */
    if (!charging && rested) f->last_batt_mv = avg_mv;
    int pct = remote_battery_pct_of_mv(charging ? avg_mv - f->chg_offset_mv : avg_mv);

    if (f->pct == 0xFF) {
        f->pct = (uint8_t)pct;
        f->last_drop_ms = now_ms;
        return f->pct;
    }
    if (chg_transition) {
        /* taken right after the change, under the load or the charge current: keep the value */
        f->last_drop_ms = now_ms;
        return f->pct;
    }
    if (charging) {
        if (pct > f->pct) f->pct = (uint8_t)pct;
        f->last_drop_ms = now_ms;
        return f->pct;
    }
    if (rested) {
        /* the voltage has recovered: this is the charge left */
        f->pct = (uint8_t)pct;
        f->last_drop_ms = now_ms;
        return f->pct;
    }
    /* under load: never up, and down by one point per step at most */
    if (pct < f->pct) {
        if (now_ms - f->last_drop_ms >= REMOTE_BATT_DROP_STEP_MS) {
            f->pct--;
            f->last_drop_ms = now_ms;
        }
    } else {
        f->last_drop_ms = now_ms; /* the step counts from the first reading below */
    }
    return f->pct;
}
