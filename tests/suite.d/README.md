# Test groups

All firmware tests live in this folder: `tests/run_mock_suite.sh` builds the mock hardware
library and then sources every `*.sh` file here, in name order. A file here runs in the suite's
shell, so it can use `build_test`, `SCRIPT_DIR`, `REPO_ROOT` and `BUILD_DIR`, and a failing
command stops the suite as usual (`set -e`).

Every file name starts with two digits; the suite stops on one that does not. The number also
picks the CI group: `SUITE_GROUP=1` runs the files 00-24, `2` runs 25-49, `3` runs 50-74, `4`
runs 75-99, and CI runs the four groups on separate machines at the same time. A file must not
need anything another file builds or writes: each group starts on a clean machine. Pick the
number of a new file so the four groups stay about the same length.

Files named `*host-tools*.sh` test the tools that run on the user's computer: `tests/run_host_tools.sh`
sources them instead, and `tests/run_mock_suite.sh` skips them. There they can use `SCRIPT_DIR`,
`REPO_ROOT` and `BUILD_DIR` (not `build_test`).