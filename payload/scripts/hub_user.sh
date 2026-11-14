#!/bin/sh
# Harmony Hub User Management Tool (CLI / SSH)
# Manages /data/harmony/config/users.json and /data/harmony/webui_auth.conf

USERS_JSON="/data/harmony/config/users.json"
AUTH_CONF="/data/harmony/webui_auth.conf"
TMP_CONF="/tmp/harmony_auth.tmp"
ACCOUNTS="/tmp/harmony_auth_accounts.tmp"

mkdir -p /data/harmony/config 2>/dev/null

show_help() {
    echo "Harmony Hub User Management"
    echo "Usage: $0 <command> [arguments]"
    echo ""
    echo "Commands:"
    echo "  set-admin <user> <pass>  Set or update admin account (enables auth)"
    echo "  add-user <user> <pass>   Add or update non-admin user account"
    echo "  delete <user>            Remove user account"
    echo "  list                     Show current user accounts and roles"
    echo "  enable-auth              Enable HTTP authentication"
    echo "  disable-auth             Disable HTTP authentication"
    echo ""
}

SHA256SUM="sha256sum"
if [ -x /cache/bin/busybox ]; then
  SHA256SUM="/cache/bin/busybox sha256sum"
elif [ -x /data/harmony/bin/busybox ]; then
  SHA256SUM="/data/harmony/bin/busybox sha256sum"
fi

calc_ha1() {
    _u="$1"
    _p="$2"
    printf '%s:%s:%s' "$_u" "Harmony Hub" "$_p" | $SHA256SUM 2>/dev/null | awk '{print $1}'
}

# Account lines (user:ha1:role) stored in users.json. hub_daemon keeps the
# accounts there while auth is off and writes "# auth disabled" to
# webui_auth.conf, so the conf alone cannot tell which accounts exist.
json_accounts() {
    [ -f "$USERS_JSON" ] || return 0
    awk '
        {
            line = $0
            while (match(line, /"(username|ha1|role)"[ \t]*:[ \t]*"[^"]*"/)) {
                kv = substr(line, RSTART, RLENGTH)
                line = substr(line, RSTART + RLENGTH)
                k = kv; sub(/^"/, "", k); sub(/".*/, "", k)
                v = kv; sub(/^"[^"]*"[ \t]*:[ \t]*"/, "", v); sub(/"$/, "", v)
                if (k == "username") {
                    if (u != "" && (length(h) == 64 || length(h) == 32)) print u ":" h ":" r
                    u = v; h = ""; r = "admin"
                } else if (k == "ha1") {
                    h = v
                } else if (k == "role") {
                    r = v
                }
            }
        }
        END { if (u != "" && (length(h) == 64 || length(h) == 32)) print u ":" h ":" r }
    ' "$USERS_JSON"
}

# Account lines of webui_auth.conf, comments and blank lines left out
conf_accounts() {
    [ -f "$AUTH_CONF" ] || return 0
    while IFS=: read -r u p r rest; do
        case "$u" in
            ''|\#*) continue ;;
        esac
        echo "$u:$p:$r"
    done < "$AUTH_CONF"
}

# Current accounts into $ACCOUNTS: from webui_auth.conf, or from users.json
# when the conf has none (auth turned off by this tool or by the web UI)
load_accounts() {
    conf_accounts > "$ACCOUNTS"
    if [ ! -s "$ACCOUNTS" ]; then
        json_accounts > "$ACCOUNTS"
    fi
}

