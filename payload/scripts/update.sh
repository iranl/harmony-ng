#!/bin/sh
# update.sh - Hub OTA software check and apply script

BB=""
if [ -x /cache/bin/busybox ]; then
  BB="/cache/bin/busybox"
elif [ -x /data/codex/bin/busybox ]; then
  BB="/data/codex/bin/busybox"
else
  BB="busybox"
fi

WGET="$BB wget --no-check-certificate"
MD5SUM="$BB md5sum"
TAR="$BB tar"

CMD="${1:-check}"
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
  if [ -f /data/codex/VERSION ]; then
    cat /data/codex/VERSION | tr -d '\r\n '
  elif [ -f /etc/codex_version ]; then
    cat /etc/codex_version | tr -d '\r\n '
  else
    echo "1.4.0"
  fi
}

do_check() {
  LOCAL_VER=$(get_local_version)
  REMOTE_VER=""

  # 1. Try raw GitHub OTA VERSION file with retries
  if download_retry "${REPO}/VERSION?_=${TIMESTAMP}" /tmp/ota_ver 8 3; then
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

  echo "$REMOTE_VER" > /data/codex/latest_version

  HAS_UPDATE=0
  if [ "$REMOTE_VER" != "$LOCAL_VER" ]; then
    HAS_UPDATE=1
    touch /data/codex/update_available.flag
  else
    rm -f /data/codex/update_available.flag
  fi

  cat <<EOF > /data/codex/update_state.conf
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
  echo "[*] Starting software update from ${REPO}..."
  LOCAL_VER=$(get_local_version)
  
  TMPDIR="/tmp/ota_update"
  rm -rf "$TMPDIR" 2>/dev/null
  mkdir -p "$TMPDIR"

  # 1. Fetch remote version
  NEW_VER=""
  if download_retry "${REPO}/VERSION?_=${TIMESTAMP}" "$TMPDIR/VERSION" 8 3; then
    NEW_VER=$(cat "$TMPDIR/VERSION" | tr -d '\r\n v')
  fi
  [ -z "$NEW_VER" ] && NEW_VER=$(cat /data/codex/latest_version 2>/dev/null | tr -d '\r\n v')
  [ -z "$NEW_VER" ] && NEW_VER="1.4.1"

  echo "[*] Updating Hub from v${LOCAL_VER} to v${NEW_VER}..."

  # 2. Update WebUI assets (www.tar.gz) -> Store on /cache/www to preserve /data flash
  echo "[*] Downloading WebUI bundle..."
  if download_retry "${REPO}/www.tar.gz?_=${TIMESTAMP}" "$TMPDIR/www.tar.gz" 30 3; then
    mkdir -p /cache/www/assets 2>/dev/null
    # Remove physical directory on /data if it is not already a symlink
    if [ -d /data/codex/www ] && [ ! -L /data/codex/www ]; then
      rm -rf /data/codex/www 2>/dev/null || true
    fi
    mkdir -p /data/codex 2>/dev/null || true
    ln -sfn /cache/www /data/codex/www 2>/dev/null || true
    $TAR -xzf "$TMPDIR/www.tar.gz" -C /cache/www/ 2>/dev/null
    rm -f "$TMPDIR/www.tar.gz"
    echo "[+] WebUI bundle updated in /cache/www."
  else
    echo "[-] Warning: WebUI bundle not available at ${REPO}/www.tar.gz"
  fi

  # 3. Update Hub Binaries
  mkdir -p /data/codex/bin /cache/bin 2>/dev/null
  if download_retry "${REPO}/bin/mips/busybox?_=${TIMESTAMP}" "$TMPDIR/busybox" 30 3; then
    chmod 755 "$TMPDIR/busybox"
    mv -f "$TMPDIR/busybox" /cache/bin/busybox
    ln -sf /cache/bin/busybox /data/codex/bin/busybox 2>/dev/null || true
  fi
  if [ -x /cache/bin/busybox ]; then
    /cache/bin/busybox --install -s /cache/bin 2>/dev/null || true
    for _app in $(/cache/bin/busybox --list 2>/dev/null); do
      [ ! -e "/bin/$_app" ] && [ ! -e "/usr/bin/$_app" ] && ln -sf /cache/bin/busybox "/usr/bin/$_app" 2>/dev/null || true
    done
  fi

  HUB_BINS="codex_daemon codex_rf codex_btstack codex_dhcpd codex_portal codex_sntp codex_sync mknod register_ehci"
  
  for b in $HUB_BINS; do
    echo "[*] Checking binary: $b..."
    if download_retry "${REPO}/bin/mips/${b}?_=${TIMESTAMP}" "$TMPDIR/$b" 30 3; then
      local_md5=""
      [ -f "/data/codex/bin/$b" ] && local_md5=$($MD5SUM "/data/codex/bin/$b" 2>/dev/null | awk '{print $1}')
      new_md5=$($MD5SUM "$TMPDIR/$b" 2>/dev/null | awk '{print $1}')
      if [ -n "$new_md5" ] && [ "$new_md5" != "$local_md5" ]; then
        echo "    -> Updating $b (MD5: $new_md5)..."
        chmod 755 "$TMPDIR/$b"
        mv -f "$TMPDIR/$b" "/data/codex/bin/$b"
      else
        echo "    -> $b up to date."
        rm -f "$TMPDIR/$b"
      fi
    else
      rm -f "$TMPDIR/$b"
    fi
  done

  # 4. Update Elite Remote OTA image
  echo "[*] Checking Elite remote OTA package..."
  if download_retry "${REPO}/bin/arm/codex_elite.ota.gz?_=${TIMESTAMP}" "$TMPDIR/codex_elite.ota.gz" 35 3; then
    cp -f "$TMPDIR/codex_elite.ota.gz" /data/codex_elite.ota.gz 2>/dev/null || true
    cp -f "$TMPDIR/codex_elite.ota.gz" /tmp/codex_elite.ota.gz 2>/dev/null || true
    rm -f "$TMPDIR/codex_elite.ota.gz"
    echo "[+] Elite remote OTA package cached (/data/codex_elite.ota.gz)."
  fi

  # 5. Record new version
  echo "$NEW_VER" > /data/codex/VERSION
  echo "$NEW_VER" > /etc/codex_version 2>/dev/null || true
  echo "$NEW_VER" > /data/codex/latest_version
  rm -f /data/codex/update_available.flag

  # 6. Clean up temporary directory
  rm -rf "$TMPDIR" 2>/dev/null

  # 7. Sync filesystem
  sync 2>/dev/null || /data/codex/bin/codex_sync 2>/dev/null || true

  echo "[+] Update to v${NEW_VER} applied successfully. Restarting services..."
  # 8. Restart services
  if [ -x /data/codex/init.sh ]; then
    /data/codex/init.sh restart
  fi
}

