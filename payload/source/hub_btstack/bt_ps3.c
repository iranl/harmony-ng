#include "bt_ps3.h"

#include <string.h>
#include <strings.h>

/* Report 1: 11 input bytes; feature reports 1 to 6, 11 bytes each (stock hal, offset 0x45e47) */
const uint8_t ps3_remote_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
    0x95, 0x0b, 0x06, 0x00, 0xff, 0x09, 0x01, 0x81, 0x00,
    0x85, 0x01, 0x95, 0x0b, 0x09, 0x01, 0xb1, 0x00,
    0x85, 0x02, 0x95, 0x0b, 0x09, 0x01, 0xb1, 0x00,
    0x85, 0x03, 0x95, 0x0b, 0x09, 0x01, 0xb1, 0x00,
    0x85, 0x04, 0x95, 0x0b, 0x09, 0x01, 0xb1, 0x00,
    0x85, 0x05, 0x95, 0x0b, 0x09, 0x01, 0xb1, 0x00,
    0x85, 0x06, 0x95, 0x0b, 0x09, 0x01, 0xb1, 0x00,
    0xc0
};
const size_t ps3_remote_descriptor_len = sizeof(ps3_remote_descriptor);

static const ps3_key_t s_ps3_keys[] = {
    { "1", 0x00, -1 }, { "2", 0x01, -1 }, { "3", 0x02, -1 }, { "4", 0x03, -1 }, { "5", 0x04, -1 },
    { "6", 0x05, -1 }, { "7", 0x06, -1 }, { "8", 0x07, -1 }, { "9", 0x08, -1 }, { "0", 0x09, -1 },
    { "Enter", 0x0b, 19 },
    { "Return", 0x0e, -1 },
    { "Clear", 0x0f, -1 },
    { "Eject", 0x16, -1 },
    { "TopMenu", 0x1a, -1 },
    { "Time", 0x28, -1 },
    { "PreviousTrack", 0x30, -1 },
    { "NextTrack", 0x31, -1 },
    { "Play", 0x32, -1 },
    { "Rewind", 0x33, -1 },
    { "FastForward", 0x34, -1 },
    { "Stop", 0x38, -1 },
    { "Pause", 0x39, -1 },
    { "PopUpMenu", 0x40, -1 },
    { "PS", 0x43, 16 },
    { "SelectButton", 0x50, 0 },
    { "L3", 0x51, 1 },
    { "R3", 0x52, 2 },
    { "Start", 0x53, 3 },
    { "DirectionUp", 0x54, 4 },
    { "DirectionRight", 0x55, 5 },
    { "DirectionDown", 0x56, 6 },
    { "DirectionLeft", 0x57, 7 },
    { "L2", 0x58, 8 },
    { "R2", 0x59, 9 },
    { "L1", 0x5a, 10 },
    { "R1", 0x5b, 11 },
    { "Triangle", 0x5c, 12 },
    { "Circle", 0x5d, 13 },
    { "Cross", 0x5e, 14 },
    { "Square", 0x5f, 15 },
    { "StepBack", 0x60, -1 },
    { "StepForward", 0x61, -1 },
    { "Subtitle", 0x63, -1 },
    { "Audio", 0x64, -1 },
    { "Angle", 0x65, -1 },
    { "Display", 0x70, -1 },
    { "Blue", 0x80, -1 },
    { "Red", 0x81, -1 },
    { "Green", 0x82, -1 },
    { "Yellow", 0x83, -1 },
};
#define NKEYS (sizeof(s_ps3_keys) / sizeof(s_ps3_keys[0]))

/* Other names for the same keys: the Harmony command names and the short forms */
static const struct { const char *alias; const char *name; } s_ps3_aliases[] = {
    { "PSButton", "PS" }, { "PlayStation", "PS" },
    { "Select", "Enter" }, { "OK", "Enter" },
    { "Back", "Return" },
    { "Up", "DirectionUp" }, { "Down", "DirectionDown" },
    { "Left", "DirectionLeft" }, { "Right", "DirectionRight" },
    { "Previous", "PreviousTrack" }, { "Prev", "PreviousTrack" }, { "SkipBack", "PreviousTrack" },
    { "Next", "NextTrack" }, { "SkipForward", "NextTrack" },
    { "ScanBack", "Rewind" }, { "ScanForward", "FastForward" },
    { "PopUp", "PopUpMenu" }, { "Options", "Triangle" },
    { "Info", "Display" },
    { "SlowBack", "StepBack" }, { "SlowForward", "StepForward" },
};

