// Mantra fan commands: the Hub builds each broadcast from a "MANTRA:<action>" keyCode and the
// device's fan id (codec from ha-ble-adv by @NicoIIT, see payload/source/mantra_codec.h)

export const MANTRA_PREFIX = 'MANTRA:';

// value: null for plain actions, otherwise [min, max, label]
export const MANTRA_ACTIONS = [
  { action: 'LIGHT_ON', label: 'Light on', value: null, v1: true },
  { action: 'LIGHT_OFF', label: 'Light off', value: null, v1: true },
  { action: 'BRIGHTNESS', label: 'Brightness', value: [1, 100, '%'], v1: true },
  { action: 'CT', label: 'Colour temperature', value: [0, 100, '0 cool – 100 warm'], v1: true },
  { action: 'FAN_ON', label: 'Fan on', value: null },
  { action: 'FAN_OFF', label: 'Fan off', value: null },
  { action: 'SPEED', label: 'Fan speed', value: [0, 31, '0–31'] },
  { action: 'FORWARD', label: 'Direction forward (summer)', value: null },
  { action: 'REVERSE', label: 'Direction reverse (winter)', value: null },
  { action: 'BREEZE', label: 'Breeze mode', value: null },
  { action: 'SLEEP', label: 'Sleep mode', value: null },
  { action: 'TIMER_1H', label: 'Off timer 1 h', value: null },
  { action: 'TIMER_2H', label: 'Off timer 2 h', value: null },
  { action: 'TIMER_4H', label: 'Off timer 4 h', value: null },
  { action: 'TIMER_8H', label: 'Off timer 8 h', value: null },
  { action: 'ALL_OFF', label: 'All off', value: null },
];

export function mantraActionsFor(version) {
  return MANTRA_ACTIONS.filter(a => Number(version) !== 1 || a.v1);
}

// Returns "MANTRA:ACTION[:value]", or null if the value is out of range. An action left without
// its value takes one with each send: the Elite remote shows it as a slider.
export function mantraKeyCode(action, value) {
  const def = MANTRA_ACTIONS.find(a => a.action === action);
  if (!def) return null;
  if (!def.value || String(value ?? '').trim() === '') return MANTRA_PREFIX + action;
  const n = Number(value);
  if (!Number.isInteger(n) || n < def.value[0] || n > def.value[1]) return null;
  return `${MANTRA_PREFIX}${action}:${n}`;
}

// Fan ids are 16-bit; accepts "C5F0" or "0xc5f0" and returns 4 uppercase hex digits, or null
export function normalizeMantraId(input) {
  const s = String(input || '').trim().replace(/^0x/i, '');
  if (!/^[0-9a-f]{1,4}$/i.test(s)) return null;
  return s.toUpperCase().padStart(4, '0');
}

// Commands a new Mantra device starts with; fan speeds follow the remote's six steps
export function mantraDefaultCommands(version) {
  const light = [
    ['LightOn', 'LIGHT_ON'], ['LightOff', 'LIGHT_OFF'],
    ['Brightness25', 'BRIGHTNESS:25'], ['Brightness50', 'BRIGHTNESS:50'],
    ['Brightness75', 'BRIGHTNESS:75'], ['Brightness100', 'BRIGHTNESS:100'],
    ['ColorWarm', 'CT:100'], ['ColorNeutral', 'CT:50'], ['ColorCool', 'CT:0'],
  ];
  const fan = [
    ['FanOn', 'FAN_ON'], ['FanOff', 'FAN_OFF'],
    ...[1, 2, 3, 4, 5, 6].map(n => [`FanSpeed${n}`, `SPEED:${Math.floor((n * 31) / 6)}`]),
    ['DirectionForward', 'FORWARD'], ['DirectionReverse', 'REVERSE'],
    ['Breeze', 'BREEZE'], ['Sleep', 'SLEEP'],
    ['Timer1h', 'TIMER_1H'], ['Timer2h', 'TIMER_2H'], ['Timer4h', 'TIMER_4H'], ['Timer8h', 'TIMER_8H'],
    ['AllOff', 'ALL_OFF'],
  ];
  const list = Number(version) === 1 ? light : [...light, ...fan];
  return list.map(([name, action]) => ({ name, raw: MANTRA_PREFIX + action }));
}