sync_to_json() {
    is_enabled="$1"
    [ -z "$is_enabled" ] && is_enabled=1
    src="$2"
    [ -z "$src" ] && src="$AUTH_CONF"
    
    echo "{" > "$USERS_JSON.tmp"
    if [ "$is_enabled" = "1" ]; then
        echo '  "auth_enabled": true,' >> "$USERS_JSON.tmp"
    else
        echo '  "auth_enabled": false,' >> "$USERS_JSON.tmp"
    fi
    echo '  "users": [' >> "$USERS_JSON.tmp"

    first=1
    if [ -f "$src" ]; then
        while IFS=: read -r u p r rest; do
            case "$u" in
                ''|\#*) continue ;;
            esac
            [ -z "$u" ] && continue
            [ -z "$r" ] && r="admin"
            # If not already a 64 or 32-char hash, compute HA1
            if [ ${#p} -ne 64 ] && [ ${#p} -ne 32 ]; then
                p=$(calc_ha1 "$u" "$p")
            fi
            if [ $first -eq 0 ]; then
                echo "," >> "$USERS_JSON.tmp"
            fi
            first=0
            printf '    { "username": "%s", "ha1": "%s", "role": "%s" }' "$u" "$p" "$r" >> "$USERS_JSON.tmp"
        done < "$src"
    fi
    echo "" >> "$USERS_JSON.tmp"
    echo '  ]' >> "$USERS_JSON.tmp"
    echo "}" >> "$USERS_JSON.tmp"
    mv -f "$USERS_JSON.tmp" "$USERS_JSON" 2>/dev/null
}

cmd="$1"
user="$2"
pass="$3"

trap 'rm -f "$ACCOUNTS"' EXIT
load_accounts

case "$cmd" in
    set-admin)
        if [ -z "$user" ] || [ -z "$pass" ]; then
            echo "Error: Username and password required. Usage: $0 set-admin <username> <password>"
            exit 1
        fi
        ha1=$(calc_ha1 "$user" "$pass")
        rm -f "$TMP_CONF" 2>/dev/null
        found=0
        if [ -f "$ACCOUNTS" ]; then
            while IFS=: read -r u p r rest; do
                case "$u" in
                    ''|\#*) continue ;;
                esac
                [ -z "$u" ] && continue
                [ -z "$r" ] && r="admin"
                if [ "$u" = "$user" ] || [ "$r" = "admin" ]; then
                    echo "$user:$ha1:admin" >> "$TMP_CONF"
                    found=1
                else
                    echo "$u:$p:$r" >> "$TMP_CONF"
                fi
            done < "$ACCOUNTS"
        fi
        if [ $found -eq 0 ]; then
            echo "$user:$ha1:admin" >> "$TMP_CONF"
        fi
        mv -f "$TMP_CONF" "$AUTH_CONF"
        sync_to_json 1
        echo "[+] Admin account configured: $user (Auth enabled)"
        ;;

    add-user)
        if [ -z "$user" ] || [ -z "$pass" ]; then
            echo "Error: Username and password required. Usage: $0 add-user <username> <password>"
            exit 1
        fi
        ha1=$(calc_ha1 "$user" "$pass")
        rm -f "$TMP_CONF" 2>/dev/null
        found=0
        if [ -f "$ACCOUNTS" ]; then
            while IFS=: read -r u p r rest; do
                case "$u" in
                    ''|\#*) continue ;;
                esac
                [ -z "$u" ] && continue
                [ -z "$r" ] && r="admin"
                if [ "$u" = "$user" ]; then
                    echo "$user:$ha1:user" >> "$TMP_CONF"
                    found=1
                else
                    echo "$u:$p:$r" >> "$TMP_CONF"
                fi
            done < "$ACCOUNTS"
        fi
        if [ $found -eq 0 ]; then
            echo "$user:$ha1:user" >> "$TMP_CONF"
        fi
        mv -f "$TMP_CONF" "$AUTH_CONF"
        sync_to_json 1
        echo "[+] Non-admin user account configured: $user (role: user)"
        ;;

    delete)
        if [ -z "$user" ]; then
            echo "Error: Username required. Usage: $0 delete <username>"
            exit 1
        fi
        admin_count=0
        target_role=""
        if [ -f "$ACCOUNTS" ]; then
            while IFS=: read -r u p r rest; do
                case "$u" in
                    ''|\#*) continue ;;
                esac
                [ -z "$r" ] && r="admin"
                [ "$r" = "admin" ] && admin_count=$((admin_count + 1))
                [ "$u" = "$user" ] && target_role="$r"
            done < "$ACCOUNTS"
        fi
        if [ -z "$target_role" ]; then
            echo "Error: User '$user' not found"
            exit 1
        fi
        if [ "$target_role" = "admin" ] && [ $admin_count -le 1 ]; then
            echo "Error: Cannot delete the sole admin account"
            exit 1
        fi
        rm -f "$TMP_CONF" 2>/dev/null
        if [ -f "$ACCOUNTS" ]; then
            while IFS=: read -r u p r rest; do
                case "$u" in
                    ''|\#*) continue ;;
                esac
                [ "$u" = "$user" ] && continue
                [ -z "$r" ] && r="admin"
                echo "$u:$p:$r" >> "$TMP_CONF"
            done < "$ACCOUNTS"
        fi
        mv -f "$TMP_CONF" "$AUTH_CONF"
        sync_to_json 1
        echo "[+] Deleted user '$user'"
        ;;

    list)
        echo "=== Configured User Accounts ==="
        if [ ! -s "$ACCOUNTS" ]; then
            echo "No user accounts configured. Authentication is DISABLED."
            exit 0
        fi
        has_users=0
        while IFS=: read -r u p r rest; do
            case "$u" in
                ''|\#*) continue ;;
            esac
            [ -z "$u" ] && continue
            [ -z "$r" ] && r="admin"
            echo " - Username: $u | Role: $r"
            has_users=1
        done < "$ACCOUNTS"
        if [ $has_users -eq 0 ] || grep -q '"auth_enabled"[[:space:]]*:[[:space:]]*false' "$USERS_JSON" 2>/dev/null; then
            echo "Authentication is DISABLED."
        else
            echo "Authentication is ENABLED."
        fi
        ;;

    enable-auth)
        if [ ! -s "$ACCOUNTS" ]; then
            echo "Error: No user accounts configured. Create an admin account first: $0 set-admin <user> <pass>"
            exit 1
        fi
        cp -f "$ACCOUNTS" "$TMP_CONF" && mv -f "$TMP_CONF" "$AUTH_CONF"
        sync_to_json 1
        if [ -z "$(json_accounts)" ]; then
            echo "Error: $USERS_JSON was not written; authentication state unchanged"
            exit 1
        fi
        echo "[+] Authentication ENABLED"
        ;;

    disable-auth)
        # Keep the accounts in users.json (as the web UI does); only the flag changes
        sync_to_json 0 "$ACCOUNTS"
        if [ -f "$AUTH_CONF" ]; then
            echo "# auth disabled" > "$AUTH_CONF"
        fi
        echo "[+] Authentication DISABLED"
        ;;

    *)
        show_help
        exit 1
        ;;
esac
