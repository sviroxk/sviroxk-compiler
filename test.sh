#!/usr/bin/env bash
# Safe, isolated compiler tests: ./test.sh
set -Eeuo pipefail
cd "$(dirname "$0")"
ulimit -c 0

for tool in timeout llvm-as llc clang; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Ошибка: не найдена необходимая команда: $tool" >&2
        exit 1
    fi
done

if [[ "${SC_SKIP_BUILD:-0}" != "1" ]]; then
    ./build.sh
fi
./test-conformance.sh
SC_BIN="${SC_BIN:-$PWD/sc}"

version_output=$("$SC_BIN" --version)
if [[ "$version_output" != "SC compiler 0.3.0" ]]; then
    echo "FAIL: --version returned unexpected text: $version_output" >&2
    exit 1
fi
help_output=$("$SC_BIN" --help)
if ! grep -Fq 'Usage: sc <source.svx>' <<< "$help_output"; then
    echo "FAIL: --help is missing source usage" >&2
    exit 1
fi
if "$SC_BIN" --unknown >/dev/null 2>"${TMPDIR:-/tmp}/sc-unknown-option.log"; then
    echo "FAIL: unknown CLI option returned success" >&2
    exit 1
fi
echo "PASS: CLI version/help/unknown-option contract"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

sources=(
    "hello.svx"
    "test.svx"
    "examples/struct.svx"
    "examples/struct_init.svx"
    "examples/array_write.svx"
    "examples/dor_func.svx"
    "examples/full.svx"
    "examples/modern.svx"
    "examples/numeric_literals.svx"
    "examples/typed.svx"
    "examples/void.svx"
    "examples/returns.svx"
    "examples/fixed_width.svx"
    "examples/fixed_width_args.svx"
    "examples/signedness.svx"
    "examples/remainder.svx"
    "examples/comparisons.svx"
    "examples/scientific.svx"
    "examples/stdlib_math.svx"
    "examples/stdlib_abs_min.svx"
    "examples/casts.svx"
    "examples/operators.svx"
    "examples/string_replace.svx"
    "docs/conformance/basic.svx"
    "docs/conformance/operators.svx"
    "examples/short_circuit.svx"
    "examples/v1_bitwise.svx"
    "examples/v1_compound_assignment.svx"
    "examples/v1_const.svx"
    "examples/v1_else_if.svx"
    "examples/v1_for_array.svx"
    "examples/v1_for_range.svx"
    "examples/v1_increment.svx"
    "examples/v1_integer_division.svx"
    "examples/v1_let_inference.svx"
    "examples/v1_loop.svx"
    "examples/v1_power.svx"
    "examples/v1_ternary.svx"
    "examples/v1_while.svx"
    "examples/p0_unknown_backend_type.svx"
    "examples/p0_struct_field_store.svx"
    "examples/b1_forward_call.svx"
    "examples/b1_factorial.svx"
    "examples/b1_mutual_recursion.svx"
    "examples/p0_array_non_i32.svx"
    "examples/b2_numeric_input.svx"
    "examples/b2_string_input.svx"
    "examples/b2_pointer.svx"
    "examples/b2_dynamic_array.svx"
    "examples/b2_file_read.svx"
    "examples/p0_array_types.svx"
    "examples/a11_file_write.svx"
    "examples/a10_lexer_literals.svx"
    "examples/a11_string_input.svx"
    "examples/a12_widening.svx"
    "examples/a13_literal_arithmetic.svx"
    "examples/a14_shadowing.svx"
    "examples/a16_builtin_types.svx"
    "examples/a18_struct_field_collision.svx"
    "examples/a2_cast_precedence.svx"
    "examples/a3_void_return.svx"
    "examples/a4_else_if.svx"
    "examples/a6_str_type.svx"
    "examples/a7_char.svx"
    "examples/a8_builtin_names.svx"
    "examples/a9_file_open.svx"
    "examples/contextual_numeric_literals.svx"
    "examples/unsigned_print.svx"
    "examples/defer.svx"
    "examples/switch.svx"
    "examples/switch_break.svx"
    "examples/overload.svx"
    "examples/string_ops.svx"
    "examples/struct_eq.svx"
)

for source in "${sources[@]}"; do
    name=${source%.svx}
    work="$TMP_DIR/${name//\//_}"
    mkdir -p "$work"
    echo "=== $source ==="

    if [[ "$source" == "examples/b2_file_read.svx" ]]; then
        printf '%s
