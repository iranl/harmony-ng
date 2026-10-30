/* Screen state for the host and WebAssembly builds of the Elite renderer. */
#ifndef ELITE_RENDER_STATE_H
#define ELITE_RENDER_STATE_H

#include "elite_state.h"

typedef struct {
    const char *root;    /* directory laid out like the remote's filesystem */
    const char *view;    /* activities|commands|devices|settings|pairing|not-paired|sync */
    const char *icons;   /* activity icon directory on the remote's filesystem, or "classic" */
    const char *active;  /* running activity ID */
    int select;
    int battery;
    bool usb;
    bool charging;
    bool no_hub;
    int sync_done, sync_total;
    int minutes;         /* clock shown in the status bar, minutes after midnight */
} render_options_t;

/* Loads config and assets from o->root and sets up the state o describes */
void render_load(const render_options_t *o);
/* All activity icon keywords on one screen, drawn with the firmware's primitives */
void render_keyword_sheet(const char *title, const char *icons);
/* Converts g_backbuffer to 8-bit RGB (channels 3) or RGBA (channels 4) at (ox, oy) of a canvas cw wide */
void render_copy_out(uint8_t *canvas, int cw, int ox, int oy, int channels);

#endif
