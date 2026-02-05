#!/usr/bin/env bash
set -euo pipefail
BASE=$(dirname "$0")
ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo $(pwd))
MIGCOM="${ROOT}/build_noasan_debug/src/mach_services/lib/migcom/migcom"
GEN=/tmp/generated_user.regress.c
INPUT=${BASE}/device_request-small.mig

echo "Regeneration test: $MIGCOM -V -user $GEN < $INPUT"
$MIGCOM -V -user "$GEN" -server /dev/null -sheader /dev/null -dheader /dev/null < "$INPUT"

# run ascii check helper
"${ROOT}/build_noasan_debug/src/mach_services/lib/migcom/test_ascii_generated_user" "$GEN"

echo "ASCII regression test passed."