' 'file data' > "$work/b2-input.tmp"
    fi
    if (ulimit -c 0; timeout --signal=TERM 15s "$SC_BIN" "$source" >"$work/module.ll" 2>"$work/compiler.err"); then
        :
    else
        status=$?
        if (( status == 2 )); then
            echo "FAIL: semantic checker rejected valid source $source" >&2
        else
            echo "FAIL: compiler failed or exceeded 15s on $source (status $status)" >&2
        fi
        cat "$work/compiler.err" >&2
        exit 1
    fi
    if [[ "$source" == "examples/fixed_width.svx" ]]; then
        for ir_type in 'global i8' 'global i16' 'global i32' 'global i64' 'global float' 'global double'; do
            if ! grep -Fq "$ir_type" "$work/module.ll"; then
                echo "FAIL: expected LLVM storage width missing: $ir_type" >&2
                exit 1
            fi
        done
    fi
    if ! llvm-as "$work/module.ll" -o "$work/module.bc" 2>"$work/llvm.err"; then
        echo "FAIL: invalid LLVM IR from $source" >&2
        cat "$work/llvm.err" >&2
        exit 1
    fi
    if ! llc "$work/module.bc" -o "$work/program.s" 2>"$work/llc.err"; then
        echo "FAIL: LLVM code generation failed for $source" >&2
        cat "$work/llc.err" >&2
        exit 1
    fi
    if ! clang -no-pie "$work/program.s" stdlib/runtime.c -o "$work/program" -lm 2>"$work/link.err"; then
        echo "FAIL: native linking failed for $source" >&2
        cat "$work/link.err" >&2
        exit 1
    fi
    if [[ "$source" == "examples/b2_numeric_input.svx" ]]; then
        printf '%s
' '37' | timeout --signal=TERM 10s "$work/program" >"$work/output.txt" 2>"$work/runtime.err"
    elif [[ "$source" == "examples/b2_string_input.svx" ]]; then
        printf '%s
' 'hello' | timeout --signal=TERM 10s "$work/program" >"$work/output.txt" 2>"$work/runtime.err"
    elif [[ "$source" == "examples/a11_string_input.svx" ]]; then
        printf '%s
