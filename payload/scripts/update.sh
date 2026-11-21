#!/bin/sh
# update.sh - Hub OTA software check and apply script

BB=""
if [ -x /cache/bin/busybox ]; then
  BB="/cache/bin/busybox"
elif [ -x /data/harmony/bin/busybox ]; then
  BB="/data/harmony/bin/busybox"
else
  BB="busybox"
fi

# busybox wget does not verify TLS certificates, so nothing downloaded here is trusted until
# hub_util ota-verify has checked it: the Ed25519 signature of manifest.json against the release
# keys built into the firmware, then the SHA-256 of every file against the signed manifest.
WGET="$BB wget"
HUB_UTIL=/data/harmony/bin/hub_util
TAR="$BB tar"

CMD="${1:-apply}"
SLOT_ARG="1"
if [ "$CMD" = "remote" ] || [ "$CMD" = "elite" ]; then
  if [ -n "${2:-}" ] && [ "$2" = "1" -o "$2" = "2" ]; then
    SLOT_ARG="$2"
    REPO_ARG="${3:-}"
    TOKEN_ARG="${4:-}"
  else
    REPO_ARG="${2:-}"
    TOKEN_ARG="${3:-}"
  fi
else
  REPO_ARG="${2:-}"
  TOKEN_ARG="${3:-}"
fi

DEFAULT_OTA="https://raw.githubusercontent.com/iranl/harmony-ng/ota"
if [ "$CMD" = "remote" -o "$CMD" = "elite" -o "$CMD" = "fetch-remote" -o "$CMD" = "fetch-remote-ota" ] && [ -z "$REPO_ARG" ] && [ -f /data/harmony/update_state.conf ]; then
  CONF_REPO=$(grep '^source=' /data/harmony/update_state.conf 2>/dev/null | cut -d= -f2- | tr -d '\r\n ')
  [ -n "$CONF_REPO" ] && REPO_ARG="$CONF_REPO"
fi
REPO="${REPO_ARG:-$DEFAULT_OTA}"
REPO="${REPO%/}"
REPO="${REPO%/bin/mips}"
REPO="${REPO%/}"
[ -z "$REPO" ] && REPO="$DEFAULT_OTA"


TIMESTAMP=$(date +%s 2>/dev/null || echo 1)

# State of the last apply for GET /api/update-status: running, restarting (with the new version)
# or failed, and since= the seconds since boot (hub_daemon ends a "running" older than 30 minutes
# on that clock, which SNTP never steps). In RAM: a reboot clears it; the new hub_daemon ends
# "restarting" when it starts.
UPDATE_STATE=/tmp/hub_update.state
set_update_state() {
  _since=$(now_secs)
  { echo "state=$1"; [ -z "$2" ] || echo "version=$2"; [ -z "$_since" ] || echo "since=$_since"; } > "$UPDATE_STATE" 2>/dev/null || true
}

download_retry() {
  local url="$1"
  local dst="$2"
  local timeout="${3:-12}"
  local max_attempts="${4:-3}"
  local i=1
  while [ $i -le $max_attempts ]; do
    rm -f "$dst" 2>/dev/null
    # the header is one argument: unquoted, the token was passed to wget as extra URLs
    if [ -n "$TOKEN_ARG" ]; then
      $WGET -q -T "$timeout" --header "Authorization: token ${TOKEN_ARG}" -O "$dst" "$url" 2>/dev/null
    else
      $WGET -q -T "$timeout" -O "$dst" "$url" 2>/dev/null
    fi
    if [ -s "$dst" ]; then
      return 0
    fi
    sleep 1
    i=$((i + 1))
  done
  return 1
}

# Download manifest.json and manifest.json.sig into $1 and verify the signature; prints the
# release version. Fails when either is missing or the signature does not match a release key.
fetch_signed_manifest() {
  local dir="$1"
  rm -f "$dir/manifest.json" "$dir/manifest.json.sig"
  if ! download_retry "${REPO}/manifest.json?_=${TIMESTAMP}" "$dir/manifest.json" 10 3; then
    echo "[-] Release manifest (manifest.json) missing from ${REPO}." >&2
    return 1
  fi
  if ! download_retry "${REPO}/manifest.json.sig?_=${TIMESTAMP}" "$dir/manifest.json.sig" 10 3; then
    echo "[-] Release signature (manifest.json.sig) missing from ${REPO}." >&2
    return 1
  fi
  if [ ! -x "$HUB_UTIL" ]; then
    echo "[-] $HUB_UTIL missing: cannot check the release signature." >&2
    return 1
  fi
  "$HUB_UTIL" ota-verify "$dir/manifest.json" "$dir/manifest.json.sig"
}

# Check downloaded files (key=path ...) against the signed manifest in $TMPDIR.
verify_files() {
  "$HUB_UTIL" ota-verify "$TMPDIR/manifest.json" "$TMPDIR/manifest.json.sig" "$@" >/dev/null
}

