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

CA_BUNDLE=""
for c in /data/harmony/cacert.pem /opt/luaworks/cacert.pem /etc/ssl/cert.pem /etc/ssl/certs/ca-certificates.crt; do
  if [ -s "$c" ]; then
    CA_BUNDLE="$c"
    break
  fi
done

if [ -n "$CA_BUNDLE" ]; then
  export SSL_CERT_FILE="$CA_BUNDLE"
  export SSL_CERT_DIR="$(dirname "$CA_BUNDLE")"
  if $BB wget --help 2>&1 | grep -q 'ca-certificate'; then
    WGET="$BB wget --ca-certificate=$CA_BUNDLE"
  else
    WGET="$BB wget"
  fi
else
  WGET="$BB wget --no-check-certificate"
fi
MD5SUM="$BB md5sum"
TAR="$BB tar"

CMD="${1:-apply}"
REPO_ARG="${2:-}"
TOKEN_ARG="${3:-}"

DEFAULT_OTA="https://raw.githubusercontent.com/iranl/harmony-ng/ota"
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

  # 1. Fetch remote version
  NEW_VER=""
  if download_retry "${REPO}/v2/VERSION?_=${TIMESTAMP}" "$TMPDIR/VERSION" 8 2 || \
     download_retry "${REPO}/VERSION_V2?_=${TIMESTAMP}" "$TMPDIR/VERSION" 8 2 || \
     download_retry "${REPO}/VERSION?_=${TIMESTAMP}" "$TMPDIR/VERSION" 8 3; then
    NEW_VER=$(cat "$TMPDIR/VERSION" | tr -d '\r\n v')
  fi
  [ -z "$NEW_VER" ] && [ -f /data/harmony/latest_version ] && NEW_VER=$(cat /data/harmony/latest_version 2>/dev/null | tr -d '\r\n v')
  [ -z "$NEW_VER" ] && NEW_VER="2.0.0"

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

    # Verify MD5 checksum (MANDATORY)
    echo "[*] Fetching and verifying mandatory MD5 checksum..."
    if ! download_retry "${REPO}/hub_ota.tar.gz.md5?_=${TIMESTAMP}" "$TMPDIR/hub_ota.tar.gz.md5" 8 2 && \
       ! download_retry "${REPO}/hub_ota.md5?_=${TIMESTAMP}" "$TMPDIR/hub_ota.tar.gz.md5" 8 2; then
      echo "[-] Mandatory checksum file (.md5) missing or unreachable from ${REPO}. Aborting update."
      rm -rf "$TMPDIR" 2>/dev/null
      return 1
    fi

    EXPECTED_MD5=$(awk '{print $1}' "$TMPDIR/hub_ota.tar.gz.md5" | tr -d '\r\n ')
    if [ -z "$EXPECTED_MD5" ]; then
      echo "[-] Invalid or empty MD5 checksum file. Aborting update."
      rm -rf "$TMPDIR" 2>/dev/null
      return 1
    fi

    ACTUAL_MD5=$($MD5SUM "$TMPDIR/hub_ota.tar.gz" 2>/dev/null | awk '{print $1}')
    if [ "$EXPECTED_MD5" != "$ACTUAL_MD5" ]; then
      echo "[-] Checksum mismatch for hub_ota.tar.gz (expected $EXPECTED_MD5, got $ACTUAL_MD5). Aborting."
      rm -rf "$TMPDIR" 2>/dev/null
      return 1
    fi
    echo "[+] MD5 checksum verified successfully ($ACTUAL_MD5)."

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
    if download_retry "${REPO}/www.tar.gz?_=${TIMESTAMP}" "$TMPDIR/www.tar.gz" 30 3; then
      mkdir -p /cache/www 2>/dev/null
      [ -d /data/harmony/www ] && [ ! -L /data/harmony/www ] && rm -rf /data/harmony/www 2>/dev/null || true
      ln -sfn /cache/www /data/harmony/www 2>/dev/null || true
      $TAR -xzf "$TMPDIR/www.tar.gz" -C /cache/www/ 2>/dev/null
      rm -f "$TMPDIR/www.tar.gz"
      echo "[+] WebUI bundle updated in /cache/www."
    fi

    for b in hub_util hub_daemon hub_rf hub_btstack register_ehci eth_fix hub_sync; do
      if download_retry "${REPO}/bin/mips/${b}?_=${TIMESTAMP}" "$TMPDIR/$b" 30 3; then
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
      fi
    done
  fi

  # 4. Update remote OTA image (store on /data/harmony)
  echo "[*] Checking remote OTA package..."
  # elite_ui.ota.gz: the name before the elite -> remote rename, on an OTA branch not yet republished
  if download_retry "${REPO}/bin/arm/remote_ui.ota.gz?_=${TIMESTAMP}" "$TMPDIR/remote_ui.ota.gz" 35 3 ||
     download_retry "${REPO}/bin/arm/elite_ui.ota.gz?_=${TIMESTAMP}" "$TMPDIR/remote_ui.ota.gz" 35 3; then
    cp -f "$TMPDIR/remote_ui.ota.gz" /data/harmony/remote_ui.ota.gz 2>/dev/null || true
    echo "$NEW_VER" > /data/harmony/remote_ui.version 2>/dev/null || true
    rm -f /cache/remote_ui.ota.gz /data/harmony/elite_ui.ota.gz /data/harmony/elite_ui.version /cache/elite_ui.ota.gz 2>/dev/null || true
    rm -f "$TMPDIR/remote_ui.ota.gz"
    echo "[+] Remote OTA package cached (/data/harmony/remote_ui.ota.gz)."
  fi

  # 5. Check if update succeeded before recording version
  if [ "$OTA_BUNDLE_OK" != "1" ] && [ "$OTA_FALLBACK_OK" != "1" ]; then
    echo "[-] Software update failed: could not download packages."
    return 1
  fi

  # 6. Record new version
  echo "$NEW_VER" > /data/harmony/VERSION
  echo "$NEW_VER" > /etc/harmony_version 2>/dev/null || true
  echo "$NEW_VER" > /data/harmony/latest_version
  rm -f /data/harmony/update_available.flag 2>/dev/null || true

  # 6. Clean up temporary directory
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

