#!/bin/sh
# usb_storage.sh - a USB stick for files the Hub can run without (icon archive, custom icons,
# plugins)
#
#   scan            find a USB stick (also behind a USB hub), load its modules, mount it on
#                   /mnt/usb and use the icons and plugins on it when they were moved there; a
#                   stick that is gone is unmounted (its plugins are stopped). Writes /tmp/usb_storage_state for the web UI.
#   install         download the USB storage modules (update.sh fetch-module), then scan
#   eject           write everything to the stick and unmount it
#   offload         move the icon archive, the custom icons and the plugins to the stick
#   restore         move them back to flash
#
# Essential files (programs, scripts, settings, web UI) never move: the Hub must run the same
# with the stick pulled out. Without the stick the icons are missing (the web UI downloads the
# archive online) and the plugins are not installed until it is back. Plugin settings and state
# (plugin-data) and the shared libraries stay on flash.

BB=/cache/bin/busybox
[ -x "$BB" ] || BB=busybox
MNT=/mnt/usb
DIR=$MNT/harmony
STATE=/tmp/usb_storage_state
FLAG=/data/harmony/config/usb_offload
ICONS=/data/harmony/custom_icons
ARCHIVE_LINK=/data/harmony/icons.tar.gz
ARCHIVE_FLASH=/cache/icons.tar.gz
PLUGINS=/data/harmony/plugins
MODS="scsi_mod sd_mod usb-storage"
MODULE_DIRS="/data/harmony/modules /cache/modules"
USB_DEVICES=/sys/bus/usb/devices
SYS_BLOCK=/sys/block

find_mod() {
  for _d in $MODULE_DIRS; do
    [ -f "$_d/$1.ko" ] && { echo "$_d/$1.ko"; return 0; }
  done
  return 1
}

# module names in /proc/modules use _ for -
mod_loaded() {
  _n=$(echo "$1" | sed 's/-/_/g')
  grep -q "^$_n " /proc/modules 2>/dev/null
}

is_mounted() {
  grep -q " $1 " /proc/mounts 2>/dev/null
}

# a USB device with a mass storage interface (class 08)
find_stick() {
  for _dev in "$USB_DEVICES"/*; do
    case "${_dev##*/}" in [0-9]*-*) ;; *) continue ;; esac
    case "${_dev##*/}" in *:*) continue ;; esac
    for _ic in "$_dev"/*:*/bInterfaceClass; do
      [ -f "$_ic" ] || continue
      if [ "$(cat "$_ic" 2>/dev/null)" = "08" ]; then
        echo "$_dev"
        return 0
      fi
    done
  done
  return 1
}