# Files are installed one at a time from the verified copy in RAM (/tmp): each is copied to
# <dest>.new on the same file system, checked by MD5 and renamed over the installed file. So each
# file is always whole (the old or the new one, never cut) and the flash needs room for one new
# file at a time, not for the whole release. A running program or script (update.sh itself) keeps
# its old file until it exits. A failure part way leaves some files new and the rest old; VERSION
# is written only after every file is in place, so the update can be run again.
INSTALLING=""

file_md5() {
  md5sum "$1" 2>/dev/null | cut -d' ' -f1
}

# install_file SRC DEST MODE: replace DEST with SRC; skipped when DEST already matches.
install_file() {
  _want=$(file_md5 "$1")
  if [ -z "$_want" ]; then
    echo "[-] Cannot read $1."
    return 1
  fi
  if [ -f "$2" ] && [ ! -L "$2" ] && [ "$(file_md5 "$2")" = "$_want" ]; then
    chmod "$3" "$2" 2>/dev/null
    return 0
  fi
  INSTALLING="$2.new"
  rm -f "$2.new" 2>/dev/null
  if ! cp "$1" "$2.new" 2>/dev/null || [ "$(file_md5 "$2.new")" != "$_want" ] || \
     ! chmod "$3" "$2.new" 2>/dev/null; then
    echo "[-] Could not write $2.new (flash full?)."
    rm -f "$2.new" 2>/dev/null
    INSTALLING=""
    return 1
  fi
  if ! mv -f "$2.new" "$2" 2>/dev/null; then
    echo "[-] Could not rename $2.new to $2."
    rm -f "$2.new" 2>/dev/null
    INSTALLING=""
    return 1
  fi
  INSTALLING=""
  return 0
}

# Binaries of the unified package that go to /data/harmony/bin
BUNDLE_BINS="hub_daemon hub_rf hub_btstack dropbearmulti hub_sync register_ehci eth_fix"

# install_bundle PKG: install every file of the unpacked package; stops at the first failure.
install_bundle() {
  _pkg="$1"
  # A. Scripts -> /data/harmony/, rcS.local also -> /etc/init.d/
  if [ -d "$_pkg/scripts" ]; then
    for _f in "$_pkg/scripts/"*; do
      [ -f "$_f" ] || continue
      install_file "$_f" "/data/harmony/${_f##*/}" 755 || return 1
    done
    if [ -f "$_pkg/scripts/rcS.local" ]; then
      install_file "$_pkg/scripts/rcS.local" /etc/init.d/rcS.local 755 || return 1
    fi
  fi
  # CA Bundle -> /data/harmony/cacert.pem
  if [ -f "$_pkg/config/cacert.pem" ]; then
    install_file "$_pkg/config/cacert.pem" /data/harmony/cacert.pem 644 || return 1
  elif [ -f "$_pkg/cacert.pem" ]; then
    install_file "$_pkg/cacert.pem" /data/harmony/cacert.pem 644 || return 1
  fi
  # B. Application binaries -> /data/harmony/bin/
  for _b in hub_util $BUNDLE_BINS; do
    if [ -f "$_pkg/bin/$_b" ]; then
      install_file "$_pkg/bin/$_b" "/data/harmony/bin/$_b" 755 || return 1
    fi
  done
  # C. BusyBox -> /cache/bin/
  if [ -f "$_pkg/bin/busybox" ]; then
    install_file "$_pkg/bin/busybox" /cache/bin/busybox 755 || return 1
  fi
  # F. Icons archive -> /cache/icons.tar.gz
  if [ -f "$_pkg/icons.tar.gz" ]; then
    install_file "$_pkg/icons.tar.gz" /cache/icons.tar.gz 644 || return 1
  fi
  return 0
}

# The helpers built into hub_util are symlinks to it
link_hub_util() {
  for _app in hub_sync sync mknod register_ehci eth_fix hub_sntp; do
    [ -f "/data/harmony/bin/$_app" ] && [ ! -L "/data/harmony/bin/$_app" ] && rm -f "/data/harmony/bin/$_app" 2>/dev/null || true
    ln -sf /data/harmony/bin/hub_util "/data/harmony/bin/$_app" 2>/dev/null || true
  done
}

link_dropbear() {
  if [ -s /data/harmony/bin/dropbearmulti ] && [ -x /data/harmony/bin/dropbearmulti ]; then
    for _app in dropbear dropbearkey dbclient scp; do
      [ -f "/data/harmony/bin/$_app" ] && [ ! -L "/data/harmony/bin/$_app" ] && rm -f "/data/harmony/bin/$_app" 2>/dev/null || true
      ln -sf /data/harmony/bin/dropbearmulti "/data/harmony/bin/$_app" 2>/dev/null || true
    done
    for _app in dropbearkey dbclient scp; do
      ln -sf /data/harmony/bin/dropbearmulti "/usr/sbin/$_app" 2>/dev/null || true
    done
    _harm_m=$(md5sum /data/harmony/bin/dropbearmulti 2>/dev/null | awk '{print $1}')
    _data_m=$(md5sum /data/dropbearmulti 2>/dev/null | awk '{print $1}')
    if [ -n "$_harm_m" ] && { [ "$_harm_m" = "$_data_m" ] || [ "$_harm_m" = "3733327fd04bca282dbf06f33da0bf6c" ]; }; then
      rm -f /data/dropbearmulti /usr/sbin/dropbearmulti 2>/dev/null || true
    fi
  fi
}

