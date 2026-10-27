#!/bin/sh
# Harmony Hub User Management Tool (CLI / SSH)
# Manages /data/harmony/config/users.json and /data/harmony/webui_auth.conf

USERS_JSON="/data/harmony/config/users.json"
AUTH_CONF="/data/harmony/webui_auth.conf"
TMP_CONF="/tmp/harmony_auth.tmp"

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

sync_to_json() {
    is_enabled="$1"
    [ -z "$is_enabled" ] && is_enabled=1
    
    echo "{" > "$USERS_JSON.tmp"
    if [ "$is_enabled" = "1" ]; then
        echo '  "auth_enabled": true,' >> "$USERS_JSON.tmp"
    else
        echo '  "auth_enabled": false,' >> "$USERS_JSON.tmp"
    fi
    echo '  "users": [' >> "$USERS_JSON.tmp"

    first=1
    if [ -f "$AUTH_CONF" ]; then
        while IFS=: read -r u p r rest; do
            case "$u" in
                ''|\#*) continue ;;
            esac
            [ -z "$u" ] && continue
            [ -z "$r" ] && r="admin"
            if [ $first -eq 0 ]; then
                echo "," >> "$USERS_JSON.tmp"
            fi
            first=0
            printf '    { "username": "%s", "password": "%s", "role": "%s" }' "$u" "$p" "$r" >> "$USERS_JSON.tmp"
        done < "$AUTH_CONF"
    fi
    echo "" >> "$USERS_JSON.tmp"
    echo '  ]' >> "$USERS_JSON.tmp"
    echo "}" >> "$USERS_JSON.tmp"
    mv -f "$USERS_JSON.tmp" "$USERS_JSON" 2>/dev/null
}

cmd="$1"
user="$2"
pass="$3"

case "$cmd" in
    set-admin)
        if [ -z "$user" ] || [ -z "$pass" ]; then
            echo "Error: Username and password required. Usage: $0 set-admin <username> <password>"
            exit 1
        fi
        rm -f "$TMP_CONF" 2>/dev/null
        found=0
        if [ -f "$AUTH_CONF" ]; then
            while IFS=: read -r u p r rest; do
                case "$u" in
                    ''|\#*) continue ;;
                esac
                [ -z "$u" ] && continue
                [ -z "$r" ] && r="admin"
                if [ "$u" = "$user" ] || [ "$r" = "admin" ]; then
                    echo "$user:$pass:admin" >> "$TMP_CONF"
                    found=1
                else
                    echo "$u:$p:$r" >> "$TMP_CONF"
                fi
            done < "$AUTH_CONF"
        fi
        if [ $found -eq 0 ]; then
            echo "$user:$pass:admin" >> "$TMP_CONF"
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
        rm -f "$TMP_CONF" 2>/dev/null
        found=0
        if [ -f "$AUTH_CONF" ]; then
            while IFS=: read -r u p r rest; do
                case "$u" in
                    ''|\#*) continue ;;
                esac
                [ -z "$u" ] && continue
                [ -z "$r" ] && r="admin"
                if [ "$u" = "$user" ]; then
                    echo "$user:$pass:user" >> "$TMP_CONF"
                    found=1
                else
                    echo "$u:$p:$r" >> "$TMP_CONF"
                fi
            done < "$AUTH_CONF"
        fi
        if [ $found -eq 0 ]; then
            echo "$user:$pass:user" >> "$TMP_CONF"
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
        if [ -f "$AUTH_CONF" ]; then
            while IFS=: read -r u p r rest; do
                case "$u" in
                    ''|\#*) continue ;;
                esac
                [ -z "$r" ] && r="admin"
                [ "$r" = "admin" ] && admin_count=$((admin_count + 1))
                [ "$u" = "$user" ] && target_role="$r"
            done < "$AUTH_CONF"
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
        if [ -f "$AUTH_CONF" ]; then
            while IFS=: read -r u p r rest; do
                case "$u" in
                    ''|\#*) continue ;;
                esac
                [ "$u" = "$user" ] && continue
                [ -z "$r" ] && r="admin"
                echo "$u:$p:$r" >> "$TMP_CONF"
            done < "$AUTH_CONF"
        fi
        mv -f "$TMP_CONF" "$AUTH_CONF"
        sync_to_json 1
        echo "[+] Deleted user '$user'"
        ;;

    list)
        echo "=== Configured User Accounts ==="
        if [ ! -f "$AUTH_CONF" ] || [ ! -s "$AUTH_CONF" ]; then
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
        done < "$AUTH_CONF"
        if [ $has_users -eq 0 ]; then
            echo "Authentication is DISABLED."
        fi
        ;;

    enable-auth)
        if [ ! -f "$AUTH_CONF" ] || [ ! -s "$AUTH_CONF" ]; then
            echo "Error: No user accounts configured. Create an admin account first: $0 set-admin <user> <pass>"
            exit 1
        fi
        sync_to_json 1
        echo "[+] Authentication ENABLED"
        ;;

    disable-auth)
        sync_to_json 0
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
