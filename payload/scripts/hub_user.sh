#!/bin/sh
# Harmony Hub User Management Tool (CLI / SSH)
# Manages /data/harmony/config/users.json and /data/harmony/webui_auth.conf

USERS_JSON="/data/harmony/config/users.json"
AUTH_CONF="/data/harmony/webui_auth.conf"
# next to the file it replaces: mv is then a rename on the same file system
TMP_CONF="/data/harmony/webui_auth.conf.tmp"
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

# A usable hash: 64 hex digits (SHA-256). An empty or short one means the hash tool is missing
# or failed, and an account with it could never log in.
valid_ha1() {
    case "$1" in
        ''|*[!0-9a-f]*) return 1 ;;
    esac
    [ ${#1} -eq 64 ]
}

# User names as hub_daemon accepts them; ":" would break the conf format and a quote the JSON
valid_name() {
    case "$1" in
        ''|*[!A-Za-z0-9_.-]*) return 1 ;;
    esac
    [ ${#1} -lt 32 ]
}

need_ha1() {
    ha1=$(calc_ha1 "$1" "$2")
    if ! valid_ha1 "$ha1"; then
        echo "Error: could not compute the password hash (sha256sum missing?); nothing changed"
        exit 1
    fi
}

# Replace the conf with $TMP_CONF; stop when it cannot be written
install_conf() {
    if ! mv -f "$TMP_CONF" "$AUTH_CONF" 2>/dev/null; then
        rm -f "$TMP_CONF" 2>/dev/null
        echo "Error: could not write $AUTH_CONF; nothing changed"
        exit 1
    fi
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
    
    # no account left: authentication cannot be on
    if [ "$is_enabled" = "1" ] && ! grep -q '^[^#]' "$src" 2>/dev/null; then
        is_enabled=0
    fi
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
                if ! valid_ha1 "$p"; then
                    rm -f "$USERS_JSON.tmp"
                    echo "Error: could not compute the password hash for '$u' (sha256sum missing?)"
                    return 1
                fi
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
    if ! mv -f "$USERS_JSON.tmp" "$USERS_JSON" 2>/dev/null; then
        echo "Error: could not write $USERS_JSON"
        return 1
    fi
}

# Removes the access tokens owned by account $1 from tokens.json (an account deleted here, or a
# name added again: tokens a deleted account of that name left must not come back). The file is
# rewritten through a temporary file and mv, only when a token was removed; a file that does not
# look like a token list is left alone. Prints how many were removed.
remove_owner_tokens() {
    _tj="${USERS_JSON%/*}/tokens.json"
    [ -f "$_tj" ] || return 0
    awk -v owner="$1" '
        # The value of the "owner" key of one token object, "" when it has none. A key is a
        # string at the top level of the object followed by ":". Text inside a value (a name with
        # "owner" in it, whose quotes the writers escape as \") and keys of nested values never
        # count. Returned as written: account names have no characters JSON escapes.
        function owner_of(obj,    len, j, k, ch, d, ins, es, str, want) {
            len = length(obj); d = 0; ins = 0; es = 0; str = ""; want = 0
            for (j = 1; j <= len; j++) {
                ch = substr(obj, j, 1)
                if (ins) {
                    if (es) { es = 0; str = str ch }
                    else if (ch == "\\") { es = 1; str = str ch }
                    else if (ch == "\"") {
                        ins = 0
                        if (d == 1) {
                            if (want) return str
                            k = j + 1
                            while (k <= len && index(" \t\r\n", substr(obj, k, 1)) > 0) k++
                            if (str == "owner" && substr(obj, k, 1) == ":") want = 1
                        }
                    } else str = str ch
                    continue
                }
                if (ch == "\"") { ins = 1; str = ""; continue }
                # an owner value that is not a string (a number, null, an object): no owner
                if (want && ch != ":" && index(" \t\r\n", ch) == 0) want = 0
                if (ch == "{" || ch == "[") d++
                else if (ch == "}" || ch == "]") d--
            }
            return ""
        }
        { s = s $0 "\n" }
        END {
            p = index(s, "\"tokens\"")
            if (p == 0) exit 2
            rest = substr(s, p + 8)
            b = index(rest, "[")
            if (b == 0) exit 2
            body = substr(rest, b + 1)
            n = length(body); depth = 0; instr = 0; esc = 0; start = 0; kept = ""; removed = 0; closed = 0
            for (i = 1; i <= n; i++) {
                c = substr(body, i, 1)
                if (instr) {
                    if (esc) esc = 0
                    else if (c == "\\") esc = 1
                    else if (c == "\"") instr = 0
                    continue
                }
                if (c == "\"") { instr = 1; continue }
                if (c == "{") { if (depth == 0) start = i; depth++ }
                else if (c == "}") {
                    depth--
                    if (depth == 0) {
                        obj = substr(body, start, i - start + 1)
                        if (owner_of(obj) == owner) removed++
                        else kept = kept (kept == "" ? "" : ",\n") "  " obj
                    }
                } else if (c == "]" && depth == 0) { closed = 1; break }
            }
            if (!closed) exit 2
            if (removed == 0) exit 1
            printf "{\n\"tokens\": [\n%s\n]\n}\n", kept > out
            print removed
        }
    ' out="$_tj.tmp" "$_tj" > "$TMP_CONF.removed"
    _rc=$?
    if [ "$_rc" = "0" ] && [ -s "$_tj.tmp" ]; then
        mv -f "$_tj.tmp" "$_tj" && echo "[+] Removed $(cat "$TMP_CONF.removed") access token(s) of '$1'"
    else
        rm -f "$_tj.tmp"
        [ "$_rc" = "2" ] && echo "[!] $_tj does not look like a token list: tokens of '$1' not removed"
    fi
    rm -f "$TMP_CONF.removed"
    return 0
}

cmd="$1"
user="$2"
pass="$3"

trap 'rm -f "$ACCOUNTS"' EXIT
load_accounts

# A command that turns authentication on prints the accounts and access tokens to check, as
# hub_daemon answers at that switch: while it was off anybody on the network was admin. The account
# the command sets (set-admin, add-user) is not listed.
CHECK_WAS_ON=0
grep -q '"auth_enabled"[[:space:]]*:[[:space:]]*true' "$USERS_JSON" 2>/dev/null && CHECK_WAS_ON=1
# no users.json: hub_daemon counts accounts in webui_auth.conf as authentication on
[ ! -f "$USERS_JSON" ] && [ -n "$(conf_accounts)" ] && CHECK_WAS_ON=1
print_switch_check() {
    [ "$CHECK_WAS_ON" = "0" ] || return 0
    grep -q '"auth_enabled"[[:space:]]*:[[:space:]]*true' "$USERS_JSON" 2>/dev/null || return 0
    _own=""
    case "$cmd" in set-admin|add-user) _own="$user" ;; esac
    _list=$(json_accounts | while IFS=: read -r u h r; do [ "$u" = "$_own" ] || echo "    account $u ($r)"; done)
    tokens_json="${USERS_JSON%/*}/tokens.json"
    if [ -f "$tokens_json" ]; then
        _list="$_list
$(awk '
            {
                line = $0
                while (match(line, /"(id|name|role)"[ \t]*:[ \t]*"[^"]*"/)) {
                    kv = substr(line, RSTART, RLENGTH)
                    line = substr(line, RSTART + RLENGTH)
                    k = kv; sub(/^"/, "", k); sub(/".*/, "", k)
                    v = kv; sub(/^"[^"]*"[ \t]*:[ \t]*"/, "", v); sub(/"$/, "", v)
                    if (k == "id") { if (id != "") print "    token " id " " nm " (" rl ")"; id = v; nm = ""; rl = "" }
                    else if (k == "name") nm = v
                    else if (k == "role") rl = v
                }
            }
            END { if (id != "") print "    token " id " " nm " (" rl ")" }
        ' "$tokens_json")"
    fi
    _list=$(echo "$_list" | grep -v '^$')
    if [ -n "$_list" ]; then
        echo "[!] Authentication is on. Check the accounts and access tokens on the Hub and delete any you"
        echo "    do not recognise (web UI Users page, or DELETE /api/users and /api/auth/tokens):"
        echo "$_list"
    fi
}
trap 'rc=$?; [ "$rc" = "0" ] && print_switch_check; rm -f "$ACCOUNTS"; exit $rc' EXIT
trap 'rm -f "$ACCOUNTS" "$TMP_CONF" "$USERS_JSON.tmp" "${USERS_JSON%/*}/tokens.json.tmp"; exit 1' INT TERM HUP

case "$cmd" in
    set-admin)
        if [ -z "$user" ] || [ -z "$pass" ]; then
            echo "Error: Username and password required. Usage: $0 set-admin <username> <password>"
            exit 1
        fi
        if ! valid_name "$user"; then
            echo "Error: user names use letters, digits, '.', '_' and '-' only (at most 31)"
            exit 1
        fi
        need_ha1 "$user" "$pass"
        : > "$TMP_CONF" || exit 1
        found=0
        if [ -f "$ACCOUNTS" ]; then
            while IFS=: read -r u p r rest; do
                case "$u" in
                    ''|\#*) continue ;;
                esac
                [ -z "$u" ] && continue
                [ -z "$r" ] && r="admin"
                # this account only: other accounts (other admins too) stay as they are
                if [ "$u" = "$user" ]; then
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
        install_conf
        sync_to_json 1 || exit 1
        echo "[+] Admin account configured: $user (Auth enabled)"
        # a name that no account had: tokens left by a deleted account of that name go
        if [ "$found" = "0" ]; then remove_owner_tokens "$user"; fi
        ;;

    add-user)
        if [ -z "$user" ] || [ -z "$pass" ]; then
            echo "Error: Username and password required. Usage: $0 add-user <username> <password>"
            exit 1
        fi
        if ! valid_name "$user"; then
            echo "Error: user names use letters, digits, '.', '_' and '-' only (at most 31)"
            exit 1
        fi
        need_ha1 "$user" "$pass"
        : > "$TMP_CONF" || exit 1
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
        install_conf
        sync_to_json 1 || exit 1
        echo "[+] Non-admin user account configured: $user (role: user)"
        if [ "$found" = "0" ]; then remove_owner_tokens "$user"; fi
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
        # created empty first: deleting the last account leaves an empty file, not none
        : > "$TMP_CONF" || exit 1
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
        install_conf
        sync_to_json 1 || exit 1
        echo "[+] Deleted user '$user'"
        remove_owner_tokens "$user"
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
        cp -f "$ACCOUNTS" "$TMP_CONF" || exit 1
        install_conf
        sync_to_json 1 || exit 1
        if [ -z "$(json_accounts)" ]; then
            echo "Error: $USERS_JSON was not written; authentication state unchanged"
            exit 1
        fi
        echo "[+] Authentication ENABLED"
        ;;

    disable-auth)
        # Keep the accounts in users.json (as the web UI does); only the flag changes
        sync_to_json 0 "$ACCOUNTS" || exit 1
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
