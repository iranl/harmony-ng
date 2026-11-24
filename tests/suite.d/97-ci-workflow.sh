# CI workflow helpers (sourced by tests/run_mock_suite.sh; bash, set -e)

# .github/scripts/code_changed.sh: the builds and firmware tests are skipped only for a change
# made of Markdown files that no test names
sh "$SCRIPT_DIR/test_ci_code_changed.sh"
