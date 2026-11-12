#!/usr/bin/env bash
# Called after test.sh verifies ASan availability; no new toolchain dependency.
set -Eeuo pipefail
SC_BIN=$1
ROOT=$2
mkdir -p "$ROOT"

# Each case runs in its own directory; fixtures cannot touch repository files.
# Modes: clean (exact stdout), runtime (SC diagnostic), detector (unsafe self-test).
check_case() {
    local name=$1 mode=$2 expected=$3 leaks=${4:-0}
    local work="$ROOT/$name" status=0
    mkdir -p "$work"
    cat > "$work/source.svx"
    if ! timeout --signal=TERM 10s "$SC_BIN" "$work/source.svx" >"$work/module.ll" 2>"$work/compile.err"; then
        echo "FAIL: memory safety '$name' did not compile" >&2
        cat "$work/compile.err" >&2
        exit 1
    fi
    sed -E 's/^(define [^{]*)\{$/\1 sanitize_address {/' "$work/module.ll" > "$work/asan.ll"
    if ! clang -no-pie -fsanitize=address -O0 "$work/asan.ll" -o "$work/program" -lm 2>"$work/build.err"; then
        echo "FAIL: memory safety '$name' ASan build failed" >&2
        cat "$work/build.err" >&2
        exit 1
    fi
    (cd "$work" && ASAN_OPTIONS="detect_leaks=$leaks:halt_on_error=1:abort_on_error=0:exitcode=86" \
        timeout --signal=TERM 10s ./program </dev/null >stdout.txt 2>stderr.txt) || status=$?
    case "$mode" in
        clean)
            printf '%s\n' "$expected" > "$work/expected.txt"
            if (( status != 0 )) || [[ -s "$work/stderr.txt" ]] ||
               ! cmp -s "$work/expected.txt" "$work/stdout.txt"; then
                echo "FAIL: memory safety '$name': expected exact output, no sanitizer report, status 0 (got $status)" >&2
                cat "$work/stdout.txt" "$work/stderr.txt" >&2
                exit 1
            fi ;;
        runtime)
            # SC diagnostic goes to stderr (D-007); any sanitizer report there is a failure.
            if (( status != 1 )) || grep -q "Sanitizer" "$work/stderr.txt" ||
               ! grep -Fq "$expected" "$work/stderr.txt" ||
               grep -Fq "SC runtime error" "$work/stdout.txt"; then
                echo "FAIL: memory safety '$name': expected SC runtime diagnostic, not an ASan fault (status $status)" >&2
                cat "$work/stdout.txt" "$work/stderr.txt" >&2
                exit 1
            fi ;;
        detector)
            # These programs are intentionally unsafe: test ASan, not a language guarantee.
            if (( status != 86 )) || ! grep -Fq "ERROR: AddressSanitizer: $expected" "$work/stderr.txt"; then
                echo "FAIL: ASan detector '$name' missed '$expected' (status $status)" >&2
                cat "$work/stderr.txt" >&2
                exit 1
            fi ;;
        *) echo "FAIL: unknown memory safety mode: $mode" >&2; exit 1 ;;
    esac
    echo "PASS: memory safety $mode '$name'"
}

check_case heap-release clean 'after' 1 <<'SC'
ptr text = replace("before", "before", "after");
print(text);
del text;
SC
check_case dynamic-grow-release clean $'5\n10\n50\n4' 1 <<'SC'
ptr values = new int[0];
values.push(10); values.push(20); values.push(30);
values.push(40); values.push(50);
print(values.len()); print(values[0]); print(values[4]);
values.pop(); print(values.len());
del values;
SC
check_case empty-release clean '0' 1 <<'SC'
ptr values = new int[0];
print(values.len());
del values;
SC
# read() returns owning heap strings that SC cannot yet release: leaks disabled here only.
check_case file-roundtrip-eof clean $'first\nsecond\n0\nfirst' <<'SC'
file output = open("roundtrip.txt", "w");
write(output, "first\nsecond"); close(output);
file input = open("roundtrip.txt", "r");
str first = read(input); str second = read(input); str eof = read(input);
print(first); print(second); print(len(eof)); print(first);
close(input);
SC
check_case dynamic-negative-read runtime 'SC runtime error: array index out of bounds' <<'SC'
ptr values = new int[2];
int index = -1;
print(values[index]);
SC
check_case dynamic-upper-write runtime 'SC runtime error: array index out of bounds' <<'SC'
ptr values = new int[2];
i64 index = values.len();
values[index] = 9;
SC
check_case missing-file runtime "SC runtime error: cannot open file 'missing/file.txt'" <<'SC'
file input = open("missing/file.txt", "r");
str line = read(input);
print(line);
SC
# Self-tests of the ASan detector. Direct use after del / double del is a compile
# error (test.sh: use-after-del, double-del); a second name for the same memory is
# not tracked, so these still reach run time.
check_case use-after-delete detector heap-use-after-free <<'SC'
ptr text = replace("abc", "b", "q");
ptr alias = text;
del text;
print(alias[0]);
SC
check_case double-delete detector attempting\ double-free <<'SC'
ptr text = replace("abc", "b", "q");
ptr alias = text;
del text;
del alias;
SC
check_case raw-pointer-overrun detector heap-buffer-overflow <<'SC'
ptr text = replace("abc", "z", "q");
print(text[1]);
del text;
SC

echo 'PASS: memory safety suite (4 clean, 3 controlled runtime errors, 3 ASan detector self-tests)'