' 'hello' | timeout --signal=TERM 10s "$work/program" >"$work/output.txt" 2>"$work/runtime.err"
    elif [[ "$source" == "examples/b2_file_read.svx" ]]; then
        (cd "$work" && timeout --signal=TERM 10s ./program) >"$work/output.txt" 2>"$work/runtime.err"
    elif [[ "$source" == "examples/a11_file_write.svx" ]]; then
        (cd "$work" && timeout --signal=TERM 10s "$work/program") >"$work/output.txt" 2>"$work/runtime.err"
    else
        timeout --signal=TERM 10s "$work/program" >"$work/output.txt" 2>"$work/runtime.err"
    fi
    if [[ $? -ne 0 ]]; then
        echo "FAIL: program failed or exceeded 10s for $source" >&2
        cat "$work/runtime.err" >&2
        exit 1
    fi
    if [[ "$source" == "examples/fixed_width.svx" ]]; then
        expected=$'579\n455\n-220\n51000\n-220\n1\n0\n4.000000\n7.750000\n300000'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: fixed-width arithmetic/output mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/fixed_width_args.svx" ]]; then
        expected=$'255\n-120\n256'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: fixed-width function argument/return mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/signedness.svx" ]]; then
        expected=$'201\n-19'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: narrow signedness promotion mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/remainder.svx" ]]; then
        expected=$'-2\n2'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: signed/unsigned remainder mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/comparisons.svx" ]]; then
        expected=$'1\n1'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: mixed-width comparisons mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/numeric_literals.svx" ]]; then
        expected=$'3.000000\n1000.000000\n3'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: decimal/exponent numeric literal semantics mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/scientific.svx" ]]; then
        expected=$'1000.000000\n0.012500\n125.000000'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: exponent float literal mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/array_write.svx" ]]; then
        expected=$'9'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: array element assignment/read mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/stdlib_math.svx" ]]; then
        expected=$'17\n12\n20\n1'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: math library function mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/stdlib_abs_min.svx" ]]; then
        expected=$'2147483648'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: math_abs failed for the minimum i32 value" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/casts.svx" ]]; then
        expected=$'127\n255\n1.250000\n1.250000\n127'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: explicit numeric cast mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/string_ops.svx" ]]; then
        expected=$'5\n104\n111\nhelloworld\nfoobar'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: string operations mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/operators.svx" ]]; then
        expected=$'7\nlogical operators passed'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: loop/index/comparison/logical operator mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/short_circuit.svx" ]]; then
        expected=$'short circuit passed'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: logical operators evaluated skipped operands" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_bitwise.svx" ]]; then
        expected=$'15\n30\n-8\n15'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: bitwise operator result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_compound_assignment.svx" ]]; then
        expected=$'5'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: compound assignment result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_const.svx" ]]; then
        expected=$'42'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: const declaration result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_else_if.svx" ]]; then
        expected=$'3'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: else-if branch selection mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_for_array.svx" ]]; then
        expected=$'12'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: static-array iteration mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_for_range.svx" ]]; then
        expected=$'13'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: range-for iteration mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_increment.svx" ]]; then
        expected=$'2'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: postfix increment/decrement mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_integer_division.svx" ]]; then
        expected=$'3\n-3'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: integer division result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_let_inference.svx" ]]; then
        expected=$'42'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: inferred let type mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_loop.svx" ]]; then
        expected=$'3'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: loop/break result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_power.svx" ]]; then
        expected=$'8\n8.000000'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: exponentiation result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_ternary.svx" ]]; then
        expected=$'10'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: ternary result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/v1_while.svx" ]]; then
        expected=$'4'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: while loop result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/p0_unknown_backend_type.svx" ]]; then
        expected=$'1'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: struct aggregate field load/store mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/p0_struct_field_store.svx" ]]; then
        expected=$'9223372036854775806'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: struct aggregate i64 field store mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/p0_array_types.svx" ]]; then
        expected=$'1\n1\n1\n1\n1\n1\n1\n1.000000'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: array element type mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/p0_array_non_i32.svx" ]]; then
        expected=$'1.500000\n2.500000\n3.500000'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: f64 array element mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/b1_forward_call.svx" ]]; then
        expected=$'6'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: forward function call mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/b1_factorial.svx" ]]; then
        expected=$'120'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: recursive function result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/b1_mutual_recursion.svx" ]]; then
        expected=$'1\n1'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: mutual recursion result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/b2_numeric_input.svx" ]]; then
        expected=$'number: 37'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: numeric input result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/b2_string_input.svx" ]]; then
        expected=$'text: hello'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: string input result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/b2_pointer.svx" ]]; then
        expected=$'9'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: pointer write/read result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/b2_dynamic_array.svx" ]]; then
        expected=$'15'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: dynamic array result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/b2_file_read.svx" ]]; then
        expected=$'file data'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: file read result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a11_file_write.svx" ]]; then
        expected=$'1'
        if [[ "$(cat "$work/output.txt")" != "$expected" || "$(cat "$work/sc-a11.tmp" 2>/dev/null)" != "ok" ]]; then
            echo "FAIL: file write result mismatch" >&2
            cat "$work/output.txt" "$work/sc-a11.tmp" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a10_lexer_literals.svx" ]]; then
        expected=$(printf '84\n'"'"'\na')
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: lexer literal mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a11_string_input.svx" ]]; then
        expected=$'text: hello'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: string input mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a12_widening.svx" ]]; then
        expected=$'12\n200\n200\n1.500000'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: numeric widening mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a13_literal_arithmetic.svx" ]]; then
        expected=$'5\n7\n11\n3.750000'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: literal arithmetic mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a14_shadowing.svx" ]]; then
        expected=$'20\n10'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: variable shadowing mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a16_builtin_types.svx" ]]; then
        expected=$'3.000000\n8.000000\n9\n7\n3\n2.000000\n2.500000\n3.500000\n2.500000\n3000000000\n2.500000'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: builtin type mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a18_struct_field_collision.svx" ]]; then
        expected=$'9\n42'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: struct field collision mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a2_cast_precedence.svx" ]]; then
        expected=$'128'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: cast precedence mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a3_void_return.svx" ]]; then
        expected=$'1'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: void return mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a4_else_if.svx" ]]; then
        expected=$'2'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: else-if mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a6_str_type.svx" ]]; then
        expected=$'ok'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: string type mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a7_char.svx" ]]; then
        expected=$'a'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: char mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a8_builtin_names.svx" ]]; then
        expected=$'7'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: builtin name shadowing mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/a9_file_open.svx" ]]; then
        expected=$'1'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: file open/close mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/contextual_numeric_literals.svx" ]]; then
        expected=$'5\n7\n6.250000\n3000000000\n4000000000'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: contextual numeric literal mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/unsigned_print.svx" ]]; then
        expected=$'4000000000\n18446744073709551615'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: unsigned print mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/defer.svx" ]]; then
        expected=$'42'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: defer example result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/switch.svx" ]]; then
        expected=$'100\n90\n80\n0'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: switch example result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/switch_break.svx" ]]; then
        expected=$'one\ntwo\nother'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: switch_break example result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "docs/conformance/basic.svx" ]]; then
        expected=$'42'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: documented basic conformance example mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "docs/conformance/operators.svx" ]]; then
        expected=$'7'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: documented operator conformance example mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/string_replace.svx" ]]; then
        long_base=""
        for ((repeat = 0; repeat < 20; repeat++)); do
            long_base+="abcdefghijklmnopqrstuvwxyz"
        done
        expected=$'before FOUND after\nplain text\n'"${long_base/nop/replacement}"$'\n'"${long_base/mnop/WXYZ}"
        printf '%s\n' "$expected" > "$work/expected.txt"
        if ! cmp -s "$work/output.txt" "$work/expected.txt"; then
            echo "FAIL: string replacement result mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/overload.svx" ]]; then
        expected=$'3\n6\n4.000000'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: function overload resolution mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "examples/struct_eq.svx" ]]; then
        expected=$'1\n0\n0'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: struct equality mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    fi
    cat "$work/output.txt"
