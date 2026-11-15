// Which activity entry of remote_slotN_map.json the Hub uses for a key the running activity
// does not bind (or any key with everything off): the "-1" entry (all off), then "default",
// then the map's only activity entry when the map has exactly one. With several entries and
// neither "-1" nor "default" the Hub sends nothing for such a key.
// Same order as fallback_activity_obj() in payload/source/rf_keymap.c.
export function defaultBindings(acts) {
  if (!acts || typeof acts !== 'object') return undefined;
  const fixed = acts['-1'] ?? acts.default;
  if (fixed !== undefined) return fixed;
  const entries = Object.values(acts).filter((v) => v && typeof v === 'object' && !Array.isArray(v));
  return entries.length === 1 ? entries[0] : undefined;
}