do_remote_ota() {
  local slot="${1:-1}"
  echo "[*] Triggering RF OTA firmware update for the remote on Slot ${slot}..."
  
  if [ ! -f /cache/remote_ui.ota.gz ] && [ ! -f /data/harmony/remote_ui.ota.gz ] && [ ! -f /tmp/remote_ui.ota.gz ]; then
    echo "[*] OTA image missing on hub. Downloading from ${REPO}..."
    download_retry "${REPO}/bin/arm/remote_ui.ota.gz?_=${TIMESTAMP}" /cache/remote_ui.ota.gz 35 3 ||
      download_retry "${REPO}/bin/arm/elite_ui.ota.gz?_=${TIMESTAMP}" /cache/remote_ui.ota.gz 35 3
    ln -sf /cache/remote_ui.ota.gz /data/harmony/remote_ui.ota.gz 2>/dev/null || true
  fi

  OTA_FILE=""
  for f in /cache/remote_ui.ota.gz /data/harmony/remote_ui.ota.gz /tmp/remote_ui.ota.gz; do
    if [ -f "$f" ]; then OTA_FILE="$f"; break; fi
  done

  if [ -z "$OTA_FILE" ]; then
    echo "[-] Error: remote_ui.ota.gz not found and could not be downloaded."
    return 1
  fi

  # Trigger hub_rf OTA endpoint on port 8092
  res=$($WGET -qO- --post-data '' "http://127.0.0.1:8092/api/rf/ota-start?slot=${slot}" 2>/dev/null || true)
  echo "Response: $res"
  
  if echo "$res" | grep -q '"ok":true'; then
    echo "[+] RF OTA started! Streaming firmware chunks over CC2544 RF..."
    while true; do
      sleep 2
      st=$($WGET -qO- "http://127.0.0.1:8092/api/rf/ota-status" 2>/dev/null || true)
      if echo "$st" | grep -q '"in_progress":true'; then
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
  elite|remote)
    do_remote_ota "${2:-1}"
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