done

expect_runtime_error() {
    local name=$1
    local source=$2
    local expected=$3
    local work="$TMP_DIR/runtime-error-$name"
    mkdir -p "$work"
    if ! timeout --signal=TERM 15s "$SC_BIN" "$source" >"$work/module.ll" 2>"$work/compiler.err"; then
        echo "FAIL: runtime-error source '$source' did not compile" >&2
        cat "$work/compiler.err" >&2
        exit 1
    fi
    llvm-as "$work/module.ll" -o "$work/module.bc"
    llc "$work/module.bc" -o "$work/program.s"
    clang -no-pie "$work/program.s" stdlib/runtime.c -o "$work/program" -lm
    local status=0
    if (ulimit -c 0; timeout --signal=TERM 10s "$work/program" >"$work/output.log" 2>"$work/error.log"); then
        echo "FAIL: runtime error '$name' did not fail" >&2
        exit 1
    else
        status=$?
    fi
    if (( status >= 128 )); then
        echo "FAIL: runtime error '$name' crashed (status $status)" >&2
        cat "$work/error.log" >&2
        exit 1
    fi
    if ! grep -Fq "$expected" "$work/error.log" &&
       ! grep -Fq "$expected" "$work/output.log"; then
        echo "FAIL: runtime error '$name' missed expected diagnostic" >&2
        cat "$work/output.log" "$work/error.log" >&2
        exit 1
    fi
    echo "PASS: runtime error '$name' was diagnosed"
}

expect_runtime_error "array-negative-index" "examples/array_bounds_negative.svx" "array index out of bounds"
expect_runtime_error "array-upper-index" "examples/array_bounds_upper.svx" "array index out of bounds"

expect_compile_error() {
    local name=$1
    local source=$2
    local work="$TMP_DIR/invalid-$name"
    local status=0
    mkdir -p "$work"
    printf '%s\n' "$source" > "$work/input.svx"

    if (ulimit -c 0; timeout --signal=TERM 10s "$SC_BIN" "$work/input.svx" >"$work/output.ll" 2>"$work/error.log"); then
        echo "FAIL: invalid program '$name' compiled successfully" >&2
        exit 1
    else
        status=$?
    fi
    if (( status >= 128 )); then
        echo "FAIL: compiler crashed while handling '$name' (status $status)" >&2
        cat "$work/error.log" >&2
        exit 1
    fi
    if ! grep -Eq 'Ошибка [0-9]+:[0-9]+|^[^:]+:[0-9]+:[0-9]+: error\[' "$work/error.log"; then
        echo "FAIL: '$name' did not produce a source-location diagnostic" >&2
        cat "$work/error.log" >&2
        exit 1
    fi
    if [[ "$name" == "diagnostic-source-caret" ]] &&
       ! grep -Fq '^' "$work/error.log"; then
        echo "FAIL: '$name' did not show a caret diagnostic" >&2
        cat "$work/error.log" >&2
        exit 1
    fi
    if [[ "$name" == "multiple-syntax-errors" ]] &&
       [[ $(grep -c 'error\[syntax\]' "$work/error.log") -ne 2 ]]; then
        echo "FAIL: '$name' did not report both independent syntax errors" >&2
        cat "$work/error.log" >&2
        exit 1
    fi
    echo "PASS: invalid input '$name' reported an error without crashing"
}

