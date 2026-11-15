# Android app login: the app keeps a personal access token instead of the Hub password, and an
# operator account may create and delete its own tokens on the Hub.
echo "[-] Running app token login tests..."
python3 "$SCRIPT_DIR/test_app_token.py"
# hub_user.sh removes the tokens of an account it deletes, and leftovers of a name it adds
python3 "$SCRIPT_DIR/test_hub_user_tokens.py"
if command -v node >/dev/null 2>&1; then
    node "$SCRIPT_DIR/test_app_token_login.mjs"
else
    echo "    (node not found, test_app_token_login.mjs skipped)"
fi
