# Hub Flash Layout: Can /data and /cache Grow?

This is a study, not a procedure. The repository has no tool that changes the partition table, and none should be used without a serial console on the Hub.

**Short answer:** yes, but only by patching and reflashing the kernel and moving the root file system. The gain is about 960 KB, or about 1.4 MB at most. A USB stick for the icons and plugins (see [USB storage](#option-c-usb-stick-recommended)) frees more space (about 1.6 MB) with no flash risk.

---

## 1. The flash

The Hub (Atheros AR9330: the kernel prints `Atheros AR9330 (Hornet)`, U-Boot `AP121 (ar9331) U-boot`) has one 16 MB SPI NOR chip (`ar7240-nor0`). It has no NAND. The SPI bus is shared with the CC2544 RF chip, so the SPI rules in `AGENTS.md` apply to every flash write.

| mtd | Name | Offset | Size | Use |
| :-- | :-- | :-- | :-- | :-- |
| 0 | u-boot | `0x000000` | 64 KB | Boot loader. The table has no separate environment partition. |
| 1 | kernel | `0x010000` | 960 KB | Kernel uImage (`bootm 0x9f010000`). The stock image is 951,019 bytes, about 31 KB free. |
| 2 | kernel2 | `0x100000` | 960 KB | **Fallback kernel.** U-Boot boots it when the kernel in mtd1 fails its checksum (`bootcmd=bootm 0x9f010000 ; bootm 0x9f100000`). It holds an older stock kernel ("Pimento Kernel Image"); the stock updater never writes it. |
| 3 | root | `0x1F0000` | 4096 KB | Stock SquashFS root (3,751,936 bytes, about 432 KB slack). |
| 4 | data | `0x5F0000` | 5120 KB | JFFS2, the writable layer of the union root (`/data`, `/etc`, ...). |
| 5 | cache | `0xAF0000` | 5120 KB | JFFS2, `/cache`. |
| 6 | mfg | `0xFF0000` | 64 KB | Manufacturing data (hardware version, read by `rcS.local` and `mfg_id.c`). Must be kept. |

`docs/OFFLINE_UNBRICK_AND_ROOT_GUIDE.md` and `tests/test_factory_reset_wipe.py` use the same numbering.

### What uses the space

| /cache (5120 KB) | KB |
| :-- | --: |
| `icons.tar.gz` | about 1588 |
| `bin/busybox` | about 1400 |
| `www` (gzipped web UI) | about 290 to 474 |
| `modules/` (USB Ethernet drivers) | 0 by default (on demand since this release); 376 for all 13 |

| /data (5120 KB) | KB |
| :-- | --: |
| `harmony/bin` (`hub_daemon` 736, `hub_btstack` 375, `hub_rf` 375, `dropbearmulti` 297, `hub_util` 184) | about 2000 |
| Stock `/data/resources` (the web UI can empty it) | about 274 |
| Configuration, scripts, custom icons | varies |

JFFS2 needs free erase blocks for garbage collection. A partition that runs full slows down and can fail writes well before it reaches 100%.

---

## 2. Why bootargs cannot change it

The partition table is **built into the kernel**:

- The board code (around `0x802541ec` in the raw kernel, `scratch/vmlinux.bin`, load address `0x80002000`) prints `flash_size passed from bootloader = %d`. It replaces the command line with U-Boot's arguments, then **always appends** a `mtdparts=` string of its own.
- It picks one of three built-in tables (file offset `0x205fe0` to `0x2060a8`): 4 MB, 8 MB, or 16 MB for any other flash size.
- The built-in `cmdlinepart` parser uses the last `mtdparts=` on the line, so the kernel's own one always wins. Setting `mtdparts=` with `setenv bootargs` in U-Boot does nothing.
- The init built into the image mounts the partitions **by name** (`mtd:root`, `mtd:data`, `mtd:cache`) and builds the union `dirs=/mnt/data=rw:/mnt/root=ro`. Its factory reset formats `mtd:data` and `mtd:cache` by name too. Names, not numbers, must therefore stay the same.

Changing the layout means changing that string inside the kernel.

---

## 3. Options

### Option A: give kernel2 to data (+960 KB)

Partitions must be contiguous. kernel2 sits between kernel and root, so root moves down into kernel2's place, and data grows by 960 KB at its start:

| Name | Offset | Size |
| :-- | :-- | :-- |
| u-boot | `0x000000` | 64 KB |
| kernel | `0x010000` | 960 KB |
| root | `0x100000` | 4096 KB |
| data | `0x500000` | **6080 KB** |
| cache | `0xAF0000` | 5120 KB |
| mfg | `0xFF0000` | 64 KB |

cache keeps its place, so it does not need to be rewritten. The new data partition must be formatted, so everything on `/data` is lost and must be restored from a backup.

This gives away the fallback kernel: U-Boot's second `bootm 0x9f100000` then points into the root file system, so a kernel in mtd1 that fails its checksum (a cut flash write, as in step 5.2 below) leaves a Hub that does not boot. The kernel in kernel2 has the stock partition tables built in, so it could not find a moved root anyway.

### Option B: also trim root (+about 1.4 MB)

The stock root uses 3,664 KB of 4,096 KB. Shrinking root to 3,712 KB (a multiple of the 64 KB erase block, with a little margin) gives data another 384 KB.

Any later stock root image that is bigger no longer fits. HarmonyNG does not reflash root, but the stock updater and the unbrick guide expect 4 MB.

### Option C: USB stick (recommended)

This release can keep the icon archive, the custom icons and the plugins on a USB stick (System page, **USB Storage**, or `POST /api/usb-storage`). That frees about 1.6 MB of `/cache` plus the custom icons and the plugins on `/data`. Nothing is reflashed. When the stick is pulled out, only the icons and the plugins are missing; plugin settings (`plugin-data`) and the shared libraries stay on flash.

It needs the USB storage modules (`scsi_mod`, `sd_mod`, `usb-storage`, built by `build/build_kmods.sh`, in `payload/modules`). The Hub downloads them from the update source when a stick is plugged in (**Download drivers**). The stick is looked for in USB host mode only, which the Hub enters with Ethernet enabled and no PC on the port: with Ethernet turned off, the icons and plugins on a stick are not used after a restart.

---

## 4. What options A and B would take

1. Unpack the kernel uImage from mtd1: strip the 64-byte U-Boot header, then LZMA-decompress.
2. Patch the 16 MB `mtdparts=` string at its offset. The new string must not be longer than the old one; shorter strings are padded with NUL bytes. Keep the partition names.
3. Compress again with the same LZMA settings and build a new uImage (`mkimage`, same load and entry address). It must stay within 960 KB.
4. Back up the whole flash first (all mtd devices, `tools/backup_hub.py` plus raw `/dev/mtdN` dumps), together with the contents of `/data`.
5. Over a serial console:
   1. Write the new root image at its new offset.
   2. Write the new kernel to mtd1.
   3. Erase the new data range.
6. Boot. The built-in init formats the empty data partition. Then restore the `/data` backup and run `deploy_hub.py`.

**Risks:**
- Option A removes the fallback kernel in kernel2 (see above): a failed kernel write then has no second kernel to boot.
- A kernel that does not boot, or a wrong offset, leaves a Hub that only boots to U-Boot. Recovery means TFTP or serial loading (`docs/OFFLINE_UNBRICK_AND_ROOT_GUIDE.md`).
- A write over u-boot or mfg cannot be repaired without an SPI programmer.
- The SPI bus is shared with the RF chip: stop `hub_rf` and `hub_btstack` (`killall -9`) before any flash write, and follow the sync and reboot rules in `AGENTS.md`.
- A stock firmware update (or `tools/restore_stock.py`) writes the stock kernel back. That brings back the stock table, and the moved root is then no longer found.

---

## 5. Elite and Ultimate remotes

The remotes use NAND with UBI/UBIFS (about 128 MB on the Elite). HarmonyNG uses less than 3 MB of `/data` there (`docs/FIRST_INSTALL_REMOTE.md`), so space is not a constraint, and the layout was not studied further.

---

## Recommendation

Use option C first. Do option A only on a Hub with a serial console, and only once the steps above have been tried on a spare Hub. Option B adds little for its extra risk.