expect_compile_error_with_details() {
    local name=$1
    local source=$2
    local expected_code=$3
    local expected_pattern=$4
    local expected_line=$5
    local expected_col=$6
    local work="$TMP_DIR/invalid-$name"
    local status=0
    mkdir -p "$work"
    printf '%s\n' "$source" > "$work/input.svx"

    if (ulimit -c 0; timeout --signal=TERM 10s "$SC_BIN" "$work/input.svx" >"$work/output.ll" 2>"$work/error.log"); then
        echo "FAIL: invalid program '$name' compiled successfully" >&2
        exit 1
    else
        status=$?
    fi
    if (( status >= 128 )); then
        echo "FAIL: compiler crashed while handling '$name' (status $status)" >&2
        cat "$work/error.log" >&2
        exit 1
    fi

    local error_line
    error_line=$(grep -E '^[^:]+:[0-9]+:[0-9]+: error\[' "$work/error.log" | head -1)
    if [[ -z "$error_line" ]]; then
        echo "FAIL: '$name' did not produce a source-location diagnostic" >&2
        cat "$work/error.log" >&2
        exit 1
    fi

    if [[ -n "$expected_code" ]] && ! echo "$error_line" | grep -Fq "[$expected_code]"; then
        echo "FAIL: '$name' expected error code [$expected_code], got: $error_line" >&2
        cat "$work/error.log" >&2
        exit 1
    fi

    if [[ -n "$expected_pattern" ]] && ! echo "$error_line" | grep -Eq "$expected_pattern"; then
        echo "FAIL: '$name' expected pattern '$expected_pattern', got: $error_line" >&2
        cat "$work/error.log" >&2
        exit 1
    fi

    if [[ -n "$expected_line" ]]; then
        local actual_line
        actual_line=$(echo "$error_line" | sed -E 's/^[^:]+:([0-9]+):[0-9]+:.*/\1/')
        if [[ "$actual_line" != "$expected_line" ]]; then
            echo "FAIL: '$name' expected line $expected_line, got line $actual_line" >&2
            cat "$work/error.log" >&2
            exit 1
        fi
    fi

    if [[ -n "$expected_col" ]]; then
        local actual_col
        actual_col=$(echo "$error_line" | sed -E 's/^[^:]+:[0-9]+:([0-9]+):.*/\1/')
        if [[ "$actual_col" != "$expected_col" ]]; then
            echo "FAIL: '$name' expected column $expected_col, got column $actual_col" >&2
            cat "$work/error.log" >&2
            exit 1
        fi
    fi

    echo "PASS: invalid input '$name' reported expected error (code: $expected_code, pattern: $expected_pattern)"
}

expect_compile_error "missing-expression" 'let value: int = ;'
expect_compile_error "diagnostic-source-caret" 'int count = "wrong";'
expect_compile_error "multiple-syntax-errors" 'int first = ; int second = ;'
expect_compile_error "missing-function-parameter" 'fn add(int left,) { return left; }'
expect_compile_error "unexpected-character" 'print(1 ! 2);'
expect_compile_error "unterminated-string" 'print("unfinished);'
expect_compile_error "unknown-variable" 'print(not_declared);'
expect_compile_error "unknown-function" 'let result: int = missing(1);'
expect_compile_error "rep-requires-strings" 'let bad: ptr = rep("text", 4, "x");'
expect_compile_error "math-abs-out-of-range-argument" "imp \"$PWD/stdlib/math/math.svx\"; let result: long = math_abs(2147483648);"
expect_compile_error "wrong-argument-type" 'fn twice(int value) { return value * 2; } let result: int = twice("text");'
expect_compile_error "wrong-return-type" 'fn wrong() { return "text"; }'
expect_compile_error "wrong-typed-return" 'fn int wrong() { return "text"; }'
expect_compile_error "nonvoid-fallthrough" 'fn int maybe(bool flag) { if (flag) { return 1; } }'
expect_compile_error "missing-return-else" 'fn choose(bool flag) { if (flag) { return 1; } else { print("no result"); } }'
expect_compile_error "unknown-function-argument" 'fn add(int value) { return value; } let result: int = add(missing);'
expect_compile_error "invalid-arithmetic" 'let result: int = "text" + 1;'
expect_compile_error "float-literal-not-implicit-int" 'int value = 3.0;'
expect_compile_error "exponent-literal-not-implicit-int" 'int value = 1e3;'
expect_compile_error "wrong-assignment-type" 'let result: int = "text";'
expect_compile_error "i8-overflow" 'i8 value = 128;'
expect_compile_error "i8-cast-overflow" 'i8 value = (i8) 128;'
expect_compile_error "u8-overflow" 'u8 value = 256;'
expect_compile_error "i16-overflow" 'i16 value = 32768;'
expect_compile_error "i8-underflow" 'i8 value = -129;'
expect_compile_error "u8-negative" 'u8 value = -1;'
expect_compile_error "byte-overflow" 'byte value = 256;'
expect_compile_error "int-alias-overflow" 'int value = 2147483648;'
expect_compile_error "double-overflow" 'double value = 1e9999;'
expect_compile_error "u64-overflow" 'u64 value = 18446744073709551616;'
expect_compile_error "u32-overflow" 'u32 value = 4294967296;'
expect_compile_error "int-overflow" 'int value = 2147483648;'
expect_compile_error "expression-integer-overflow" 'print(9223372036854775808);'
expect_compile_error "expression-float-overflow" 'print(1e9999);'
expect_compile_error "numeric-tail-garbage" 'print(123abc);'
expect_compile_error "long-overflow" 'long value = 9223372036854775808;'
expect_compile_error "short-overflow" 'short value = 32768;'
expect_compile_error "f32-overflow" 'f32 value = 1e999;'
expect_compile_error "malformed-exponent" 'double value = 1e;'
expect_compile_error "malformed-multiple-decimals" 'double value = 1.2.3;'
# exact-array-not-yet-supported is now supported (i16, u16, f32, u8, i8, u32, u64, i64)
# Keep a test for an array type that should still be rejected
expect_compile_error "array-unsupported-element" 'ptr[] values = [1, 2];'
expect_compile_error "unsigned-mixed-sign" 'u32 positive = 1; i32 signed = -1; let result: int = positive + signed;'
expect_compile_error "unsigned-width-mix" 'u64 large = 2; i64 signed = -1; let result: i64 = large + signed;'
expect_compile_error "mixed-signedness-comparison" 'u32 unsigned_value = 1; i32 signed_value = 1; if (unsigned_value > signed_value) { print(1); }'
expect_compile_error "array-index-type" 'svi rox[] values = [1, 2]; svi dor index = 0.5; values[index] = 9;'
expect_compile_error "unknown-type" 'mystery value = 1;'
expect_compile_error "duplicate-variable" 'let result: int = 1; let result: int = 2;'
expect_compile_error "break-outside-loop" 'brk;'
expect_compile_error "builtin-function-collision" 'fn int max(int left, int right) { return left + right; }'
expect_compile_error "nested-function" 'if (1) { fn int inner(int value) { return value; } }'
expect_compile_error "unreachable-after-return" 'fn void dead() { return; print(2); }'
expect_compile_error "unreachable-after-break" 'fn void dead() { cert ceh (2) { brk; print(3); } }'
expect_compile_error "unreachable-after-continue" 'fn void dead() { cert ceh (2) { cont; print(4); } }'
expect_compile_error "continue-in-counted-loop" 'let total: int = 0; cert ceh (5) { cont; total = total + 1; } print(total);'
expect_compile_error "string-arithmetic" 'let bad: int = "x" + "y";'
expect_compile_error "int-overflow-negative" 'int overflow = -2147483649;'
expect_compile_error "array-index-float" 'svi rox[] values = [1, 2]; svi dor index = 0.5; slov(values[index]);'
expect_compile_error "builtin-no-args" 'sqrt();'
expect_compile_error "type-mismatch-initializer" 'int count = "wrong";'

