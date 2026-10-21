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

  # 2. Update WebUI assets (www.tar.gz)
  echo "[*] Downloading WebUI bundle..."
  if download_retry "${REPO}/www.tar.gz?_=${TIMESTAMP}" "$TMPDIR/www.tar.gz" 30 3; then
    mkdir -p /data/codex/www/assets
    $TAR -xzf "$TMPDIR/www.tar.gz" -C /data/codex/www/ 2>/dev/null
    rm -f "$TMPDIR/www.tar.gz"
    echo "[+] WebUI bundle updated."
  else
    echo "[-] Warning: WebUI bundle not available at ${REPO}/www.tar.gz"
  fi

  # 3. Update Hub Binaries
  mkdir -p /data/codex/bin
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

  # 4. Record new version
  echo "$NEW_VER" > /data/codex/VERSION
  echo "$NEW_VER" > /etc/codex_version 2>/dev/null || true
  echo "$NEW_VER" > /data/codex/latest_version
  rm -f /data/codex/update_available.flag

  # 5. Clean up temporary directory
  rm -rf "$TMPDIR" 2>/dev/null

  # 6. Sync filesystem
  sync 2>/dev/null || /data/codex/bin/codex_sync 2>/dev/null || true

  echo "[+] Update to v${NEW_VER} applied successfully. Restarting services..."
  # 7. Restart services
  if [ -x /data/codex/init.sh ]; then
    /data/codex/init.sh restart
  fi
}

case "$CMD" in
  check)
    do_check
    ;;
  apply)
    do_apply
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
