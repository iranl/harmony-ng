// Which activity entry of remote_slotN_map.json the Hub uses for a key the running activity
// does not bind (or any key with everything off): the "-1" entry (all off), then "default",
// then the map's only activity entry when the map has exactly one (empty entries do not count).
// With several entries and neither "-1" nor "default" the Hub sends nothing for such a key.
// Same order as fallback_activity_obj() in payload/source/rf_keymap.c, also for a hand-edited
// map: a "-1" or "default" present but not an object (null) stops the search there.
const isEntry = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);

export function defaultBindings(acts) {
  if (!isEntry(acts)) return undefined;
  if ('-1' in acts) return isEntry(acts['-1']) ? acts['-1'] : undefined;
  if ('default' in acts) return isEntry(acts.default) ? acts.default : undefined;
  const entries = Object.values(acts).filter((v) => isEntry(v) && Object.keys(v).length > 0);
  return entries.length === 1 ? entries[0] : undefined;
}

// The fallback binding of one key: a key bound to anything that is not an object (null) in the
// fallback entry binds nothing, as on the Hub
export function defaultBinding(acts, keyId) {
  const b = defaultBindings(acts)?.[keyId];
  return isEntry(b) ? b : undefined;
}
