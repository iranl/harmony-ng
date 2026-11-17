# Web UI translations: no English text written directly into the UI code (both UIs), the new UI's
# setup pages read in German, rows drawn from sample Hub data read in a second language (both UIs),
# the legacy UI redraws the open view on a language change, and the i18n checker (keys,
# placeholders of each language and of each call, machine-translation lists) for both UIs. Node
# parts are skipped when node is not installed.
echo "[-] Running web UI translation tests..."
python3 "$SCRIPT_DIR/test_webui_hardcoded_text.py"
# the hard-coded text check and the escape scan find each form they are meant to find
python3 "$SCRIPT_DIR/test_webui_hardcoded_text_cases.py"
if command -v node >/dev/null 2>&1; then
    node "$SCRIPT_DIR/test_frontend_views_translated.mjs"
    node "$SCRIPT_DIR/test_i18n_checker.mjs"
    node "$SCRIPT_DIR/test_webui_lang_switch.mjs"
    node "$SCRIPT_DIR/test_webui_rows_translated.mjs"
    node "$SCRIPT_DIR/test_webui_i18n_fill.mjs"
    node "$REPO_ROOT/frontend/scripts/i18n-check.js" "$REPO_ROOT/frontend" >/dev/null
    node "$REPO_ROOT/frontend/scripts/i18n-check.js" "$REPO_ROOT/android-app" >/dev/null
    echo "    i18n checker: both UIs ok"
else
    echo "    (node not found, translation checks skipped)"
fi
