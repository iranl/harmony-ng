#ifndef HARMONY_REMOTE_SLOTS_H
#define HARMONY_REMOTE_SLOTS_H

/* RF remote slots of the Hub's CC2544, 1-based. Its pipe table has 6 entries (pipes 0x20..0x25,
 * 0x30..0x35, 0x40..0x45), and stock hal_hub (libhal_get_paired_device_info) reads paired devices
 * 1 to 6. Used by hub_rf and hub_daemon. */
#define REMOTE_SLOT_MAX 6

#endif /* HARMONY_REMOTE_SLOTS_H */
