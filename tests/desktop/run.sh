#!/bin/bash
# web-savvycan: build and run the desktop C++ tests (CMakeLists.txt here).
#
# Usage: tests/desktop/run.sh [plain|address|thread]...   (default: plain)
#   plain    normal build, load test 10 s per run (WSC_LOAD_SECONDS)
#   address  ASan + UBSan (and LSan), load test 5 s per run
#   thread   TSan with tsan.supp, load test 5 s per run, without the table view (its painting uses Qt's own pooled
#            threads, which TSan cannot follow in an uninstrumented Qt)
# Build trees: build/desktop-tests[-address|-thread]/. Heavy: on the rig run it through the wrapper,
#   ~/.local/share/web-savvycan/heavy.sh tests/desktop/run.sh plain address thread
set -u
cd "$(dirname "$0")"
HERE="$PWD"
ROOT="$(cd ../.. && pwd)"
ENV_FILE="$HOME/.local/share/web-savvycan/toolchain.env"
[ -z "${QT_DESKTOP:-}" ] && [ -f "$ENV_FILE" ] && . "$ENV_FILE"
[ -n "${QT_DESKTOP:-}" ] || { echo "QT_DESKTOP not set (scripts/setup-toolchain.sh)" >&2; exit 2; }
KINDS=("$@"); [ ${#KINDS[@]} -eq 0 ] && KINDS=(plain)
rc=0
for k in "${KINDS[@]}"; do
  case "$k" in
    plain) dir=$ROOT/build/desktop-tests; san=""; secs=${WSC_LOAD_SECONDS:-10} ;;
    address|thread) dir=$ROOT/build/desktop-tests-$k; san=$k; secs=${WSC_LOAD_SECONDS:-5} ;;
    *) echo "unknown kind: $k" >&2; exit 2 ;;
  esac
  [ -f "$dir/build.ninja" ] || cmake -S "$HERE" -B "$dir" -G Ninja -DCMAKE_PREFIX_PATH="$QT_DESKTOP" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWSC_SANITIZER="$san" >/dev/null || exit 1
  ninja -C "$dir" -j"${WSC_JOBS:-2}" >/dev/null || { echo "build failed: $dir"; exit 1; }
  (
    export WSC_LOAD_SECONDS=$secs ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1
    export TSAN_OPTIONS="second_deadlock_stack=1 suppressions=$HERE/tsan.supp"
    [ "$k" = thread ] && export WSC_LOAD_NOVIEW=1
    echo "=== $k"
    ctest --test-dir "$dir" -V | grep -E "QINFO|FAIL|Loc:|Actual|Expected|Totals|WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error|Passed|Failed|tests passed"
    exit "${PIPESTATUS[0]}"
  ) || rc=1
done
exit $rc
