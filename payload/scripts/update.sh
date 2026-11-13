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

AUTH_HEADER=""
if [ -n "$TOKEN_ARG" ]; then
  AUTH_HEADER="--header Authorization: token ${TOKEN_ARG}"
fi

TIMESTAMP=$(date +%s 2>/dev/null || echo 1)

download_retry() {
  local url="$1"
  local dst="$2"
  local timeout="${3:-12}"
  local max_attempts="${4:-3}"
  local i=1
  while [ $i -le $max_attempts ]; do
    rm -f "$dst" 2>/dev/null
    $WGET -q -T "$timeout" $AUTH_HEADER -O "$dst" "$url" 2>/dev/null
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
  if download_retry "${REPO}/v2/VERSION?_=${TIMESTAMP}" /tmp/ota_ver 8 2 || \
     download_retry "${REPO}/VERSION_V2?_=${TIMESTAMP}" /tmp/ota_ver 8 2 || \
     download_retry "${REPO}/VERSION?_=${TIMESTAMP}" /tmp/ota_ver 8 3; then
    REMOTE_VER=$(cat /tmp/ota_ver | tr -d '\r\n v')
  fi

  # 2. Fallback: try GitHub releases API
  if [ -z "$REMOTE_VER" ]; then
    if download_retry "https://api.github.com/repos/iranl/harmony-ng/releases/latest" /tmp/gh_rel 10 2; then
      REMOTE_VER=$(cat /tmp/gh_rel | awk -F'"tag_name":' '{print $2}' | awk -F'"' '{print $2}' | tr -d '\r\n v')
    fi
  fi

  rm -f /tmp/ota_ver /tmp/gh_rel 2>/dev/null

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

do_apply() {
  touch /tmp/hub_updating.flag 2>/dev/null || true
  TMPDIR="/tmp/ota_update"
  trap 'rm -f /tmp/hub_updating.flag; rm -rf "$TMPDIR" 2>/dev/null' EXIT INT TERM
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

  mkdir -p /data/harmony/bin /cache/bin /cache/www 2>/dev/null

  # Prune redundant/bloated binaries to ensure flash space
  rm -f /cache/bin/hub_portal /cache/bin/hub_dhcpd /cache/bin/hub_sntp /data/harmony/bin/mknod 2>/dev/null || true

  # 2. Attempt unified RAM bundle update (hub_ota.tar.gz)
  OTA_BUNDLE_OK=0
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
      echo "[*] Extracting package to flash partitions..."
      # A. Scripts -> /data/harmony/
      if [ -d "$TMPDIR/pkg/scripts" ]; then
        cp -rf "$TMPDIR/pkg/scripts/"* /data/harmony/ 2>/dev/null || true
        chmod 755 /data/harmony/*.sh /data/harmony/rcS.local /data/harmony/dropbear* 2>/dev/null || true
        if [ -f /data/harmony/rcS.local ]; then
          cp -f /data/harmony/rcS.local /etc/init.d/rcS.local 2>/dev/null || true
          chmod 755 /etc/init.d/rcS.local 2>/dev/null || true
        fi
      fi

      # CA Bundle -> /data/harmony/cacert.pem
      if [ -f "$TMPDIR/pkg/config/cacert.pem" ]; then
        cp -f "$TMPDIR/pkg/config/cacert.pem" /data/harmony/cacert.pem 2>/dev/null || true
      elif [ -f "$TMPDIR/pkg/cacert.pem" ]; then
        cp -f "$TMPDIR/pkg/cacert.pem" /data/harmony/cacert.pem 2>/dev/null || true
      fi

      # B. Application binaries -> /data/harmony/bin/
      if [ -f "$TMPDIR/pkg/bin/hub_util" ]; then
        chmod 755 "$TMPDIR/pkg/bin/hub_util"
        cp -f "$TMPDIR/pkg/bin/hub_util" "/data/harmony/bin/hub_util" 2>/dev/null || true
        for _app in hub_sync sync mknod register_ehci eth_fix hub_dhcpd hub_portal hub_sntp; do
          [ -f "/data/harmony/bin/$_app" ] && [ ! -L "/data/harmony/bin/$_app" ] && rm -f "/data/harmony/bin/$_app" 2>/dev/null || true
          ln -sf /data/harmony/bin/hub_util "/data/harmony/bin/$_app" 2>/dev/null || true
        done
      fi
      for b in hub_daemon hub_rf hub_btstack dropbearmulti hub_sync register_ehci eth_fix; do
        if [ -f "$TMPDIR/pkg/bin/$b" ]; then
          chmod 755 "$TMPDIR/pkg/bin/$b"
          cp -f "$TMPDIR/pkg/bin/$b" "/data/harmony/bin/$b" 2>/dev/null || true
          rm -f "/cache/bin/$b" 2>/dev/null || true
        fi
      done

      # C. BusyBox -> /cache/bin/
      if [ -f "$TMPDIR/pkg/bin/busybox" ]; then
        chmod 755 "$TMPDIR/pkg/bin/busybox"
        cp -f "$TMPDIR/pkg/bin/busybox" "/cache/bin/busybox" 2>/dev/null || true
      fi

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
        cp -rf "$TMPDIR/pkg/www/"* /cache/www/ 2>/dev/null || true
        # Prune raw uncompressed assets where .gz exists to save flash
        for f in /cache/www/assets/*.js /cache/www/assets/*.css /cache/www/index.html; do
          [ -f "${f}.gz" ] && rm -f "$f" 2>/dev/null || true
        done
        rm -f /cache/www/assets/*.png.gz /cache/www/*.png.gz 2>/dev/null || true
        echo "[+] WebUI bundle updated in /cache/www (raw files pruned, .gz preserved)."
      fi

      # F. Icons archive -> /cache/icons.tar.gz
      if [ -f "$TMPDIR/pkg/icons.tar.gz" ]; then
        cp -f "$TMPDIR/pkg/icons.tar.gz" /cache/icons.tar.gz 2>/dev/null || true
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
    for b in hub_util hub_daemon hub_rf hub_btstack register_ehci eth_fix hub_sync; do
      if download_retry "${REPO}/bin/mips/${b}?_=${TIMESTAMP}" "$TMPDIR/$b" 30 3; then
        FB_FILES="$FB_FILES bin/mips/$b=$TMPDIR/$b"
        FB_BINS="$FB_BINS $b"
      fi
    done
    # Every downloaded file must match the signed manifest before any of them is installed
    # shellcheck disable=SC2086
    if [ -z "$FB_FILES" ] || ! verify_files $FB_FILES; then
      echo "[-] Downloaded files missing or not matching the signed manifest. Aborting update, nothing was changed."
      rm -rf "$TMPDIR" 2>/dev/null
      return 1
    fi
    if [ -f "$TMPDIR/www.tar.gz" ]; then
      mkdir -p /cache/www 2>/dev/null
      [ -d /data/harmony/www ] && [ ! -L /data/harmony/www ] && rm -rf /data/harmony/www 2>/dev/null || true
      ln -sfn /cache/www /data/harmony/www 2>/dev/null || true
      $TAR -xzf "$TMPDIR/www.tar.gz" -C /cache/www/ 2>/dev/null
      rm -f "$TMPDIR/www.tar.gz"
      echo "[+] WebUI bundle updated in /cache/www."
    fi
    for b in $FB_BINS; do
      chmod 755 "$TMPDIR/$b"
      mv -f "$TMPDIR/$b" "/data/harmony/bin/$b"
      rm -f "/cache/bin/$b" 2>/dev/null || true
      [ "$b" = "hub_daemon" ] && OTA_FALLBACK_OK=1
      if [ "$b" = "hub_util" ]; then
        for _app in hub_sync sync mknod register_ehci eth_fix hub_dhcpd hub_portal hub_sntp; do
          [ -f "/data/harmony/bin/$_app" ] && [ ! -L "/data/harmony/bin/$_app" ] && rm -f "/data/harmony/bin/$_app" 2>/dev/null || true
          ln -sf /data/harmony/bin/hub_util "/data/harmony/bin/$_app" 2>/dev/null || true
        done
      fi
    done
  fi

  # 5. Check if update succeeded before recording version
  if [ "$OTA_BUNDLE_OK" != "1" ] && [ "$OTA_FALLBACK_OK" != "1" ]; then
    echo "[-] Software update failed: could not download packages."
    return 1
  fi

  # 6. Record new version (Hub and remote version metadata)
  echo "$NEW_VER" > /data/harmony/VERSION
  echo "$NEW_VER" > /etc/harmony_version 2>/dev/null || true
  echo "$NEW_VER" > /data/harmony/latest_version
  echo "$NEW_VER" > /data/harmony/remote_ui.version 2>/dev/null || true
  rm -f /data/harmony/update_available.flag /data/harmony/remote_ui.ota.gz /cache/remote_ui.ota.gz \
        /data/harmony/elite_ui.ota.gz /data/harmony/elite_ui.version /cache/elite_ui.ota.gz 2>/dev/null || true

  # 7. Clean up temporary directory
  rm -rf "$TMPDIR" 2>/dev/null

  # 7. Sync filesystem
  /data/harmony/bin/hub_sync 2>/dev/null || /cache/bin/sync 2>/dev/null || sync 2>/dev/null || true

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

case "$CMD" in
  check)
    do_check
    ;;
  apply)
    do_apply
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
    do_apply
    ;;
esac
