#!/bin/bash
# web-savvycan: build and run the frame tests (store, parsers, full-rate replay load test).
#
# Usage: test/frames/run.sh [plain|address|thread]...   (default: plain address thread)
#   plain    normal build, load test 10 s per run (WSC_LOAD_SECONDS)
#   address  ASan + UBSan (and LSan), load test 5 s per run
#   thread   TSan with test/frames/tsan.supp, load test 5 s per run, without the table view (its
#            painting uses Qt's own pooled threads, which TSan cannot follow in an uninstrumented Qt)
# Build trees: build-tests[-address|-thread]/ at the repository root. On the rig run it through
# the heavy-job wrapper:  ~/.local/share/web-savvycan/heavy.sh test/frames/run.sh
set -u
cd "$(dirname "$0")/../.."
ROOT="$PWD"
ENV_FILE="$HOME/.local/share/web-savvycan/toolchain.env"
[ -z "${QT_DESKTOP:-}" ] && [ -f "$ENV_FILE" ] && . "$ENV_FILE"
[ -n "${QT_DESKTOP:-}" ] || { echo "QT_DESKTOP not set (scripts/setup-toolchain.sh)" >&2; exit 2; }
KINDS=("$@"); [ ${#KINDS[@]} -eq 0 ] && KINDS=(plain address thread)
rc=0
for k in "${KINDS[@]}"; do
  case "$k" in
    plain) dir=build-tests; san=""; secs=${WSC_LOAD_SECONDS:-10} ;;
    address|thread) dir=build-tests-$k; san=$k; secs=${WSC_LOAD_SECONDS:-5} ;;
    *) echo "unknown kind: $k" >&2; exit 2 ;;
  esac
  [ -f "$dir/build.ninja" ] || cmake -S test/frames -B "$dir" -G Ninja -DCMAKE_PREFIX_PATH="$QT_DESKTOP" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFRAMES_SANITIZER="$san" >/dev/null || exit 1
  ninja -C "$dir" -j"${WSC_JOBS:-2}" >/dev/null || { echo "build failed: $dir"; exit 1; }
  (
    cd "$dir"
    export QT_QPA_PLATFORM=offscreen WSC_LOAD_SECONDS=$secs
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1
    export TSAN_OPTIONS="second_deadlock_stack=1 suppressions=$ROOT/test/frames/tsan.supp"
    [ "$k" = thread ] && export WSC_LOAD_NOVIEW=1
    r=0
    for t in tst_framestore tst_parsers tst_ingest_load; do
      out=$(./$t 2>&1); e=$?
      echo "$out" | grep -E "QINFO|FAIL|Loc:|Actual|Expected|Totals|WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error"
      echo "=== $t ($k): exit $e"
      [ $e -ne 0 ] && r=1
    done
    exit $r
  ) || rc=1
done
exit $rc
