#!/bin/bash
# Сборка и запуск: ./run.sh examples/hello.svx
set -euo pipefail
cd "$(dirname "$0")"
./build.sh
OUT="${1:-examples/hello.svx}"
BASE=$(basename "$OUT")
BASE=${BASE%.*}
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT
timeout 10 ./sc "$OUT" > "$WORK_DIR/program.ll"
llvm-as "$WORK_DIR/program.ll" -o "$WORK_DIR/program.bc"
llc "$WORK_DIR/program.bc" -o "$WORK_DIR/program.s"
clang -no-pie "$WORK_DIR/program.s" -o "$WORK_DIR/$BASE" -lm
"$WORK_DIR/$BASE"
