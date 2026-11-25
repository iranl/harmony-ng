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
#
# offload writes one id to the flag file and to the stick: only that stick is used for the icons
# and plugins (another stick is mounted but not bound over the flash folders).
#
# The stick is looked for in USB host mode only, which network_manager.sh enters with Ethernet
# enabled (ETH_ENABLED=1) and no PC on the port.

BB=/cache/bin/busybox
[ -x "$BB" ] || BB=busybox
MNT=/mnt/usb
DIR=$MNT/harmony
STATE=/tmp/usb_storage_state
FLAG=/data/harmony/config/usb_offload
ID_FILE=$DIR/.offload_id
LOCK=/tmp/usb_storage.lock
ICONS=/data/harmony/custom_icons
ARCHIVE_LINK=/data/harmony/icons.tar.gz
ARCHIVE_FLASH=/cache/icons.tar.gz
PLUGINS=/data/harmony/plugins
# kept free on /data for the configuration (as plugin_mgr does)
RESERVE_KB=256
MODS="scsi_mod sd_mod usb-storage"
MODULE_DIRS="/data/harmony/modules /cache/modules"
USB_DEVICES=/sys/bus/usb/devices
SYS_BLOCK=/sys/block
OTHER_STICK=0

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

# one operation at a time: a scan started by the network monitor must not bind the folders again
# while a restore copies them. A lock left by a killed run (its pid gone) is taken over.
take_lock() {
  _w=0
  while ! mkdir "$LOCK" 2>/dev/null; do
    _o=$(cat "$LOCK/pid" 2>/dev/null)
    if { [ -n "$_o" ] && ! kill -0 "$_o" 2>/dev/null; } || { [ -z "$_o" ] && [ "$_w" -ge 5 ]; }; then
      rm -rf "$LOCK" 2>/dev/null
      continue
    fi
    [ "$_w" -ge 900 ] && { echo "[-] Another USB storage operation is still running"; return 1; }
    sleep 1
    _w=$(expr "$_w" + 1)
  done
  echo $$ > "$LOCK/pid"
  trap 'rm -rf "$LOCK" 2>/dev/null' EXIT
  trap 'exit 1' INT TERM
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
      # the disk shows up before the kernel has read its partition table: give the partitions
      # a few seconds before the whole disk is taken (a stick without a partition table)
      [ -z "$_part" ] && [ "$_i" -lt 4 ] && break
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

# unmount_folder DIR: a request serving an icon or a stopped plugin can hold the folder for a
# moment (umount: busy); fails while it is still mounted
unmount_folder() {
  _i=0
  while is_mounted "$1" && [ "$_i" -lt 5 ]; do
    [ "$1" = "$PLUGINS" ] && stop_plugins
    $BB umount "$1" 2>/dev/null || sleep 1
    _i=$(expr "$_i" + 1)
  done
  ! is_mounted "$1"
}

new_id() {
  cat /proc/sys/kernel/random/uuid 2>/dev/null || date +%s
}

# the mounted stick is the one the icons and plugins were moved to. A flag of an older version
# has no id: a stick that holds the folders is taken and both get one.
our_stick() {
  _want=$(cat "$FLAG" 2>/dev/null)
  if [ -z "$_want" ]; then
    [ -d "$DIR/plugins" ] || [ -d "$DIR/custom_icons" ] || return 1
    _want=$(new_id)
    echo "$_want" > "$ID_FILE" 2>/dev/null && echo "$_want" > "$FLAG" 2>/dev/null
    return 0
  fi
  [ "$_want" = "$(cat "$ID_FILE" 2>/dev/null)" ]
}

# the icons and plugins on the stick replace the flash ones: custom icons and plugins by a bind
# mount over their folder, the archive by a link (dangling while the stick is out: the Hub then
# reports it missing)
use_offload() {
  [ -f "$FLAG" ] && is_mounted "$MNT" || return 0
  if ! our_stick; then
    OTHER_STICK=1
    echo "[USB_STORAGE] This is not the USB stick the icons and plugins were moved to: not used for them"
    return 0
  fi
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

# free_kb DIR / mount_of DIR (the last line: BusyBox df wraps a long device name)
free_kb() {
  $BB df -k "$1" 2>/dev/null | awk 'END {print $(NF-2)}'
}
mount_of() {
  $BB df -k "$1" 2>/dev/null | awk 'END {print $NF}'
}
size_kb() {
  _s=""
  [ -e "$1" ] && _s=$($BB du -sk "$1" 2>/dev/null | awk '{print $1; exit}')
  echo "${_s:-0}"
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
    echo "other_stick=$OTHER_STICK"
  } > "$STATE.new" 2>/dev/null && mv -f "$STATE.new" "$STATE" 2>/dev/null
}

mounted_dev() {
  awk -v m="$MNT" '$2 == m {sub("/dev/", "", $1); print $1; exit}' /proc/mounts
}
mounted_fs() {
  awk -v m="$MNT" '$2 == m {print $3; exit}' /proc/mounts
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
      # FAT has no modes: umask=022 gives every file the same ones whoever runs the scan
      _o=noatime
      [ "$_t" = vfat ] && _o=noatime,umask=022
      if $BB mount -t "$_t" -o "$_o" "/dev/$_dev" "$MNT" 2>/dev/null; then
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
    _dev=$(mounted_dev)
    _fs=$(mounted_fs)
  fi
  use_offload
  write_state 1 "$_dev" "$_fs" ""
}

# copy_verified SRC DST: copy and compare MD5 (a stick can be full or fail)
copy_verified() {
  rm -f "$2.new" 2>/dev/null
  cp "$1" "$2.new" 2>/dev/null || { rm -f "$2.new"; return 1; }
  same_file "$1" "$2.new" || { rm -f "$2.new"; return 1; }
  mv -f "$2.new" "$2"
}

same_file() {
  [ -f "$2" ] && [ "$(md5sum "$1" | cut -d' ' -f1)" = "$(md5sum "$2" | cut -d' ' -f1)" ]
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

# same_tree SRC DST: every file of SRC is in DST with the same content
same_tree() {
  for _t in "$1"/* "$1"/.[!.]*; do
    [ -e "$_t" ] || continue
    if [ -d "$_t" ]; then
      ( same_tree "$_t" "$2/${_t##*/}" ) || return 1
    else
      same_file "$_t" "$2/${_t##*/}" || { echo "[-] $2/${_t##*/} does not read back as written"; return 1; }
    fi
  done
}

# merge_folders SRC DST: each folder of SRC replaces the one of that name in DST; the other
# folders of DST stay (plugins only on one side are kept)
merge_folders() {
  for _t in "$1"/* "$1"/.[!.]*; do
    [ -e "$_t" ] || continue
    rm -rf "$2/${_t##*/}" 2>/dev/null
    mv "$_t" "$2/${_t##*/}" || return 1
  done
  rm -rf "$1" 2>/dev/null
}