# Categorized negative tests using expect_compile_error_with_details
# === LEXICAL ERRORS ===
expect_compile_error_with_details "lex-unclosed-comment" '/* unclosed comment' "E_LEX_UNCLOSED_COMMENT" "незакрытый блочный комментарий" "1" "1"
expect_compile_error_with_details "lex-malformed-hex" 'let x: int = 0xGG;' "E_LEX_MALFORMED_NUMBER" "некорректный числовой литерал" "1" "14"
expect_compile_error_with_details "lex-malformed-binary" 'let x: int = 0b12;' "E_LEX_MALFORMED_NUMBER" "некорректный числовой литерал" "1" "14"
expect_compile_error_with_details "lex-number-out-of-range" 'let x: u64 = 18446744073709551616;' "E_SEMANTIC_INT_RANGE" "вне диапазона" "1" "14"
expect_compile_error_with_details "lex-char-empty" $'let c: char = \'\'\';' "E_LEX_CHAR_EXPECTED" "ожидался один символ" "1" "15"
expect_compile_error_with_details "lex-char-unescaped" "$(printf 'let c: char = '\''\\\n')" "E_LEX_CHAR_UNESCAPED" "незавершённая escape-последовательность" "1" "15"
expect_compile_error_with_details "lex-char-unknown-escape" $'let c: char = \'\\z\';' "E_LEX_CHAR_UNKNOWN_ESCAPE" "неизвестная escape-последовательность" "1" "15"
expect_compile_error_with_details "lex-char-missing-quote" "$(printf 'let c: char = '\''a\n')" "E_LEX_CHAR_MISSING_QUOTE" "символьный литерал должен содержать один символ и закрывающую кавычку" "1" "15"
expect_compile_error_with_details "lex-string-unknown-escape" 'let s: str = "\z";' "E_LEX_STRING_UNKNOWN_ESCAPE" "неизвестная escape-последовательность" "1" "16"
expect_compile_error_with_details "lex-string-unclosed" 'let s: str = "unclosed;' "E_LEX_STRING_UNCLOSED" "строковый литерал не закрыт" "1" "14"
expect_compile_error_with_details "lex-unknown-char" 'let x = @;' "E_LEX_UNKNOWN_CHAR" "неизвестный символ" "1" "9"

