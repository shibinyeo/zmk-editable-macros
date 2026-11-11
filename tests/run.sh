#!/bin/sh
# Run the module's tests with ZMK's own snapshot test harness.
#
# Run this inside the ZMK build container, from anywhere within a west workspace that
# has ZMK checked out:
#
#     /path/to/zmk-editable-macros/tests/run.sh               # every test
#     /path/to/zmk-editable-macros/tests/run.sh play-basic    # named tests
#
# Set ZMK_TESTS_AUTO_ACCEPT=1 to rewrite snapshots from the current output (then review
# the diff), and ZMK_TESTS_VERBOSE=1 to see the build output.

set -u

MODULE_DIR=$(cd "$(dirname "$0")/.." && pwd)
WS=$(west topdir) || {
    echo "Run this from inside a west workspace." >&2
    exit 2
}
ZMK_APP="$WS/zmk/app"

export ZMK_SRC_DIR="$ZMK_APP"
export ZMK_BUILD_DIR="${EM_TEST_BUILD_DIR:-$WS/build-emacro-tests}"
export ZMK_EXTRA_MODULES="$MODULE_DIR"

if [ $# -gt 0 ]; then
    cases="$*"
else
    cases=$(cd "$MODULE_DIR/tests" && for d in */; do [ -f "$d/native_posix_64.keymap" ] && echo "${d%/}"; done)
fi

fail=0
for c in $cases; do
    # One empty working directory per test. The native flash simulator keeps the flash
    # contents in ./flash.bin, and a file left behind by one test must not reach another.
    cwd="$ZMK_BUILD_DIR/run/$c"
    rm -rf "$cwd"
    mkdir -p "$cwd"
    (cd "$cwd" && "$ZMK_APP/run-test.sh" "$MODULE_DIR/tests/$c") || fail=1
done

exit $fail