# the first partition of the first USB disk (or the disk itself without a partition table);
# makes its /dev node (no hotplug on the stock kernel, so no node appears by itself)
find_blockdev() {
  for _i in 1 2 3 4 5 6 7 8 9 10; do
    for _b in "$SYS_BLOCK"/sd*; do
      [ -d "$_b" ] || continue
      _disk=${_b##*/}
      _part=""
      for _p in "$_b/$_disk"*; do
        [ -f "$_p/dev" ] && { _part=${_p##*/}; break; }
      done
      _name=${_part:-$_disk}
      _devfile="$_b/dev"
      [ -n "$_part" ] && _devfile="$_b/$_part/dev"
      if [ ! -b "/dev/$_name" ]; then
        _mm=$(cat "$_devfile" 2>/dev/null)
        $BB mknod "/dev/$_name" b "${_mm%%:*}" "${_mm##*:}" 2>/dev/null
      fi
      echo "$_name"
      return 0
    done
    sleep 1
  done
  return 1
}

# running plugins (hub_daemon starts them with HARMONY_SOCKET=/tmp/plugins/<id>.sock, as
# init.sh finds them)
plugin_pids() {
  for _e in /proc/[0-9]*/environ; do
    _p=${_e#/proc/}
    _p=${_p%/environ}
    tr '\000' '\n' < "$_e" 2>/dev/null | grep -q '^HARMONY_SOCKET=/tmp/plugins/' && echo "$_p"
  done
}

# stop the plugins so none runs from the folder that is switched; hub_daemon reaps them and
# its watchdog starts those it finds in the plugin folder again (within about 20 s)
stop_plugins() {
  _left=$(plugin_pids)
  [ -n "$_left" ] || return 0
  kill -15 $_left 2>/dev/null
  sleep 1
  _left=$(plugin_pids)
  [ -n "$_left" ] && kill -9 $_left 2>/dev/null
  return 0
}

# the icons and plugins on the stick replace the flash ones: custom icons and plugins by a bind
# mount over their folder, the archive by a link (dangling while the stick is out: the Hub then
# reports it missing)
use_offload() {
  [ -f "$FLAG" ] && is_mounted "$MNT" || return 0
  mkdir -p "$DIR/custom_icons" "$ICONS" "$DIR/plugins" "$PLUGINS" 2>/dev/null
  is_mounted "$ICONS" || $BB mount -o bind "$DIR/custom_icons" "$ICONS" 2>/dev/null
  if ! is_mounted "$PLUGINS"; then
    # plugins installed on flash while the stick was out stay there, hidden below the mount
    stop_plugins
    $BB mount -o bind "$DIR/plugins" "$PLUGINS" 2>/dev/null
  fi
  if [ -f "$DIR/icons.tar.gz" ] && [ ! -L "$ARCHIVE_LINK" ]; then
    rm -f "$ARCHIVE_LINK" 2>/dev/null
    ln -sf "$DIR/icons.tar.gz" "$ARCHIVE_LINK" 2>/dev/null
  fi
}

release_stick() {
  is_mounted "$ICONS" && $BB umount -l "$ICONS" 2>/dev/null
  if is_mounted "$PLUGINS"; then
    $BB umount -l "$PLUGINS" 2>/dev/null
    stop_plugins
  fi
  is_mounted "$MNT" && $BB umount -l "$MNT" 2>/dev/null
}

flush() {
  for _s in /data/harmony/bin/hub_sync /data/codex/bin/codex_sync; do
    [ -x "$_s" ] && { "$_s" 2>/dev/null; return 0; }
  done
  $BB sync 2>/dev/null
}

write_state() {
  _present=0; _mounted=0; _total=""; _free=""
  [ -n "$1" ] && _present=1
  if is_mounted "$MNT"; then
    _mounted=1
    _df=$($BB df -k "$MNT" 2>/dev/null | awk 'NR==2 {print $2" "$4}')
    _total=${_df%% *}; _free=${_df##* }
  fi
  _off=0
  [ -f "$FLAG" ] && _off=1
  {
    echo "present=$_present"
    echo "device=$2"
    echo "fs=$3"
    echo "mounted=$_mounted"
    echo "missing=$4"
    echo "total_kb=$_total"
    echo "free_kb=$_free"
    echo "offload=$_off"
  } > "$STATE.new" 2>/dev/null && mv -f "$STATE.new" "$STATE" 2>/dev/null
}

do_scan() {
  _stick=$(find_stick)
  if [ -z "$_stick" ]; then
    if is_mounted "$MNT"; then
      echo "[USB_STORAGE] USB stick removed: unmounting $MNT"
      release_stick
    fi
    write_state "" "" "" ""
    return 0
  fi
  _missing=""
  for _m in $MODS; do
    find_mod "$_m" >/dev/null || _missing="${_missing:+$_missing }$_m"
  done
  if [ -n "$_missing" ]; then
    echo "[USB_STORAGE] USB stick found; modules not installed: $_missing"
    write_state 1 "" "" "$_missing"
    return 0
  fi
  if ! is_mounted "$MNT"; then
    for _m in $MODS; do
      mod_loaded "$_m" || insmod "$(find_mod "$_m")" 2>/dev/null || true
    done
    _dev=$(find_blockdev)
    if [ -z "$_dev" ]; then
      echo "[USB_STORAGE] USB stick found but no disk appeared"
      write_state 1 "" "" ""
      return 0
    fi
    mkdir -p "$MNT" 2>/dev/null
    _fs=""
    for _t in vfat ext2; do
      if $BB mount -t "$_t" -o noatime "/dev/$_dev" "$MNT" 2>/dev/null; then
        _fs=$_t
        break
      fi
    done
    if [ -z "$_fs" ]; then
      echo "[USB_STORAGE] /dev/$_dev: no FAT or ext2 file system"
      write_state 1 "$_dev" "" ""
      return 0
    fi
    echo "[USB_STORAGE] /dev/$_dev ($_fs) mounted on $MNT"
  else
    _dev=$(awk -v m="$MNT" '$2 == m {sub("/dev/", "", $1); print $1; exit}' /proc/mounts)
    _fs=$(awk -v m="$MNT" '$2 == m {print $3; exit}' /proc/mounts)
  fi
  use_offload
  write_state 1 "$_dev" "$_fs" ""
}

# copy_verified SRC DST: copy and compare MD5 (a stick can be full or fail)
copy_verified() {
  rm -f "$2.new" 2>/dev/null
  cp "$1" "$2.new" 2>/dev/null || { rm -f "$2.new"; return 1; }
  [ "$(md5sum "$1" | cut -d' ' -f1)" = "$(md5sum "$2.new" | cut -d' ' -f1)" ] || { rm -f "$2.new"; return 1; }
  mv -f "$2.new" "$2"
}

# copy_tree SRC DST: a folder with its subfolders, each file checked
copy_tree() {
  mkdir -p "$2" || return 1
  for _t in "$1"/* "$1"/.[!.]*; do
    [ -e "$_t" ] || continue
    if [ -d "$_t" ]; then
      ( copy_tree "$_t" "$2/${_t##*/}" ) || return 1
    else
      copy_verified "$_t" "$2/${_t##*/}" || { echo "[-] Could not copy $_t"; return 1; }
    fi
  done
}

do_offload() {
  is_mounted "$MNT" || { echo "[-] No USB stick mounted"; return 1; }
  mkdir -p "$DIR/custom_icons" || return 1
  if [ -f "$ARCHIVE_FLASH" ] && [ ! -L "$ARCHIVE_FLASH" ]; then
    copy_verified "$ARCHIVE_FLASH" "$DIR/icons.tar.gz" || { echo "[-] Could not copy the icon archive to the stick"; return 1; }
  fi
  if ! is_mounted "$ICONS" && [ -d "$ICONS" ]; then
    for _f in "$ICONS"/*; do
      [ -f "$_f" ] || continue
      copy_verified "$_f" "$DIR/custom_icons/${_f##*/}" || { echo "[-] Could not copy ${_f##*/} to the stick"; return 1; }
    done
  fi
  if ! is_mounted "$PLUGINS" && [ -d "$PLUGINS" ]; then
    rm -rf "$DIR/plugins.new" 2>/dev/null
    ( copy_tree "$PLUGINS" "$DIR/plugins.new" ) || { rm -rf "$DIR/plugins.new"; echo "[-] Could not copy the plugins to the stick"; return 1; }
    rm -rf "$DIR/plugins" 2>/dev/null
    mv "$DIR/plugins.new" "$DIR/plugins" || return 1
  fi
  # everything is on the stick: only now the flash copies go
  flush
  touch "$FLAG" || return 1
  rm -f "$ARCHIVE_FLASH" "$ARCHIVE_LINK" 2>/dev/null
  if ! is_mounted "$ICONS"; then
    rm -f "$ICONS"/* 2>/dev/null
  fi
  if ! is_mounted "$PLUGINS"; then
    stop_plugins
    rm -rf "$PLUGINS"/* "$PLUGINS"/.[!.]* 2>/dev/null
  fi
  use_offload
  write_state 1 "$(awk -v m="$MNT" '$2 == m {sub("/dev/", "", $1); print $1; exit}' /proc/mounts)" "$(awk -v m="$MNT" '$2 == m {print $3; exit}' /proc/mounts)" ""
  echo "[+] Icons and plugins moved to the USB stick"
}

do_restore() {
  if is_mounted "$MNT"; then
    is_mounted "$ICONS" && $BB umount "$ICONS" 2>/dev/null
    mkdir -p "$ICONS"
    if [ -f "$DIR/icons.tar.gz" ]; then
      rm -f "$ARCHIVE_LINK" 2>/dev/null
      copy_verified "$DIR/icons.tar.gz" "$ARCHIVE_FLASH" || { echo "[-] No room on flash for the icon archive"; use_offload; return 1; }
      ln -sf "$ARCHIVE_FLASH" "$ARCHIVE_LINK" 2>/dev/null
    fi
    for _f in "$DIR/custom_icons"/*; do
      [ -f "$_f" ] || continue
      copy_verified "$_f" "$ICONS/${_f##*/}" || { echo "[-] No room on flash for ${_f##*/}"; use_offload; return 1; }
    done
    if [ -d "$DIR/plugins" ]; then
      is_mounted "$PLUGINS" && { stop_plugins; $BB umount "$PLUGINS" 2>/dev/null; }
      ( copy_tree "$DIR/plugins" "$PLUGINS" ) || { echo "[-] No room on flash for the plugins"; use_offload; return 1; }
    fi
  else
    # no stick: the icons stay on it; the flash gets the archive again with the next update
    rm -f "$ARCHIVE_LINK" 2>/dev/null
  fi
  rm -f "$FLAG"
  flush
  # the plugins start again from flash (hub_daemon watchdog)
  stop_plugins
  do_scan >/dev/null
  echo "[+] Icons and plugins kept on flash again"
}

do_eject() {
  is_mounted "$ICONS" && $BB umount "$ICONS" 2>/dev/null
  if is_mounted "$PLUGINS"; then
    stop_plugins
    $BB umount "$PLUGINS" 2>/dev/null
  fi
  flush
  if is_mounted "$MNT" && ! $BB umount "$MNT" 2>/dev/null; then
    echo "[-] The stick is in use; try again"
    use_offload
    return 1
  fi
  write_state 1 "" "" ""
  echo "[+] USB stick can be removed"
}

do_install() {
  for _m in $MODS; do
    find_mod "$_m" >/dev/null && continue
    /data/harmony/update.sh fetch-module "$_m" || return 1
  done
  do_scan
}

case "${1:-scan}" in
  scan) do_scan ;;
  install) do_install ;;
  eject) do_eject ;;
  offload) do_offload ;;
  restore) do_restore ;;
  *) echo "Usage: $0 scan|install|eject|offload|restore" >&2; exit 1 ;;
esac
