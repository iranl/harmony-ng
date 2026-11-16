# Test groups

`tests/run_mock_suite.sh` sources every `*.sh` file in this folder, in name order, after its own
list. A file here runs in the suite's shell, so it can use `build_test`, `SCRIPT_DIR`, `REPO_ROOT`
and `BUILD_DIR`, and a failing command stops the suite as usual (`set -e`).

Files named `*host-tools*.sh` test the tools that run on the user's computer: `tests/run_host_tools.sh`
sources them instead, and `tests/run_mock_suite.sh` skips them. There they can use `SCRIPT_DIR`,
`REPO_ROOT` and `BUILD_DIR` (not `build_test`).
