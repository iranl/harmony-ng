// BLE broadcast commands: the Hub rebroadcasts captured advertising data stored as "BLEADV:<hex>"

export const BLE_ADV_PREFIX = 'BLEADV:';
const MAX_ADV_LEN = 31;

// Flags AD structure: LE General Discoverable, BR/EDR not supported
const FLAGS_AD = '020106';
// Complete list of 16-bit / 32-bit service UUIDs
const UUID_LIST_AD_TYPE = { 4: '03', 8: '05' };

function hexByte(n) {
  return n.toString(16).padStart(2, '0').toUpperCase();
}

function structureValid(hex) {
  const len = hex.length / 2;
  if (len === 0 || len > MAX_ADV_LEN) return false;
  let pos = 0;
  while (pos < len) {
    const adLen = parseInt(hex.substr(pos * 2, 2), 16);
    if (adLen === 0) return false;
    pos += 1 + adLen;
  }
  return pos === len;
}

// A UUID list as printed by tools/ble_capture/ble_scan ("uuids=AABBCCDD,11223344,...") becomes
// flags + a service UUID list, each UUID little-endian on air
function uuidListToHex(list) {
  const uuids = list.split(/[\s,]+/).filter(Boolean).map(u => u.toUpperCase());
  const width = uuids[0]?.length;
  const adType = UUID_LIST_AD_TYPE[width];
  if (!adType || !uuids.every(u => u.length === width && /^[0-9A-F]+$/.test(u))) return null;
  const body = uuids.map(u => u.match(/../g).reverse().join('')).join('');
  return `${FLAGS_AD}${hexByte(body.length / 2 + 1)}${adType}${body}`;
}

// Accepts raw advertising data hex, a "uuids=..." line, or a bare comma-separated UUID list.
// Returns the advertising data as uppercase hex, or null if it is not valid.
export function toAdvHex(input) {
  let text = String(input || '').trim();
  if (text.toUpperCase().startsWith(BLE_ADV_PREFIX)) text = text.slice(BLE_ADV_PREFIX.length);
  const uuidMatch = text.match(/uuids=([0-9A-Fa-f,]+)/);
  let hex;
  if (uuidMatch) hex = uuidListToHex(uuidMatch[1]);
  else if (text.includes(',')) hex = uuidListToHex(text);
  else hex = text.replace(/\s+/g, '').toUpperCase();
  if (!hex || !/^([0-9A-F]{2})+$/.test(hex) || !structureValid(hex)) return null;
  return hex;
}
