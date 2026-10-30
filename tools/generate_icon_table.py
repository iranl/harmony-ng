#!/usr/bin/env python3
"""
Generate stock icon index table header and C source for Harmony Elite and Hub.
Maps all ~1,467 stock PNG assets in Elite firmware squashfs to 16-bit IDs (1..N).
"""

import os
import glob
import json
import pathlib

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SQUASH_ROOT = REPO_ROOT / "firmware" / "elite_116_extracted" / "squashfs-root"
HEADER_OUT = REPO_ROOT / "payload" / "source" / "elite_stock_icons.h"
SOURCE_OUT = REPO_ROOT / "payload" / "source" / "elite_stock_icons.c"

ALIASES = {
    "watchtv": "tv.png",
    "watchtv.png": "tv.png",
    "smarttv": "tv.png",
    "smarttv.png": "tv.png",
    "movie": "bab09c4a-e61d-4284-b1f3-84135f562217_small.png",
    "movie.png": "bab09c4a-e61d-4284-b1f3-84135f562217_small.png",
    "music": "72d0aab4-2ed8-4644-84d2-0031bdae356e_small.png",
    "music.png": "72d0aab4-2ed8-4644-84d2-0031bdae356e_small.png",
    "game": "dec07396-b858-47e8-ae50-4ab346af1d98_small.png",
    "game.png": "dec07396-b858-47e8-ae50-4ab346af1d98_small.png",
    "ps": "ps.png",
    "xbox": "xbox.png",
    "switch": "9d24eb35-468b-4148-a6a2-bb764aa9609c_small.png",
    "shield": "shield.png",
    "apple": "0f40f257-68c8-4f24-9561-3cb3441da395_small.png",
    "apple.png": "0f40f257-68c8-4f24-9561-3cb3441da395_small.png",
    "roku": "6ebd03b8-c815-48be-adbb-d36e06caac5e_small.png",
    "roku.png": "6ebd03b8-c815-48be-adbb-d36e06caac5e_small.png",
    "firetv": "firetv.png",
    "pc": "468668ab-2587-4662-9e89-cf66bd70f314_small.png",
    "pc.png": "468668ab-2587-4662-9e89-cf66bd70f314_small.png",
    "sonos": "sonos.png",
    "receiver": "heos.png",
    "receiver.png": "heos.png",
    "light": "bulb.png",
    "light.png": "bulb.png",
    "power": "116.png",
    "power.png": "116.png",
    "poweroff": "116.png",
    "poweroff.png": "116.png",
    "generic": "default.png",
    "generic.png": "default.png",
}