do_elite_ota() {
  local slot="${1:-1}"
  echo "[*] Triggering RF OTA firmware update for Elite remote on Slot ${slot}..."
  
  # Ensure OTA file exists, download if missing
  if [ ! -f /data/codex_elite.ota.gz ] && [ ! -f /tmp/codex_elite.ota.gz ]; then
    echo "[*] OTA image missing on hub. Downloading from ${REPO}..."
    download_retry "${REPO}/bin/arm/codex_elite.ota.gz?_=${TIMESTAMP}" /tmp/codex_elite.ota.gz 35 3
  fi

  if [ ! -f /data/codex_elite.ota.gz ] && [ ! -f /tmp/codex_elite.ota.gz ]; then
    echo "[-] Error: /data/codex_elite.ota.gz not found and could not be downloaded."
    return 1
  fi

  # Trigger codex_rf OTA endpoint on port 8092
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
        echo "[+] Elite remote OTA complete! Remote is verifying and rebooting."
        break
      elif echo "$st" | grep -q '"status":-1'; then
        echo "[-] Elite remote OTA failed: $st"
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
    do_elite_ota "${2:-1}"
    ;;
  status)
    if [ -f /data/codex/update_state.conf ]; then
      cat /data/codex/update_state.conf
    else
      echo "No update check performed yet."
    fi
    ;;
  *)
    # Default to check if interactive, apply if invoked directly
    do_check
    ;;
esac