static const ps3_key_t *ps3_find(const char *name) {
    for (size_t i = 0; i < NKEYS; i++) {
        if (strcasecmp(s_ps3_keys[i].name, name) == 0) return &s_ps3_keys[i];
    }
    return NULL;
}

const ps3_key_t *ps3_key_by_name(const char *name) {
    if (!name || !name[0]) return NULL;
    const ps3_key_t *k = ps3_find(name);
    if (k) return k;
    if (strncasecmp(name, "Number", 6) == 0 && name[6] >= '0' && name[6] <= '9' && !name[7]) {
        char d[2] = { name[6], 0 };
        return ps3_find(d);
    }
    for (size_t i = 0; i < sizeof(s_ps3_aliases) / sizeof(s_ps3_aliases[0]); i++) {
        if (strcasecmp(s_ps3_aliases[i].alias, name) == 0) return ps3_find(s_ps3_aliases[i].name);
    }
    return NULL;
}

const ps3_key_t *ps3_key_by_code(unsigned int code) {
    for (size_t i = 0; i < NKEYS; i++) {
        if (s_ps3_keys[i].code == code) return &s_ps3_keys[i];
    }
    return NULL;
}

const ps3_key_t *ps3_key_from_keyboard(uint8_t usage) {
    if (usage >= 0x1e && usage <= 0x26) return ps3_key_by_code((unsigned int)(usage - 0x1e)); /* 1..9 */
    switch (usage) {
        case 0x27: return ps3_find("0");
        case 0x28: return ps3_find("Enter");
        case 0x29: return ps3_find("Return");      /* Escape */
        case 0x2a: return ps3_find("Return");      /* Backspace */
        case 0x4a: return ps3_find("PS");          /* Home */
        case 0x4f: return ps3_find("DirectionRight");
        case 0x50: return ps3_find("DirectionLeft");
        case 0x51: return ps3_find("DirectionDown");
        case 0x52: return ps3_find("DirectionUp");
        case 0x65: return ps3_find("PopUpMenu");   /* Application (context menu) */
        case 0x76: return ps3_find("PopUpMenu");   /* Menu */
        default: return NULL;
    }
}

const ps3_key_t *ps3_key_from_consumer(uint16_t usage) {
    switch (usage) {
        case 0x0030: return ps3_find("PS");        /* Power */
        case 0x0040: return ps3_find("PopUpMenu"); /* Menu */
        case 0x008d: return ps3_find("PopUpMenu"); /* Guide */
        case 0x0061: return ps3_find("Subtitle");  /* Closed caption */
        case 0x0069: return ps3_find("Red");
        case 0x006a: return ps3_find("Green");
        case 0x006b: return ps3_find("Blue");
        case 0x006c: return ps3_find("Yellow");
        case 0x00b0: return ps3_find("Play");
        case 0x00b1: return ps3_find("Pause");
        case 0x00b3: return ps3_find("FastForward");
        case 0x00b4: return ps3_find("Rewind");
        case 0x00b5: return ps3_find("NextTrack");
        case 0x00b6: return ps3_find("PreviousTrack");
        case 0x00b7: return ps3_find("Stop");
        case 0x00b8: return ps3_find("Eject");
        case 0x00cd: return ps3_find("Pause");     /* Play/Pause: the remote's Pause toggles */
        case 0x01bd: return ps3_find("Display");   /* Info */
        case 0x0223: return ps3_find("PS");        /* Home */
        case 0x0224: return ps3_find("Return");    /* Back */
        default: return NULL;
    }
}

void ps3_report(const ps3_key_t *key, uint8_t out[PS3_REPORT_LEN]) {
    memset(out, 0, PS3_REPORT_LEN);
    out[0] = 0xa1;
    out[1] = 0x01;
    out[5] = key ? key->code : 0xff;
    memset(out + 6, 0xff, 5);
    out[11] = key ? 0x01 : 0x00;
    out[12] = 0x05;
    if (key && key->bit >= 0) out[2 + key->bit / 8] = (uint8_t)(1u << (key->bit % 8));
}

int bt_profile_from_name(const char *name) {
    if (!name) return -1;
    if (strcasecmp(name, "keyboard") == 0) return BT_PROFILE_KEYBOARD;
    if (strcasecmp(name, "ps3") == 0) return BT_PROFILE_PS3;
    return -1;
}

const char *bt_profile_name(int profile) {
    return profile == BT_PROFILE_PS3 ? "ps3" : "keyboard";
}
