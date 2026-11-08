/* Screen state for the host and WebAssembly builds of the remote renderer. */
#ifndef REMOTE_RENDER_STATE_H
#define REMOTE_RENDER_STATE_H

#include "remote_state.h"

typedef struct {
    const char *root;    /* directory laid out like the remote's filesystem */
    const char *view;    /* activities|menu|hint|link|commands|devices|device (the device at select)|settings|pairing|not-paired|sync|help-wait|help-question|help-done */
    const char *theme;   /* theme name (a directory under /data/themes), or "classic" */
    const char *layout;  /* home layout: list, grid or carousel; the theme's when unset or "theme" */
    const char *icons;   /* activity icon directory on the remote's filesystem, or "classic" */
    const char *active;  /* running activity ID */
    int select;
    int battery;
    bool usb;
    bool charging;
    bool no_hub;
    int link_loss;       /* percent of link reports and Hub replies lost; 0 shows four bars */
    bool hub_silent;     /* the link is up but the Hub doesn't answer */
    int sync_done, sync_total;
    int minutes;         /* clock shown in the status bar, minutes after midnight */
    int scroll;          /* home pages or a list dragged this many pixels (carousel: sideways); positive shows the previous page or rows */
    int list_top;        /* with has_list_top: a list scrolled this many pixels down, within its ends */
    bool has_list_top;
} render_options_t;

/* Loads config and assets from o->root and sets up the state o describes */
void render_load(const render_options_t *o);
/* All activity icon keywords on one screen, drawn with the firmware's primitives */
void render_keyword_sheet(const char *title, const char *icons);
/* Converts g_backbuffer to 8-bit RGB (channels 3) or RGBA (channels 4) at (ox, oy) of a canvas cw wide */
void render_copy_out(uint8_t *canvas, int cw, int ox, int oy, int channels);

#endif
