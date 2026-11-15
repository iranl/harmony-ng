# Test groups

`tests/run_mock_suite.sh` sources every `*.sh` file in this folder, in name order, after its own
list. A file here runs in the suite's shell, so it can use `build_test`, `SCRIPT_DIR`, `REPO_ROOT`
and `BUILD_DIR`, and a failing command stops the suite as usual (`set -e`).