get_local_version() {
  local v=""
  if [ -f /data/harmony/VERSION ]; then
    v=$(cat /data/harmony/VERSION | tr -d '\r\n ')
  elif [ -f /etc/harmony_version ]; then
    v=$(cat /etc/harmony_version | tr -d '\r\n ')
  fi
  # If hub_daemon binary is installed, this Hub is running HarmonyNG v2+.
  # Heal any missing or stale 1.x version artifact.
  local maj=$(echo "$v" | cut -d. -f1)
  if [ -x /data/harmony/bin/hub_daemon ] && { [ -z "$v" ] || [ "$maj" = "1" ]; }; then
    v="2.0.0"
    echo "$v" > /data/harmony/VERSION 2>/dev/null || true
  fi
  echo "${v:-2.0.0}"
}

is_version_newer() {
  awk -v r="$1" -v l="$2" '
  BEGIN {
    sub(/^[vV]/, "", r);
    sub(/^[vV]/, "", l);
    split(r, ra, ".");
    split(l, la, ".");
    for (i = 1; i <= 3; i++) {
      rv = ra[i] + 0;
      lv = la[i] + 0;
      if (rv > lv) exit 0;
      if (rv < lv) exit 1;
    }
    exit 1;
  }'
}

do_check() {
  LOCAL_VER=$(get_local_version)
  REMOTE_VER=""

  # 1. Try raw GitHub OTA VERSION file with retries (v2 checks v2/VERSION first)
  if download_retry "${REPO}/v2/VERSION?_=${TIMESTAMP}" /tmp/ota_ver.$$ 8 2 || \
     download_retry "${REPO}/VERSION_V2?_=${TIMESTAMP}" /tmp/ota_ver.$$ 8 2 || \
     download_retry "${REPO}/VERSION?_=${TIMESTAMP}" /tmp/ota_ver.$$ 8 3; then
    REMOTE_VER=$(cat /tmp/ota_ver.$$ | tr -d '\r\n v')
  fi

  # 2. Fallback: try GitHub releases API
  if [ -z "$REMOTE_VER" ]; then
    if download_retry "https://api.github.com/repos/iranl/harmony-ng/releases/latest" /tmp/gh_rel.$$ 10 2; then
      REMOTE_VER=$(cat /tmp/gh_rel.$$ | awk -F'"tag_name":' '{print $2}' | awk -F'"' '{print $2}' | tr -d '\r\n v')
    fi
  fi

  rm -f /tmp/ota_ver.$$ /tmp/gh_rel.$$ 2>/dev/null

  if [ -z "$REMOTE_VER" ]; then
    echo "{\"ok\":false,\"error\":\"Could not fetch remote version from ${REPO}\"}"
    return 1
  fi

  # Major version guard: 1.x cannot OTA-update to 2.x
  LOCAL_MAJOR=$(echo "$LOCAL_VER" | cut -d. -f1)
  REMOTE_MAJOR=$(echo "$REMOTE_VER" | cut -d. -f1)
  if [ -n "$LOCAL_MAJOR" ] && [ -n "$REMOTE_MAJOR" ] && [ "$LOCAL_MAJOR" != "$REMOTE_MAJOR" ]; then
    if [ -x /data/harmony/bin/hub_daemon ] && [ "$LOCAL_MAJOR" = "1" ]; then
      LOCAL_VER="2.0.0"
      LOCAL_MAJOR="2"
      echo "2.0.0" > /data/harmony/VERSION 2>/dev/null || true
    else
      echo "{\"ok\":false,\"error\":\"Major upgrade from v${LOCAL_VER} to v${REMOTE_VER} requires force deploy script\"}"
      return 1
    fi
  fi

  mkdir -p /data/harmony 2>/dev/null || true
  echo "$REMOTE_VER" > /data/harmony/latest_version

  HAS_UPDATE=0
  if is_version_newer "$REMOTE_VER" "$LOCAL_VER"; then
    HAS_UPDATE=1
    touch /data/harmony/update_available.flag
  else
    rm -f /data/harmony/update_available.flag 2>/dev/null || true
  fi

  cat <<EOF > /data/harmony/update_state.conf
checkedAt=${TIMESTAMP}
available=${HAS_UPDATE}
local_version=${LOCAL_VER}
remote_version=${REMOTE_VER}
source=${REPO}
EOF

  echo "{\"ok\":true,\"local_version\":\"${LOCAL_VER}\",\"remote_version\":\"${REMOTE_VER}\",\"update_available\":$([ $HAS_UPDATE -eq 1 ] && echo true || echo false)}"
  return 0
}

