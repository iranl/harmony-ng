# Android app login: the app keeps a personal access token instead of the Hub password, and an
# operator account may create and delete its own tokens on the Hub.
echo "[-] Running app token login tests..."
python3 "$SCRIPT_DIR/test_app_token.py"
