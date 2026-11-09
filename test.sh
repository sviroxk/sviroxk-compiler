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

./build.sh
./test-conformance.sh
SC_BIN="$PWD/sc"

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
    "examples/dyn_array_methods.svx"
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
    if (ulimit -c 0; timeout --signal=TERM 15s ./sc "$source" >"$work/module.ll" 2>"$work/compiler.err"); then
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
        for ir_type in 'alloca i8' 'alloca i16' 'alloca i32' 'alloca i64' 'alloca float' 'alloca double'; do
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
    if ! clang -no-pie "$work/program.s" -o "$work/program" -lm 2>"$work/link.err"; then
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
    elif [[ "$source" == "examples/dyn_array_methods.svx" ]]; then
        expected=$'3\n10\n20\n30\n2\n20\n1\n5'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: dynamic array methods result mismatch" >&2
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
    fi
    cat "$work/output.txt"
done

expect_runtime_error() {
    local name=$1
    local source=$2
    local expected=${3:-}
    local work="$TMP_DIR/runtime-error-$name"
    mkdir -p "$work"
    if ! timeout --signal=TERM 15s "$SC_BIN" "$source" >"$work/module.ll" 2>"$work/compiler.err"; then
        echo "FAIL: runtime-error source '$source' did not compile" >&2
        cat "$work/compiler.err" >&2
        exit 1
    fi
    llvm-as "$work/module.ll" -o "$work/module.bc"
    llc "$work/module.bc" -o "$work/program.s"
    clang -no-pie "$work/program.s" -o "$work/program" -lm
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

expect_compile_error() {
    local name=$1
    local source=$2
    local expected=${3:-}
    local work="$TMP_DIR/invalid-$name"
    local status=0
    mkdir -p "$work"
    printf '%s\n' "$source" > "$work/input.svx"

    if (ulimit -c 0; timeout --signal=TERM 10s ./sc "$work/input.svx" >"$work/output.ll" 2>"$work/error.log"); then
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
    if [[ -n "$expected" ]] &&
       ! grep -Fq "$expected" "$work/error.log"; then
        echo "FAIL: '$name' did not produce expected error '$expected'" >&2
        cat "$work/error.log" >&2
        exit 1
    fi
    echo "PASS: invalid input '$name' reported an error without crashing"
}

expect_compile_error "array-negative-index" "svi rox[] lor values = [1, 2]; slov(values[-1]);" "индекс массива выходит за границы"
expect_compile_error "array-upper-index" "svi rox[] lor values = [1, 2]; slov(values[2]);" "индекс массива выходит за границы"

expect_compile_error "missing-expression" 'let value: int = ;' "ожидалось выражение"
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
clang -no-pie "$TMP_DIR/imports.s" -o "$TMP_DIR/imports-program" -lm
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