# The services restart at the end of an update must not cut an activity switch in the middle (the devices
# were left half switched). hub_daemon refuses to start an update while one runs, but a switch can
# start during the download: wait until GET /api/activity (loopback, no login) reports no switch,
# no queued request and no steps left to send, twice 1 s apart (a finished switch hands over to its
# queued follower in between). Gives up after UPDATE_ACTIVITY_WAIT_SECS (600 s) and restarts anyway
# (the new files are installed already). A query that fails is "unknown", not "no switch": only a
# hub_daemon that is not running means no switch; one that does not answer for
# UPDATE_ACTIVITY_FAIL_SECS (60 s) in a row lets the restart go on. Times are real seconds from
# /proc/uptime (a query can take 3 s; no steps when SNTP sets the clock), counted from the sleeps
# when it cannot be read.
UPTIME_FILE="${UPDATE_UPTIME_FILE:-/proc/uptime}"
now_secs() {
  _up=""
  read -r _up _rest 2>/dev/null < "$UPTIME_FILE"
  _up="${_up%%.*}"
  case "$_up" in ''|*[!0-9]*) echo "" ;; *) echo "$_up" ;; esac
}
# seconds since $1 (now_secs), or $2 (counted from the sleeps) when the uptime cannot be read
since_secs() {
  _n=$(now_secs)
  if [ -n "$1" ] && [ -n "$_n" ]; then echo $((_n - $1)); else echo "$2"; fi
}
wait_activity_switch() {
  local max="${UPDATE_ACTIVITY_WAIT_SECS:-600}"
  local fail_max="${UPDATE_ACTIVITY_FAIL_SECS:-60}"
  case "$max" in ''|*[!0-9]*) max=600 ;; esac
  case "$fail_max" in ''|*[!0-9]*) fail_max=60 ;; esac
  local start
  start=$(now_secs)
  local counted=0
  local fail_start=""
  local fail_counted=0
  local idle=0
  local said=0
  local st
  local qstart
  while [ "$(since_secs "$start" "$counted")" -lt "$max" ]; do
    qstart=$(now_secs)
    if st=$($WGET -q -T 3 -O - "http://127.0.0.1:8089/api/activity" 2>/dev/null); then
      fail_start=""
      fail_counted=0
      if ! echo "$st" | grep -q '"busy":[[:space:]]*true' && \
         ! echo "$st" | grep -q '"sending":[[:space:]]*true' && \
         ! echo "$st" | grep -q '"queued":[[:space:]]*"'; then
        idle=$((idle + 1))
        if [ "$idle" -ge 2 ]; then
          [ "$said" = 1 ] && echo "[+] Activity switch finished after $(since_secs "$start" "$counted") s."
          return 0
        fi
        sleep 1
        counted=$((counted + 1))
        continue
      fi
      idle=0
      if [ "$said" = 0 ]; then
        echo "[*] An activity switch is running: the restart waits until it has finished (at most ${max} s)."
        said=1
      fi
    else
      idle=0
      if ! pidof hub_daemon >/dev/null 2>&1; then
        return 0
      fi
      if [ -z "$fail_start" ]; then
        fail_start="$qstart" # from the start of the first failed query
        [ -z "$fail_start" ] && fail_start="none"
      fi
      _fs="$fail_start"
      [ "$_fs" = "none" ] && _fs=""
      fail_counted=$((fail_counted + 2))
      if [ "$(since_secs "$_fs" "$fail_counted")" -ge "$fail_max" ]; then
        echo "[!] hub_daemon has not answered for $(since_secs "$_fs" "$fail_counted") s: restarting anyway."
        return 0
      fi
    fi
    sleep 2
    counted=$((counted + 2))
  done
  if [ "$idle" -gt 0 ]; then
    echo "[*] Waited $(since_secs "$start" "$counted") s: restarting."
  else
    echo "[!] The activity switch is still running after $(since_secs "$start" "$counted") s: restarting anyway."
  fi
  return 0
}

