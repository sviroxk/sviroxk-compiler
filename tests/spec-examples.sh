#!/usr/bin/env bash
# Извлекает примеры из docs/spec.md и проверяет их:
#   ```sc [```stdin] ```text  — программа компилируется, запускается, вывод совпадает;
#   ```sc-error ```text       — программа не компилируется, сообщение содержит текст.
set -Eeuo pipefail
cd "$(dirname "$0")/.."
SPEC=${1:-docs/spec.md}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

awk -v dir="$work" '
    /^```/ {
        if (inside) { inside = 0; next }
        kind = substr($0, 4)
        if (kind == "sc" || kind == "sc-error") { n++; file = dir "/" n "." kind; inside = 1; next }
        if ((kind == "stdin" || kind == "text") && n > 0) { file = dir "/" n "." kind; inside = 1; next }
        file = ""; inside = 1; next
    }
    inside && file != "" { print > file }
' "$SPEC"

count=0
for source in "$work"/*.sc "$work"/*.sc-error; do
    [[ -e "$source" ]] || continue
    base=${source%.*}
    number=$(basename "$base")
    expected="$base.text"
    if [[ ! -f "$expected" ]]; then
        echo "FAIL: пример $number в $SPEC без блока text" >&2
        exit 1
    fi
    case_dir="$work/run-$number"
    mkdir -p "$case_dir"
    cp "$source" "$case_dir/input.svx"
    if [[ "$source" == *.sc-error ]]; then
        if (ulimit -c 0; timeout --signal=TERM 10s ./sc "$case_dir/input.svx" >/dev/null 2>"$case_dir/err"); then
            echo "FAIL: пример $number из $SPEC должен давать ошибку компиляции" >&2
            exit 1
        fi
        if ! grep -Fq -- "$(cat "$expected")" "$case_dir/err"; then
            echo "FAIL: пример $number из $SPEC: нет ожидаемого текста ошибки" >&2
            cat "$expected" "$case_dir/err" >&2
            exit 1
        fi
    else
        if ! (ulimit -c 0; timeout --signal=TERM 10s ./sc "$case_dir/input.svx" >"$case_dir/module.ll" 2>"$case_dir/err"); then
            echo "FAIL: пример $number из $SPEC не компилируется" >&2
            cat "$case_dir/err" >&2
            exit 1
        fi
        stdin_file="$base.stdin"
        [[ -f "$stdin_file" ]] || stdin_file=/dev/null
        if ! (cd "$case_dir" && ulimit -c 0 && timeout --signal=TERM 10s lli module.ll <"$stdin_file" >out 2>run.err); then
            echo "FAIL: пример $number из $SPEC упал при выполнении" >&2
            cat "$case_dir/run.err" >&2
            exit 1
        fi
        if ! diff -u "$expected" "$case_dir/out" >&2; then
            echo "FAIL: пример $number из $SPEC: вывод не совпадает" >&2
            exit 1
        fi
    fi
    count=$((count + 1))
done
echo "PASS: $count примеров из $SPEC"