def main():
    if not SQUASH_ROOT.is_dir():
        print(f"Error: squashfs root not found at {SQUASH_ROOT}")
        return 1

    pattern = str(SQUASH_ROOT / "opt" / "luadraws" / "**" / "*.png")
    all_files = sorted(set(glob.glob(pattern, recursive=True)))
    print(f"[*] Found {len(all_files)} total stock PNG files in firmware")

    # Priority order for selecting the canonical path for duplicate filenames:
    # 1. appscommon/asset/juniperDevice
    # 2. appscommon/asset/juniperactivity
    # 3. appscommon/asset/juniperHomeAutomation
    # 4. appscommon/asset/commands
    # 5. appscommon/asset
    # 6. others
    def path_priority(p):
        norm = p.replace("\\", "/")
        if "juniperDevice" in norm: return 10
        if "juniperactivity" in norm: return 9
        if "juniperHomeAutomation" in norm: return 8
        if "commands" in norm: return 7
        if "appscommon/asset" in norm: return 6
        if "hops" in norm: return 5
        return 1

    unique_icons = {}
    for f in all_files:
        rel = "/" + os.path.relpath(f, str(SQUASH_ROOT)).replace("\\", "/")
        base = os.path.basename(f).lower()
        if base not in unique_icons:
            unique_icons[base] = rel
        else:
            if path_priority(rel) > path_priority(unique_icons[base]):
                unique_icons[base] = rel

    sorted_bases = sorted(unique_icons.keys())
    print(f"[*] Indexed {len(sorted_bases)} canonical stock icon basenames")

    # Assign IDs 1..N
    base_to_id = {b: i + 1 for i, b in enumerate(sorted_bases)}

    # Build search lookup keys:
    # - exact basename (lowercase)
    # - basename without .png extension
    # - aliases
    lookup_entries = {}
    for b in sorted_bases:
        lookup_entries[b] = base_to_id[b]
        if b.endswith(".png"):
            no_ext = b[:-4]
            if no_ext not in lookup_entries:
                lookup_entries[no_ext] = base_to_id[b]

    # Add aliases
    for alias, target in ALIASES.items():
        t_low = target.lower()
        if t_low in base_to_id:
            lookup_entries[alias.lower()] = base_to_id[t_low]
        elif t_low.endswith(".png") and t_low[:-4] in lookup_entries:
            lookup_entries[alias.lower()] = lookup_entries[t_low[:-4]]

    sorted_lookup = sorted(lookup_entries.items(), key=lambda x: x[0])
    print(f"[*] Generated {len(sorted_lookup)} total search lookup keys")

    # Generate Header
    with open(HEADER_OUT, "w", encoding="utf-8") as h:
        h.write(f"""#ifndef ELITE_STOCK_ICONS_H
#define ELITE_STOCK_ICONS_H

#include <stdint.h>
#include <stddef.h>

#define TOTAL_STOCK_ICONS {len(sorted_bases)}

/* Return stock ROM path on Elite remote for stock_id (1..TOTAL_STOCK_ICONS), or NULL */
const char *get_stock_icon_path(uint16_t stock_id);

/* Look up stock icon ID (1..TOTAL_STOCK_ICONS) from icon string/GUID/alias/filename.
   Returns 0 if not a recognized stock icon. */
uint16_t lookup_stock_icon_id(const char *name_or_guid);

#endif /* ELITE_STOCK_ICONS_H */
""")
    print(f"[+] Wrote header: {HEADER_OUT}")

    # Generate C Source
    with open(SOURCE_OUT, "w", encoding="utf-8") as c:
        c.write(f"""#include "elite_stock_icons.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static const char *const s_stock_icon_paths[{len(sorted_bases) + 1}] = {{
    NULL, /* 0: None / invalid */
""")
        for b in sorted_bases:
            c.write(f'    "{unique_icons[b]}",\n')
        c.write("""};

const char *get_stock_icon_path(uint16_t stock_id) {
    if (stock_id >= 1 && stock_id <= TOTAL_STOCK_ICONS) {
        return s_stock_icon_paths[stock_id];
    }
    return NULL;
}

typedef struct {
    const char *key;
    uint16_t id;
} icon_lookup_t;

static const icon_lookup_t s_icon_lookup[] = {
""")
        for k, v in sorted_lookup:
            c.write(f'    {{ "{k}", {v} }},\n')
        c.write(f"""}};

static int icon_lookup_cmp(const void *key_ptr, const void *elem_ptr) {{
    const char *key = (const char *)key_ptr;
    const icon_lookup_t *elem = (const icon_lookup_t *)elem_ptr;
    return strcmp(key, elem->key);
}}

uint16_t lookup_stock_icon_id(const char *name_or_guid) {{
    if (!name_or_guid || !name_or_guid[0]) return 0;

    const char *slash = strrchr(name_or_guid, '/');
    const char *base = slash ? slash + 1 : name_or_guid;

    char low[80] = {{0}};
    for (int i = 0; base[i] && i < 79; i++) {{
        low[i] = (char)tolower((unsigned char)base[i]);
    }}

    const icon_lookup_t *match = (const icon_lookup_t *)bsearch(
        low,
        s_icon_lookup,
        sizeof(s_icon_lookup) / sizeof(s_icon_lookup[0]),
        sizeof(s_icon_lookup[0]),
        icon_lookup_cmp
    );

    if (match) return match->id;

    /* If has .png, try stripping it */
    char *dot = strrchr(low, '.');
    if (dot && strcmp(dot, ".png") == 0) {{
        *dot = '\\0';
        match = (const icon_lookup_t *)bsearch(
            low,
            s_icon_lookup,
            sizeof(s_icon_lookup) / sizeof(s_icon_lookup[0]),
            sizeof(s_icon_lookup[0]),
            icon_lookup_cmp
        );
        if (match) return match->id;
    }}

    return 0;
}}
""")
    print(f"[+] Wrote source: {SOURCE_OUT}")
    return 0

if __name__ == "__main__":
    import sys
    sys.exit(main())