do_apply() {
  touch /tmp/hub_updating.flag 2>/dev/null || true
  set_update_state running
  TMPDIR="/tmp/ota_update"
  trap 'rm -f $INSTALLING /tmp/hub_updating.flag; rm -rf "$TMPDIR" 2>/dev/null' EXIT
  # A kill ends the install (the file being copied stays the old one)
  trap 'exit 1' INT TERM
  echo "[*] Starting software update from ${REPO}..."
  LOCAL_VER=$(get_local_version)
  
  rm -rf "$TMPDIR" 2>/dev/null
  mkdir -p "$TMPDIR"

  # 1. Fetch and verify the signed release manifest; its version is the version installed
  NEW_VER=$(fetch_signed_manifest "$TMPDIR")
  if [ $? -ne 0 ] || [ -z "$NEW_VER" ]; then
    echo "[-] Update refused: no valid release signature from ${REPO}. Nothing was changed."
    return 1
  fi
  echo "[+] Release manifest v${NEW_VER} signed by a release key."

  # Major version guard
  LOCAL_MAJOR=$(echo "$LOCAL_VER" | cut -d. -f1)
  NEW_MAJOR=$(echo "$NEW_VER" | cut -d. -f1)
  if [ -n "$LOCAL_MAJOR" ] && [ -n "$NEW_MAJOR" ] && [ "$LOCAL_MAJOR" != "$NEW_MAJOR" ]; then
    if [ -x /data/harmony/bin/hub_daemon ] && [ "$LOCAL_MAJOR" = "1" ]; then
      LOCAL_VER="2.0.0"
      LOCAL_MAJOR="2"
      echo "2.0.0" > /data/harmony/VERSION 2>/dev/null || true
    else
      echo "[-] Major upgrade from v${LOCAL_VER} to v${NEW_VER} requires force deploy script. Aborting."
      return 1
    fi
  fi

  # Guard against update to lower or equal version
  if ! is_version_newer "$NEW_VER" "$LOCAL_VER"; then
    echo "[-] Remote version v${NEW_VER} is not newer than current v${LOCAL_VER}. Aborting update."
    return 1
  fi

  echo "[*] Updating Hub from v${LOCAL_VER} to v${NEW_VER}..."

  # 2. Attempt unified RAM bundle update (hub_ota.tar.gz)
  OTA_BUNDLE_OK=0
  INSTALL_OK=0
  echo "[*] Checking for unified Hub package (hub_ota.tar.gz)..."
  if download_retry "${REPO}/hub_ota.tar.gz?_=${TIMESTAMP}" "$TMPDIR/hub_ota.tar.gz" 35 3; then
    echo "[+] Downloaded unified OTA package to RAM ($TMPDIR/hub_ota.tar.gz)."

    # The package must match the SHA-256 in the signed manifest
    if ! verify_files "hub_ota.tar.gz=$TMPDIR/hub_ota.tar.gz"; then
      echo "[-] hub_ota.tar.gz does not match the signed manifest. Aborting update, nothing was changed."
      rm -rf "$TMPDIR" 2>/dev/null
      return 1
    fi
    echo "[+] hub_ota.tar.gz matches the signed manifest."

    mkdir -p "$TMPDIR/pkg" 2>/dev/null
    if $TAR -xzf "$TMPDIR/hub_ota.tar.gz" -C "$TMPDIR/pkg/" 2>/dev/null; then
      # Verified: prepare the flash (nothing is touched before this point)
      mkdir -p /data/harmony/bin /cache/bin /cache/www 2>/dev/null
      # Prune redundant/bloated binaries to ensure flash space
      rm -f /cache/bin/hub_portal /cache/bin/hub_dhcpd /cache/bin/hub_sntp 2>/dev/null || true
      echo "[*] Installing package files one at a time..."
      if ! install_bundle "$TMPDIR/pkg"; then
        rm -rf "$TMPDIR" 2>/dev/null
        echo "[-] Software update stopped: a file could not be written to flash. Every file is whole, but some are new and some old. Version not changed: free space and run the update again."
        return 1
      fi
      INSTALL_OK=1

      if [ -f "$TMPDIR/pkg/bin/hub_util" ]; then
        link_hub_util
      fi
      link_dropbear
      # The package no longer has the recovery access point: its script, settings and the
      # hub_util links it used go (one still running stops at the next reboot)
      rm -f /data/harmony/recovery_ap.sh /data/harmony/scripts/recovery_ap.sh /data/harmony/bin/recovery_ap.sh \
            /data/harmony/recovery_ap.conf /data/harmony/bin/hub_portal /data/harmony/bin/hub_dhcpd 2>/dev/null || true
      for b in $BUNDLE_BINS; do
        [ -f "$TMPDIR/pkg/bin/$b" ] && rm -f "/cache/bin/$b" 2>/dev/null
      done

      # D. Install extended BusyBox applets strictly to /cache/bin (/bin/sh is NEVER touched)
      rm -f /data/bin/sh /data/bin/ash /data/bin/busybox 2>/dev/null || true
      if [ -x /cache/bin/busybox ]; then
        /cache/bin/busybox --install -s /cache/bin 2>/dev/null || true
        ln -sf /cache/bin/busybox /data/harmony/bin/busybox 2>/dev/null || true
      fi

      # E. WebUI -> /cache/www/ (clean old hashed assets first to prevent flash fill-up)
      if [ -d "$TMPDIR/pkg/www" ]; then
        if [ -d /data/harmony/www ] && [ ! -L /data/harmony/www ]; then
          rm -rf /data/harmony/www 2>/dev/null || true
        fi
        ln -sfn /cache/www /data/harmony/www 2>/dev/null || true
        rm -f /cache/www/assets/index-*.js* /cache/www/assets/web-*.js* 2>/dev/null || true
        rm -rf /cache/www/assets/icons 2>/dev/null || true
        if cp -rf "$TMPDIR/pkg/www/"* /cache/www/ 2>/dev/null; then
          # Prune raw uncompressed assets where .gz exists to save flash
          for f in /cache/www/assets/*.js /cache/www/assets/*.css /cache/www/index.html; do
            [ -f "${f}.gz" ] && rm -f "$f" 2>/dev/null || true
          done
          rm -f /cache/www/assets/*.png.gz /cache/www/*.png.gz 2>/dev/null || true
          echo "[+] WebUI bundle updated in /cache/www (raw files pruned, .gz preserved)."
        else
          echo "[-] WebUI bundle could not be copied to /cache/www (flash full?)."
          INSTALL_OK=0
        fi
      fi

      # F. Icons archive -> /cache/icons.tar.gz (staged above)
      if [ -f "$TMPDIR/pkg/icons.tar.gz" ]; then
        rm -f /data/harmony/icons.tar.gz 2>/dev/null || true
        ln -sf /cache/icons.tar.gz /data/harmony/icons.tar.gz 2>/dev/null || true
        echo "[+] Icons archive updated in /cache/icons.tar.gz (symlinked)."
      fi

      OTA_BUNDLE_OK=1
    fi
    rm -rf "$TMPDIR/hub_ota.tar.gz" "$TMPDIR/pkg" 2>/dev/null
  fi

  # 3. Fallback to individual asset download if unified bundle missing
  OTA_FALLBACK_OK=0
  if [ "$OTA_BUNDLE_OK" != "1" ]; then
    echo "[*] Falling back to modular asset download..."
    FB_FILES=""
    if download_retry "${REPO}/www.tar.gz?_=${TIMESTAMP}" "$TMPDIR/www.tar.gz" 30 3; then
      FB_FILES="$FB_FILES www.tar.gz=$TMPDIR/www.tar.gz"
    fi
    FB_BINS=""
    FB_MISSING=""
    for b in hub_util hub_daemon hub_rf hub_btstack register_ehci eth_fix hub_sync; do
      if download_retry "${REPO}/bin/mips/${b}?_=${TIMESTAMP}" "$TMPDIR/$b" 30 3; then
        FB_FILES="$FB_FILES bin/mips/$b=$TMPDIR/$b"
        FB_BINS="$FB_BINS $b"
      elif grep -q "\"bin/mips/$b\"" "$TMPDIR/manifest.json" 2>/dev/null; then
        FB_MISSING="$FB_MISSING $b"
      fi
    done
    # Every downloaded file must match the signed manifest before any of them is installed
    # shellcheck disable=SC2086
    if [ -z "$FB_FILES" ] || ! verify_files $FB_FILES; then
      echo "[-] Downloaded files missing or not matching the signed manifest. Aborting update, nothing was changed."
      rm -rf "$TMPDIR" 2>/dev/null
      return 1
    fi
    # Every binary the release lists, and hub_daemon in any case, must be installed together
    case " $FB_BINS " in *" hub_daemon "*) ;; *) FB_MISSING="$FB_MISSING hub_daemon" ;; esac
    if [ -n "$FB_MISSING" ]; then
      echo "[-] Could not download:$FB_MISSING. Aborting update, nothing was changed."
      rm -rf "$TMPDIR" 2>/dev/null
      return 1
    fi
    # Verified: prepare the flash (nothing is touched before this point)
    mkdir -p /data/harmony/bin /cache/bin /cache/www 2>/dev/null
    # Prune redundant/bloated binaries to ensure flash space
    rm -f /cache/bin/hub_portal /cache/bin/hub_dhcpd /cache/bin/hub_sntp 2>/dev/null || true
    for b in $FB_BINS; do
      if ! install_file "$TMPDIR/$b" "/data/harmony/bin/$b" 755; then
        rm -rf "$TMPDIR" 2>/dev/null
        echo "[-] Software update stopped: a file could not be written to flash. Every file is whole, but some are new and some old. Version not changed: free space and run the update again."
        return 1
      fi
    done
    INSTALL_OK=1
    for b in $FB_BINS; do
      rm -f "/cache/bin/$b" 2>/dev/null || true
    done
    case " $FB_BINS " in *" hub_util "*) link_hub_util ;; esac
    if [ -f "$TMPDIR/www.tar.gz" ]; then
      mkdir -p /cache/www 2>/dev/null
      [ -d /data/harmony/www ] && [ ! -L /data/harmony/www ] && rm -rf /data/harmony/www 2>/dev/null || true
      ln -sfn /cache/www /data/harmony/www 2>/dev/null || true
      if $TAR -xzf "$TMPDIR/www.tar.gz" -C /cache/www/ 2>/dev/null; then
        echo "[+] WebUI bundle updated in /cache/www."
      else
        echo "[-] WebUI bundle could not be unpacked to /cache/www (flash full?)."
        INSTALL_OK=0
      fi
      rm -f "$TMPDIR/www.tar.gz"
    fi
    OTA_FALLBACK_OK=1
  fi

  # 5. Check if update succeeded before recording version
  if [ "$OTA_BUNDLE_OK" != "1" ] && [ "$OTA_FALLBACK_OK" != "1" ]; then
    echo "[-] Software update failed: could not download packages."
    return 1
  fi
  if [ "$INSTALL_OK" != "1" ]; then
    rm -rf "$TMPDIR" 2>/dev/null
    echo "[-] Software update incomplete: the binaries were installed but not all files. Version not changed, run the update again."
    return 1
  fi

  # 6. Record new version (Hub and remote version metadata), only after a complete install
  if ! { echo "$NEW_VER" > /data/harmony/VERSION.new && mv -f /data/harmony/VERSION.new /data/harmony/VERSION; }; then
    rm -f /data/harmony/VERSION.new 2>/dev/null
    echo "[-] Could not record version v${NEW_VER} in /data/harmony/VERSION."
  fi
  echo "$NEW_VER" > /etc/harmony_version 2>/dev/null || true
  echo "$NEW_VER" > /data/harmony/latest_version
  echo "$NEW_VER" > /data/harmony/remote_ui.version 2>/dev/null || true
  rm -f /data/harmony/update_available.flag /data/harmony/remote_ui.ota.gz /cache/remote_ui.ota.gz \
        /data/harmony/elite_ui.ota.gz /data/harmony/elite_ui.version /cache/elite_ui.ota.gz 2>/dev/null || true
  # What a reboot cleared and a services restart keeps: the icons extracted from the old
  # icons.tar.gz (the new hub_daemon extracts them again when the folder is missing) and a remote
  # image of the old version that hub_rf kept for a resume (sent unverified once its manifest is gone)
  rm -rf /tmp/www/assets/icons /tmp/remote_ui.ota.gz /tmp/remote_manifest.json /tmp/remote_manifest.json.sig 2>/dev/null || true

  # 7. Clean up temporary directory
  rm -rf "$TMPDIR" 2>/dev/null

  # 7. Sync filesystem
  /data/harmony/bin/hub_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || sync 2>/dev/null || true

  wait_activity_switch
  # As build/deploy_hub.py: restarting the services loads the new files, no reboot needed (an
  # update has no kernel modules; a new rcS.local takes effect at the next boot). Detached, its
  # output to /dev/null: the services must not write into the apply log.
  set_update_state restarting "$NEW_VER"
  if [ -x /data/harmony/init.sh ]; then
    echo "[+] Update to v${NEW_VER} applied successfully. Restarting Hub services..."
    rm -f /tmp/harmony_stop 2>/dev/null || true
    /data/harmony/init.sh restart >/dev/null 2>&1 </dev/null &
    return 0
  fi
  echo "[+] Update to v${NEW_VER} applied successfully. Initiating safe Hub reboot..."
  if [ -x /data/harmony/safe_reboot.sh ]; then
    /data/harmony/safe_reboot.sh &
  elif [ -x /data/harmony/scripts/safe_reboot.sh ]; then
    /data/harmony/scripts/safe_reboot.sh &
  else
    killall -9 hub_rf hub_btstack hub_daemon 2>/dev/null || true
    sleep 1
    /data/harmony/bin/hub_sync 2>/dev/null || sync 2>/dev/null || true
    sleep 2
    mount -o remount,ro /data 2>/dev/null || true
    mount -o remount,ro /cache 2>/dev/null || true
    (sleep 1 && reboot -f) >/dev/null 2>&1 &
  fi
}

do_fetch_remote_ota() {
  local target="/tmp/remote_ui.ota.gz"
  local manifest="/tmp/remote_manifest.json"
  local sig="/tmp/remote_manifest.json.sig"
  echo "[*] Downloading remote OTA package from ${REPO} to ${target}..."
  rm -f "${target}" "${manifest}" "${sig}"

  # The image is kept only together with its manifest and signature: hub_rf verifies them before
  # it sends anything to the remote, and treats an image without a manifest as staged by hand.
  if download_retry "${REPO}/manifest.json?_=${TIMESTAMP}" "${manifest}" 10 1 &&
     download_retry "${REPO}/manifest.json.sig?_=${TIMESTAMP}" "${sig}" 10 1; then
    if download_retry "${REPO}/bin/arm/remote_ui.ota.gz?_=${TIMESTAMP}" "${target}" 20 1 ||
       download_retry "${REPO}/bin/arm/elite_ui.ota.gz?_=${TIMESTAMP}" "${target}" 20 1; then
      echo "[+] Remote OTA package downloaded to ${target}."
      return 0
    fi
  fi
  echo "[-] Failed to download the remote OTA package with its signed manifest."
  rm -f "${target}" "${manifest}" "${sig}"
  return 1
}

do_remote_ota() {
  local slot="${1:-1}"
  echo "[*] Triggering RF OTA firmware update for the remote on Slot ${slot}..."

  # Purge any stale flash copies
  rm -f /data/harmony/remote_ui.ota.gz /cache/remote_ui.ota.gz /data/harmony/elite_ui.ota.gz /cache/elite_ui.ota.gz 2>/dev/null || true

  # Trigger hub_rf OTA endpoint on port 8092 (hub_rf downloads, verifies manifest, and streams)
  res=$($WGET -qO- --post-data '' "http://127.0.0.1:8092/api/rf/ota-start?slot=${slot}" 2>/dev/null || true)
  echo "Response: $res"
  
  if echo "$res" | grep -q '"ok":true'; then
    echo "[+] RF OTA started! Streaming firmware chunks over CC2544 RF..."
    while true; do
      sleep 2
      st=$($WGET -qO- "http://127.0.0.1:8092/api/rf/ota-status" 2>/dev/null || true)
      if echo "$st" | grep -q '"status":3'; then
        echo "    Preparing / downloading remote firmware..."
      elif echo "$st" | grep -q '"in_progress":true'; then
        sent=$(echo "$st" | grep -o '"sent_bytes":[0-9]*' | cut -d':' -f2)
        total=$(echo "$st" | grep -o '"total_bytes":[0-9]*' | cut -d':' -f2)
        echo "    Progress: ${sent}/${total} bytes..."
      elif echo "$st" | grep -q '"status":2'; then
        echo "[+] Remote OTA complete! Remote is verifying and rebooting."
        break
      elif echo "$st" | grep -q '"status":-1'; then
        echo "[-] Remote OTA failed: $st"
        break
      else
        echo "Status: $st"
        break
      fi
    done
  else
    echo "[-] Could not initiate RF OTA on slot ${slot}."
    return 1
  fi
}

# One apply at a time: every run uses /tmp/ota_update and its exit removes it, so a second
# run (a double click, two browser tabs) deleted the files the first was installing. The lock
# is a directory with the owner's pid and start time; one left by a run that was killed is taken
# over. The start time (field 22 of /proc/<pid>/stat) tells the owner from a later process that
# got the same pid; a lock written without it falls back to the command line.
APPLY_LOCK=/tmp/ota_update.lock
proc_start() {
  sed 's/.*) //' "/proc/$1/stat" 2>/dev/null | awk '{print $20}'
}
apply_lock_owner_runs() {
  _owner=$(cat "$APPLY_LOCK/pid" 2>/dev/null)
  _start=$(cat "$APPLY_LOCK/start" 2>/dev/null)
  [ -n "$_owner" ] && [ -d "/proc/$_owner" ] || return 1
  if [ -n "$_start" ]; then
    [ "$(proc_start "$_owner")" = "$_start" ]
  else
    grep -q 'update.sh' "/proc/$_owner/cmdline" 2>/dev/null
  fi
}
apply_locked() {
  if ! mkdir "$APPLY_LOCK" 2>/dev/null; then
    if apply_lock_owner_runs; then
      echo "[-] Another software update is running (pid $_owner); not starting a second one."
      return 1
    fi
    rm -rf "$APPLY_LOCK" 2>/dev/null
    if ! mkdir "$APPLY_LOCK" 2>/dev/null; then
      echo "[-] Another software update is starting; not starting a second one."
      return 1
    fi
  fi
  echo "$$" > "$APPLY_LOCK/pid"
  proc_start $$ > "$APPLY_LOCK/start"
  ( do_apply )
  _rc=$?
  # A kill -9 of the subshell skips its EXIT trap: the flag must not stay behind (a running update
  # for 30 minutes), nor the package in RAM (an OOM kill is the likely cause). A "<file>.new" being
  # copied stays on flash until the next apply removes it. The last "[-]" line says it ended (a
  # TERM prints none of its own).
  if [ $_rc -ne 0 ]; then
    rm -f /tmp/hub_updating.flag 2>/dev/null
    rm -rf /tmp/ota_update 2>/dev/null
    echo "[-] The software update ended with exit code $_rc."
    set_update_state failed
  fi
  rm -rf "$APPLY_LOCK" 2>/dev/null
  return $_rc
}

case "$CMD" in
  check)
    do_check
    ;;
  apply)
    apply_locked
    ;;
  fetch-remote|fetch-remote-ota)
    do_fetch_remote_ota
    ;;
  elite|remote)
    do_remote_ota "$SLOT_ARG"
    ;;
  status)
    if [ -f /data/harmony/update_state.conf ]; then
      cat /data/harmony/update_state.conf
    else
      echo "No update check performed yet."
    fi
    ;;
  *)
    apply_locked
    ;;
esac