# the copies on the stick are compared once they left RAM: a stick that drops writes or was
# pulled passes a check that reads the page cache
check_stick_copies() {
  flush
  echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
  find_stick >/dev/null && is_mounted "$MNT" || { echo "[-] The USB stick is gone"; return 1; }
  if [ -f "$ARCHIVE_FLASH" ] && [ ! -L "$ARCHIVE_FLASH" ]; then
    same_file "$ARCHIVE_FLASH" "$DIR/icons.tar.gz" || { echo "[-] The icon archive does not read back as written"; return 1; }
  fi
  if ! is_mounted "$ICONS" && [ -d "$ICONS" ]; then
    same_tree "$ICONS" "$DIR/custom_icons" || return 1
  fi
  if ! is_mounted "$PLUGINS" && [ -d "$PLUGINS" ]; then
    same_tree "$PLUGINS" "$DIR/plugins" || return 1
  fi
}

do_offload() {
  is_mounted "$MNT" || { echo "[-] No USB stick mounted"; return 1; }
  if [ -f "$FLAG" ] && ! our_stick; then
    echo "[-] The icons and plugins are on another USB stick; keep them on flash first"
    return 1
  fi
  _id=$(cat "$FLAG" 2>/dev/null)
  [ -n "$_id" ] || _id=$(new_id)
  mkdir -p "$DIR/custom_icons" "$DIR/plugins" || return 1
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
    # by plugin: those only on the stick (kept on flash while it was out, say) stay
    merge_folders "$DIR/plugins.new" "$DIR/plugins" || { echo "[-] Could not move the plugins into place on the stick"; return 1; }
  fi
  check_stick_copies || { echo "[-] Nothing was removed from flash"; return 1; }
  # everything is on the stick: only now the flash copies go
  { echo "$_id" > "$ID_FILE" && flush && echo "$_id" > "$FLAG"; } 2>/dev/null || { echo "[-] Could not write the offload id"; return 1; }
  rm -f "$ARCHIVE_FLASH" "$ARCHIVE_LINK" 2>/dev/null
  if ! is_mounted "$ICONS"; then
    rm -f "$ICONS"/* 2>/dev/null
  fi
  if ! is_mounted "$PLUGINS"; then
    stop_plugins
    rm -rf "$PLUGINS"/* "$PLUGINS"/.[!.]* 2>/dev/null
  fi
  use_offload
  write_state 1 "$(mounted_dev)" "$(mounted_fs)" ""
  echo "[+] Icons and plugins moved to the USB stick"
}

# undo_restore: what this restore copied to flash goes, the stick is used again
undo_restore() {
  rm -rf "$ICONS.restore" "$PLUGINS.restore" 2>/dev/null
  use_offload
}

do_restore() {
  if is_mounted "$MNT"; then
    if [ -f "$FLAG" ] && ! our_stick; then
      echo "[-] This is not the USB stick the icons and plugins were moved to"
      return 1
    fi
    # room first: a copy that stops half way would fill /data below the bind mount
    _need=$(expr "$(size_kb "$DIR/plugins")" + "$(size_kb "$DIR/custom_icons")")
    _need_cache=0
    if [ -f "$DIR/icons.tar.gz" ] && ! { [ -f "$ARCHIVE_FLASH" ] && same_file "$DIR/icons.tar.gz" "$ARCHIVE_FLASH"; }; then
      _need_cache=$(size_kb "$DIR/icons.tar.gz")
    fi
    _free=$(free_kb /data)
    if [ "$(mount_of /cache)" = "$(mount_of /data)" ]; then
      _need=$(expr "$_need" + "$_need_cache")
    elif [ "$_need_cache" -gt 0 ] && [ "$_need_cache" -gt "$(free_kb /cache)" ]; then
      echo "[-] No room on flash for the icon archive: ${_need_cache} KB needed, $(free_kb /cache) KB free"
      return 1
    fi
    if [ -n "$_free" ] && [ "$(expr "$_need" + "$RESERVE_KB")" -gt "$_free" ]; then
      echo "[-] No room on flash: the icons and plugins need ${_need} KB, ${_free} KB free (${RESERVE_KB} KB stay free for the settings). Nothing was changed"
      return 1
    fi
    unmount_folder "$ICONS" || { echo "[-] The custom icon folder on the stick is still in use; try again"; use_offload; return 1; }
    unmount_folder "$PLUGINS" || { echo "[-] The plugin folder on the stick is still in use; try again"; use_offload; return 1; }
    mkdir -p "$ICONS" "$PLUGINS"
    # into side folders first, moved into place once everything is copied
    rm -rf "$ICONS.restore" "$PLUGINS.restore" 2>/dev/null
    mkdir -p "$ICONS.restore" "$PLUGINS.restore" || { undo_restore; return 1; }
    for _f in "$DIR/custom_icons"/*; do
      [ -f "$_f" ] || continue
      copy_verified "$_f" "$ICONS.restore/${_f##*/}" || { echo "[-] No room on flash for ${_f##*/}"; undo_restore; return 1; }
    done
    if [ -d "$DIR/plugins" ]; then
      ( copy_tree "$DIR/plugins" "$PLUGINS.restore" ) || { echo "[-] No room on flash for the plugins"; undo_restore; return 1; }
    fi
    if [ "$_need_cache" -gt 0 ]; then
      copy_verified "$DIR/icons.tar.gz" "$ARCHIVE_FLASH" || { echo "[-] No room on flash for the icon archive"; undo_restore; return 1; }
    fi
    if [ -f "$DIR/icons.tar.gz" ]; then
      # the link changes only once the flash copy is there
      rm -f "$ARCHIVE_LINK" 2>/dev/null
      ln -sf "$ARCHIVE_FLASH" "$ARCHIVE_LINK" 2>/dev/null
    fi
    for _f in "$ICONS.restore"/*; do
      [ -f "$_f" ] || continue
      mv -f "$_f" "$ICONS/${_f##*/}" || { undo_restore; return 1; }
    done
    rm -rf "$ICONS.restore" 2>/dev/null
    merge_folders "$PLUGINS.restore" "$PLUGINS" || { undo_restore; return 1; }
    _msg="[+] Icons and plugins kept on flash again"
  else
    # no stick: the icons and plugins stay on it; the flash gets the archive again with the
    # next update
    rm -f "$ARCHIVE_LINK" 2>/dev/null
    _msg="[+] Icons and plugins kept on flash again; those on the USB stick stay there (moving them to the stick later keeps them)"
  fi
  rm -f "$FLAG"
  flush
  # the plugins start again from flash (hub_daemon watchdog)
  stop_plugins
  do_scan >/dev/null
  echo "$_msg"
}

do_eject() {
  unmount_folder "$ICONS"
  unmount_folder "$PLUGINS"
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
  scan|install|eject|offload|restore) take_lock || exit 1 ;;
  *) echo "Usage: $0 scan|install|eject|offload|restore" >&2; exit 1 ;;
esac
case "${1:-scan}" in
  scan) do_scan ;;
  install) do_install ;;
  eject) do_eject ;;
  offload) do_offload ;;
  restore) do_restore ;;
esac
