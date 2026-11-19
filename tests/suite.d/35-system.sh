# Hub system: SSH logins, network time, factory reset

# Every path that starts SSH on the Hub starts dropbear for key logins only (-s -g)
python3 "$SCRIPT_DIR/test_hub_ssh_keys_only.py"
# SNTP: a step of more than an hour needs two servers that agree (fake servers on 127.0.0.11-15, ports 9261-9265);
# built with the UTC build date as build/compile_all.sh passes it, and without it from a build host whose
# local date is already tomorrow (east of UTC after midnight)
build_test -Wall -Wextra -O2 -pthread -I"$REPO_ROOT/payload/source" -DSNTP_UNCONFIRMED_FILE="\"$BUILD_DIR/test_sntp_unconfirmed\"" \
    -DHUB_BUILD_DATE_UTC="$(date -u +%Y%m%d)" \
    -o "$BUILD_DIR/test_sntp_agreement" "$SCRIPT_DIR/test_sntp_agreement.c" "$REPO_ROOT/payload/source/hub_ntp.c" && "$BUILD_DIR/test_sntp_agreement"
build_test -Wall -Wextra -O2 -pthread -I"$REPO_ROOT/payload/source" -DSNTP_UNCONFIRMED_FILE="\"$BUILD_DIR/test_sntp_unconfirmed\"" \
    -Wno-builtin-macro-redefined -D__DATE__="\"$(LC_ALL=C date -u -d tomorrow '+%b %e %Y')\"" \
    -o "$BUILD_DIR/test_sntp_agreement_local" "$SCRIPT_DIR/test_sntp_agreement.c" "$REPO_ROOT/payload/source/hub_ntp.c" && "$BUILD_DIR/test_sntp_agreement_local"
# Factory reset: one script per device, it deletes the secrets; every other way runs it
python3 "$SCRIPT_DIR/test_factory_reset_wipe.py"