# === SYNTACTIC ERRORS ===
expect_compile_error_with_details "syntax-expected-expression" 'let value: int = ;' "E_SYNTAX_EXPECTED" "ожидалось выражение" "1" "18"
expect_compile_error_with_details "syntax-expected-semicolon" 'let value: int = 1' "E_SYNTAX_EXPECTED" "ожидалось" "1" "1"
expect_compile_error_with_details "syntax-unknown-type" 'let value: mystery = 1;' "E_SYNTAX_UNKNOWN_TYPE" "неизвестный тип" "1" "12"
expect_compile_error_with_details "syntax-unexpected-token" 'print(1 ! 2);' "E_SYNTAX_EXPECTED" "ожидалось" "1" "9"
expect_compile_error_with_details "syntax-missing-function-parameter" 'fn add(int left,) { return left; }' "E_SYNTAX_EXPECTED" "ожидалось" "1" "17"
expect_compile_error_with_details "syntax-missing-rbrace" 'fn main() { print(1);' "E_SYNTAX_EXPECTED" "ожидалось" "1" "1"
expect_compile_error_with_details "syntax-missing-rparen" 'print(1;' "E_SYNTAX_EXPECTED" "ожидалось" "1" "7"
expect_compile_error_with_details "syntax-missing-rbracket" 'let arr: int[ = {1};' "E_SYNTAX_EXPECTED" "ожидалось" "1" "14"

# === SEMANTIC ERRORS ===
expect_compile_error_with_details "semantic-redef" 'let x: int = 1; let x: int = 2;' "E_SEMANTIC_REDEF" "переменная" "1" "1"
expect_compile_error_with_details "semantic-type-mismatch" 'let value: int = "wrong";' "E_SEMANTIC_TYPE_MISMATCH" "имеет тип" "1" "1"
expect_compile_error_with_details "semantic-unknown-func" 'let result: int = missing(1);' "E_SEMANTIC_UNKNOWN_FUNC" "неизвестная функция" "1" "19"
expect_compile_error_with_details "semantic-signedness" 'u32 positive = 1; i32 signed = -1; let result: int = positive + signed;' "E_SEMANTIC_SIGNEDNESS" "знаковое и беззнаковое" "1" "1"
expect_compile_error_with_details "semantic-float-to-int" 'int value = 3.0;' "E_SEMANTIC_FLOAT_TO_INT" "float" "1" "11"
expect_compile_error_with_details "semantic-int-range" 'i8 value = 128;' "E_SEMANTIC_INT_RANGE" "вне диапазона" "1" "11"
expect_compile_error_with_details "semantic-shift-range" 'let x: int = 1 << 32;' "E_SEMANTIC_SHIFT_RANGE" "сдвиг" "1" "1"
expect_compile_error_with_details "semantic-arg-count" 'fn add(int a, int b) { return a + b; } add(1);' "E_SEMANTIC_ARG_COUNT" "аргумент" "1" "1"
expect_compile_error_with_details "semantic-arg-type" 'fn twice(int value) { return value * 2; } let result: int = twice("text");' "E_SEMANTIC_ARG_TYPE" "аргумент" "1" "1"
expect_compile_error_with_details "semantic-missing-return" 'fn int maybe(bool flag) { if (flag) { return 1; } }' "E_SEMANTIC_MISSING_RETURN" "возврат" "1" "1"
expect_compile_error_with_details "semantic-unreachable" 'fn void dead() { return; print(2); }' "E_SEMANTIC_UNREACHABLE" "недостижимый" "1" "1"
expect_compile_error_with_details "semantic-unknown-type" 'mystery value = 1;' "E_SEMANTIC_UNKNOWN_TYPE" "неизвестный тип" "1" "1"
expect_compile_error_with_details "semantic-duplicate-field" 'tip Point { int x; int x; }' "E_SEMANTIC_REDEF" "поле" "1" "1"
expect_compile_error_with_details "semantic-builtin-collision" 'fn int max(int left, int right) { return left + right; }' "E_SEMANTIC_BUILTIN_COLLISION" "встроенная функция" "1" "1"
expect_compile_error_with_details "semantic-break-outside-loop" 'brk;' "E_SEMANTIC_INTERNAL" "break вне цикла" "1" "1"
expect_compile_error_with_details "semantic-nested-function" 'if (1) { fn int inner(int value) { return value; } }' "E_SEMANTIC_INTERNAL" "вложенная функция" "1" "1"

