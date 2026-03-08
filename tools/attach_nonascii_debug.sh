#!/usr/bin/env bash
set -euo pipefail
OUTDIR=/workspace/docker_output
mkdir -p "$OUTDIR"
# Build debug interceptor inside container to ensure ABI match
gcc -shared -fPIC -o /workspace/tools/libnonascii_debug.so /workspace/tools/find_nonascii_write.c -ldl
# Build NOASAN if not present
if [ ! -x "$OUTDIR/noasan_migcom" ]; then
  BUILD_DIR="/tmp/build_for_debug_$$"
  rm -rf "$BUILD_DIR"
  mkdir -p "$BUILD_DIR"
  # Detect SRC similarly to build_and_run.sh (project may live under subdirs)
  if [ -f "/workspace/CMakeLists.txt" ]; then
    SRC=/workspace
  elif [ -f "/workspace/osfmk/CMakeLists.txt" ]; then
    SRC=/workspace/osfmk
  elif [ -f "/workspace/src/mach_services/lib/migcom/CMakeLists.txt" ]; then
    SRC=/workspace/src/mach_services/lib/migcom
  elif [ -f "/workspace/osfmk/src/mach_services/lib/migcom/CMakeLists.txt" ]; then
    SRC=/workspace/osfmk/src/mach_services/lib/migcom
  else
    SRC=/workspace
    echo "[attach] Warning: could not find CMakeLists.txt in expected locations; using /workspace" >> "$OUTDIR/gdb_pause_run.log"
  fi
  cmake -S "$SRC" -B "$BUILD_DIR" -DCMAKE_C_FLAGS="-g -O0 -fno-inline -fno-omit-frame-pointer" 2>&1 | tee "$OUTDIR/cmake_debug.log"
  cmake --build "$BUILD_DIR" --target migcom -j "$(nproc)" 2>&1 | tee "$OUTDIR/build_debug.log"
  cp "$BUILD_DIR/src/mach_services/lib/migcom/migcom" "$OUTDIR/noasan_migcom" || true
  chmod +x "$OUTDIR/noasan_migcom" || true
fi
TEST_MIG="/workspace/osfmk/src/mach_services/lib/migcom/tests/device_request-small.mig"
# Run NOASAN under debug interceptor; when pause file appears attach gdb and dump diagnostics
MIG_ABORT_ON_NONASCII=2 LD_PRELOAD=/workspace/tools/libnonascii_debug.so "$OUTDIR/noasan_migcom" -V -i test_ -user /dev/null -server /dev/null -sheader /dev/null -dheader /dev/null < "$TEST_MIG" & child=$!
echo "[attach] child=$child" > "$OUTDIR/gdb_pause_run.log"
# wait for the pause file created by the interceptor (match any pid)
for i in $(seq 1 400); do
  f=$(ls /tmp/nonascii_pause_* 2>/dev/null || true)
  if [ -n "$f" ]; then
    pid=$(cat "$f" 2>/dev/null || true)
    echo "[attach] pause file found at $f pid=$pid" >> "$OUTDIR/gdb_pause_run.log"
    sleep 0.2
    gdb -p $pid -batch -ex "set logging file $OUTDIR/gdb_pause_bt.log" -ex "set logging on" -ex "bt full" -ex "info registers" -ex "x/512bx \$rsi" -ex "x/512bx \$rdi" -ex "x/256bx \$rsp" -ex "x/256bx \$rbp" -ex "set logging off" -ex "detach" -ex "quit" || true
    echo "[attach] gdb attach/dump complete" >> "$OUTDIR/gdb_pause_run.log"
    kill -CONT $pid || true
    wait $pid || true
    echo "[attach] child exited" >> "$OUTDIR/gdb_pause_run.log"
    exit 0
  fi
  sleep 0.05
done

echo "[attach] timeout waiting for pause file" >> "$OUTDIR/gdb_pause_run.log"
wait $child || true
exit 1