# === TYPE ERRORS ===
expect_compile_error_with_details "type-i8-overflow" 'i8 value = 128;' "E_SEMANTIC_INT_RANGE" "вне диапазона" "1" "11"
expect_compile_error_with_details "type-u8-negative" 'u8 value = -1;' "E_SEMANTIC_INT_RANGE" "вне диапазона" "1" "11"
expect_compile_error_with_details "type-int-overflow" 'int value = 2147483648;' "E_SEMANTIC_INT_RANGE" "вне диапазона" "1" "11"
expect_compile_error_with_details "type-f32-overflow" 'f32 value = 1e999;' "E_SEMANTIC_INT_RANGE" "вне диапазона" "1" "11"
expect_compile_error_with_details "type-array-index-float" 'svi rox[] values = [1, 2]; svi dor index = 0.5; values[index] = 9;' "E_SEMANTIC_TYPE_MISMATCH" "индекс.*должен быть целым" "1" "1"

# === REGRESSION DETECTION TEST ===
# This test ensures a known bug (fixed in codegen.cpp: i1 bool must use zext not sext for print)
# If this test fails, the regression has been re-introduced.
expect_compile_error_with_details "regression-bool-print-sext" 'fn int main() { print(true); return 0; }' "" "bool.*print.*sext|i1.*sext" "" ""

IMPORT_DIR="$TMP_DIR/imports"
mkdir -p "$IMPORT_DIR/lib"
cat > "$IMPORT_DIR/lib/base.svx" <<'SC'
fn base() {
    return 21;
}
SC
cat > "$IMPORT_DIR/lib/math.svx" <<'SC'
imp "base.svx";
fn twice(int value) {
    return value * 2;
}
SC
cat > "$IMPORT_DIR/main.svx" <<'SC'
imp "lib/math.svx";
let result: int = twice(base());
print(result);
SC
if ! (cd "$IMPORT_DIR" && timeout --signal=TERM 10s "$SC_BIN" main.svx >"$TMP_DIR/imports.ll" 2>"$TMP_DIR/imports.err"); then
    echo "FAIL: nested relative imports did not compile" >&2
    cat "$TMP_DIR/imports.err" >&2
    exit 1
fi
llvm-as "$TMP_DIR/imports.ll" -o "$TMP_DIR/imports.bc"
llc "$TMP_DIR/imports.bc" -o "$TMP_DIR/imports.s"
clang -no-pie "$TMP_DIR/imports.s" stdlib/runtime.c -o "$TMP_DIR/imports-program" -lm
timeout --signal=TERM 10s "$TMP_DIR/imports-program" > "$TMP_DIR/imports.out"
if [[ "$(cat "$TMP_DIR/imports.out")" != "42" ]]; then
    echo "FAIL: nested imports generated incorrect program output" >&2
    cat "$TMP_DIR/imports.out" >&2
    exit 1
fi
echo "PASS: nested relative imports"

expect_import_error() {
    local name=$1
    local expected=$2
    local work="$TMP_DIR/import-error-$name"
    local status=0
    mkdir -p "$work"
    shift 2
    printf '%s\n' "$@" > "$work/main.svx"
    if timeout --signal=TERM 10s "$SC_BIN" "$work/main.svx" >"$work/output.ll" 2>"$work/error.log"; then
        echo "FAIL: import error '$name' was not detected" >&2
        exit 1
    else
        status=$?
    fi
    if (( status >= 128 )); then
        echo "FAIL: compiler crashed for import error '$name'" >&2
        cat "$work/error.log" >&2
        exit 1
    fi
    if ! grep -Fq "$expected" "$work/error.log"; then
        echo "FAIL: import error '$name' did not explain the failure" >&2
        cat "$work/error.log" >&2
        exit 1
    fi
    echo "PASS: import error '$name'"
}

expect_import_error "missing-module" "не удалось открыть модуль" 'imp "missing.svx";'
mkdir -p "$TMP_DIR/import-cycle"
printf '%s\n' 'imp "b.svx";' > "$TMP_DIR/import-cycle/a.svx"
printf '%s\n' 'imp "a.svx";' > "$TMP_DIR/import-cycle/b.svx"
if timeout --signal=TERM 10s "$SC_BIN" "$TMP_DIR/import-cycle/a.svx" >"$TMP_DIR/import-cycle/out.ll" 2>"$TMP_DIR/import-cycle/error.log"; then
    echo "FAIL: cyclic imports were not rejected" >&2
    exit 1
else
    status=$?
fi
if (( status >= 128 )) || ! grep -Fq "циклический импорт" "$TMP_DIR/import-cycle/error.log"; then
    echo "FAIL: cyclic import did not produce a controlled diagnostic" >&2
    cat "$TMP_DIR/import-cycle/error.log" >&2
    exit 1
fi
echo "PASS: cyclic import detection"

echo "All compiler tests passed."
