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

version_output=$(timeout --signal=TERM 10s "$SC_BIN" --version)
if [[ "$version_output" != "SC compiler 0.3.0" ]]; then
    echo "FAIL: --version returned unexpected text: $version_output" >&2
    exit 1
fi
help_output=$(timeout --signal=TERM 10s "$SC_BIN" --help)
if ! grep -Fq 'Usage: sc <source.svx>' <<< "$help_output"; then
    echo "FAIL: --help is missing source usage" >&2
    exit 1
fi
if timeout --signal=TERM 10s "$SC_BIN" --unknown >/dev/null 2>"${TMPDIR:-/tmp}/sc-unknown-option.log"; then
    echo "FAIL: unknown CLI option returned success" >&2
    exit 1
fi
echo "PASS: CLI version/help/unknown-option contract"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# Parser API regression: run in the same process, including the sanitizer CI build.
read -r -a PARSER_TEST_FLAGS <<< "${CXXFLAGS:-}"
"${CXX:-clang++}" -std=c++17 "${PARSER_TEST_FLAGS[@]}" -I. \
    tests/parser-state.cpp lexer.cpp parser.cpp -o "$TMP_DIR/parser-state"
timeout --signal=TERM 10s "$TMP_DIR/parser-state" 2>"$TMP_DIR/parser-state.err"
if ! grep -Fq 'error[syntax]' "$TMP_DIR/parser-state.err"; then
    echo "FAIL: parser API regression did not report the expected syntax error" >&2
    exit 1
fi

sources=(
    "examples/hello.svx"
    "examples/legacy_square.svx"
    "tests/fixtures/string_basics.svx"
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
    "examples/struct_copy_value.svx"
    "examples/p0_struct_mandatory_field.svx"
    "examples/a2_cast_precedence.svx"
    "examples/a3_void_return.svx"
    "examples/a4_else_if.svx"
    "examples/a6_str_type.svx"
    "examples/a7_char.svx"
    "examples/a8_builtin_names.svx"
    "examples/a9_file_open.svx"
    "examples/contextual_numeric_literals.svx"
    "examples/unsigned_print.svx"
    "tests/fixtures/struct_chain_access.svx"
    "tests/fixtures/struct_nested.svx"
    "tests/fixtures/struct_simple.svx"
    "tests/fixtures/dynamic_array.svx"
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
        # Переменные верхнего уровня — глобалы LLVM (шаг 2), ширина хранения проверяется по ним.
        for ir_type in 'global i8 ' 'global i16 ' 'global i32 ' 'global i64 ' 'global float ' 'global double '; do
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
    # set +e: статус запуска программы проверяется явно ниже (раньше `$?` после
    # if/elif был мёртвым кодом, а set -e обрывал скрипт без сообщения).
    set +e
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
    run_status=$?
    set -e
    if (( run_status != 0 )); then
        echo "FAIL: program failed or exceeded 10s for $source (status $run_status)" >&2
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
        expected=$'3\n10\n20\n30\n2\n20\n1\n4'
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
    elif [[ "$source" == "tests/fixtures/struct_chain_access.svx" ]]; then
        expected=$'1\n2\n3\n4\n42\n7'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: chained struct access mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "tests/fixtures/struct_nested.svx" ]]; then
        expected=$'1\n2\n3\n4'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: nested struct literal mismatch" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "tests/fixtures/dynamic_array.svx" ]]; then
        expected=$'10\n20\n30'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: nw rox[n] must create n zeroed elements" >&2
            cat "$work/output.txt" >&2
            exit 1
        fi
    elif [[ "$source" == "tests/fixtures/struct_simple.svx" ]]; then
        expected=$'5\n10'
        if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
            echo "FAIL: simple struct chain mismatch" >&2
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
    if (( status == 124 )); then
        echo "FAIL: compiler hung (timeout 10s) on '$name'" >&2
        head -c 2000 "$work/error.log" >&2
        exit 1
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

# Мусорный ввод: компилятор обязан завершиться кодом 1 или 2 быстрее 5 секунд,
# не зависнуть (124) и не упасть (>=128), и выдать не больше max_errors ошибок.
expect_garbage_input() {
    local name=$1
    local source=$2
    local expected=$3
    local max_errors=${4:-3}
    local work="$TMP_DIR/garbage-$name"
    local status=0
    mkdir -p "$work"
    printf '%s' "$source" > "$work/input.svx"
    if (ulimit -c 0; timeout --signal=KILL 5s ./sc "$work/input.svx" >"$work/output.ll" 2>"$work/error.log"); then
        echo "FAIL: garbage input '$name' compiled successfully" >&2
        exit 1
    else
        status=$?
    fi
    if (( status != 1 && status != 2 )); then
        echo "FAIL: garbage input '$name': exit status $status (124/137 = hang, >=128 = crash), expected 1 or 2" >&2
        head -c 2000 "$work/error.log" >&2
        exit 1
    fi
    local count
    count=$(grep -c 'error\[' "$work/error.log" || true)
    if (( count < 1 || count > max_errors )); then
        echo "FAIL: garbage input '$name' produced $count diagnostics, expected 1..$max_errors" >&2
        head -c 2000 "$work/error.log" >&2
        exit 1
    fi
    if [[ -n "$expected" ]] && ! grep -Fq "$expected" "$work/error.log"; then
        echo "FAIL: garbage input '$name' did not produce expected error '$expected'" >&2
        cat "$work/error.log" >&2
        exit 1
    fi
    echo "PASS: garbage input '$name' ($count diagnostics, status $status)"
}

expect_garbage_input "stray-rbrace" $'}\n' "лишняя '}'" 1
expect_garbage_input "stray-rbraces" $'}}}\n' "лишняя '}'"
expect_garbage_input "if-without-parens" $'let x: int = 1;\nif x == 1 { print(1); }\nprint(2);\n' "получено 'x'"
expect_garbage_input "array-param-type" $'fn int first(int[] a) { return 0; }\nprint(1);\n' "ожидалось имя параметра"
expect_garbage_input "struct-param-type" $'tip P { int x = 0; }\nfn int getx(P p) { return 1; }\nprint(1);\n' "неизвестный тип параметра 'P'"
expect_garbage_input "keyword-function-name" $'fn int f32(int a) { return a; }\nprint(1);\n' "'f32' — зарезервированное слово"
expect_garbage_input "truncated-fn" 'fn int f(' ""
expect_garbage_input "truncated-let" 'let x: int = ' "ожидалось выражение"
expect_garbage_input "truncated-if" 'if (1) {' ""
expect_garbage_input "truncated-struct" 'tip P {' ""
expect_garbage_input "truncated-call" 'print(max(1, ' ""
expect_garbage_input "truncated-cop" 'cop f(1 2 3' ""
expect_garbage_input "stray-rbrace-in-fn" $'fn void f() { print(1); } }\nprint(2);\n' "лишняя '}'" 1
many_errors=""
for ((i = 0; i < 50; i++)); do many_errors+=$'let a: int = ;\n'; done
expect_garbage_input "error-limit" "$many_errors" "слишком много синтаксических ошибок" 21

expect_compile_error "multiple-syntax-errors" $'let a: int = ;\nlet b: int = ;'
expect_compile_error "diagnostic-source-caret" 'let value: int = ;' "ожидалось выражение"

expect_compile_error_file() {
    local source=$1
    local expected=$2
    local name
    name=$(basename "$source" .svx)
    local work="$TMP_DIR/invalid-file-$name"
    local status=0
    mkdir -p "$work"
    if (ulimit -c 0; timeout --signal=TERM 10s ./sc "$source" >"$work/output.ll" 2>"$work/error.log"); then
        echo "FAIL: invalid example '$source' compiled successfully" >&2
        exit 1
    else
        status=$?
    fi
    if (( status != 1 && status != 2 )); then
        echo "FAIL: example '$source': exit status $status, expected 1 or 2" >&2
        head -c 2000 "$work/error.log" >&2
        exit 1
    fi
    if ! grep -Fq "$expected" "$work/error.log"; then
        echo "FAIL: example '$source' did not produce expected error '$expected'" >&2
        cat "$work/error.log" >&2
        exit 1
    fi
    echo "PASS: invalid example '$source' rejected with expected error"
}

expect_compile_error_file "examples/a15_builtin_collision.svx" "зарезервировано встроенной функцией"
expect_compile_error_file "examples/a19_nested_function.svx" "вложенные определения функций"
expect_compile_error_file "examples/a20_unreachable.svx" "недостижимый код"
expect_compile_error_file "examples/a5_positions.svx" "a5_positions.svx:5:13: error[semantic]"
expect_compile_error_file "examples/array_bounds_negative.svx" "индекс массива выходит за границы"
expect_compile_error_file "examples/array_bounds_upper.svx" "индекс массива выходит за границы"
expect_compile_error_file "examples/continue_counted_loop.svx" "недостижимый код"
expect_compile_error_file "examples/diagnostic_caret.svx" "^"
expect_compile_error_file "examples/p0_struct_copy_ownership.svx" "неявное копирование структуры"
expect_compile_error_file "examples/p0_struct_recursive.svx" "рекурсивное определение структуры"

RUNTIME_DIR="$TMP_DIR/runtime-sources"
mkdir -p "$RUNTIME_DIR"
printf '%s\n' 'svi rox[] lor values = [1, 2, 3];' 'let i: int = 5;' 'print(values[i]);' > "$RUNTIME_DIR/static_read.svx"
printf '%s\n' 'svi rox[] lor values = [1, 2, 3];' 'let i: int = 0 - 1;' 'values[i] = 4;' > "$RUNTIME_DIR/static_write.svx"
printf '%s\n' 'ptr values = nw rox[2];' 'values.push(7);' 'let i: int = 3;' 'print(values[i]);' > "$RUNTIME_DIR/dynamic_read.svx"
expect_runtime_error "static-array-read-oob" "$RUNTIME_DIR/static_read.svx" "array index out of bounds at line 3, column 7"
expect_runtime_error "static-array-write-oob" "$RUNTIME_DIR/static_write.svx" "array index out of bounds at line 3, column 1"
expect_runtime_error "dynamic-array-read-oob" "$RUNTIME_DIR/dynamic_read.svx" "array index out of bounds at line 4, column 7"

# Программа из строки: компилируется, запускается, вывод совпадает с ожидаемым.
expect_program_output() {
    local name=$1
    local source=$2
    local expected=$3
    local work="$TMP_DIR/program-$name"
    local status=0
    mkdir -p "$work"
    printf '%s\n' "$source" > "$work/input.svx"
    if (ulimit -c 0; timeout --signal=TERM 10s ./sc "$work/input.svx" >"$work/module.ll" 2>"$work/compiler.err"); then
        :
    else
        status=$?
        echo "FAIL: program '$name' did not compile (status $status)" >&2
        head -c 2000 "$work/compiler.err" >&2
        exit 1
    fi
    llvm-as "$work/module.ll" -o "$work/module.bc"
    llc "$work/module.bc" -o "$work/program.s"
    clang -no-pie "$work/program.s" -o "$work/program" -lm
    if ! (ulimit -c 0; timeout --signal=TERM 10s "$work/program" >"$work/output.txt" 2>"$work/runtime.err"); then
        echo "FAIL: program '$name' failed at runtime" >&2
        cat "$work/runtime.err" >&2
        exit 1
    fi
    if [[ "$(cat "$work/output.txt")" != "$expected" ]]; then
        echo "FAIL: program '$name' output mismatch" >&2
        echo "expected:" >&2; printf '%s\n' "$expected" >&2
        echo "actual:" >&2; cat "$work/output.txt" >&2
        exit 1
    fi
    echo "PASS: program '$name'"
}

# Шаг 2: глобальные переменные, структуры и имена в функциях.
expect_program_output "global-read-in-fn" $'let g = 5;\nfn int h() { return g; }\nprint(h());' '5'
expect_program_output "global-write-in-fn" $'let counter: int = 0;\nfn void inc() { counter = counter + 1; }\ninc();\ninc();\nprint(counter);' '2'
expect_program_output "global-in-recursion" $'let depth: int = 0;\nfn int fact(int n) { depth = depth + 1; if (n <= 1) { return 1; } return n * fact(n - 1); }\nprint(fact(5));\nprint(depth);' $'120\n5'
expect_program_output "global-declared-after-fn" $'fn void show() { print(total); }\nlet total: int = 7;\nshow();' '7'
expect_program_output "global-array-in-fn" $'svi rox[] lor arr = [1, 2, 3];\nfn int second() { return arr[1]; }\nprint(second());' '2'
expect_program_output "global-const-and-param-shadow" $'const limit: int = 10;\nlet x: int = 1;\nfn int f(int x) { return x + limit; }\nprint(f(5));\nprint(x);' $'15\n1'
expect_program_output "block-local-vs-global" $'if (1) { let inner: int = 3; print(inner); }\nlet inner: int = 9;\nfn int f() { return inner; }\nprint(f());' $'3\n9'
expect_program_output "globals-named-like-runtime" $'let printf = 3;\nlet puts = 4;\nfn int f() { return printf + puts; }\nprint(f());' '7'
expect_program_output "struct-in-fn" $'tip P { int x = 4; }\nfn int f() { svi tip P lor p; return p.x; }\nprint(f());' '4'
expect_program_output "ar-and-rep-together" $'print(ar("abc"));\nprint(rep("hello world", "world", "SC"));' $'3\nhello SC'
expect_program_output "user-fn-named-strlen" $'fn int strlen(int x) { return x + 1; }\nprint(strlen(4));\nprint(ar("abcd"));' $'5\n4'
expect_compile_error "user-main-reserved" $'fn void main() { print(111); }\nprint(1);' "имя функции 'main' зарезервировано"
expect_compile_error "return-dynamic-array" $'fn ptr make() { ptr v = nw rox[3]; return v; }\nprint(1);' "возврат динамического массива из функции пока не поддержан"

# Шаг 3: строки в куче, файлы, nw, push, разыменование.
expect_program_output "str-huge-double" 'print(ar(str(1e300)));' '308'
expect_program_output "str-returned-from-fn" $'fn str mk(int n) { return str(n); }\nstr a = mk(12);\nstr b = mk(345);\nprint(a);\nprint(b);' $'12\n345'
expect_program_output "str-locals-in-fn" $'fn str both(int a, int b) { str x = str(a); str y = str(b); return x; }\nprint(both(7, 8));\nprint(str(3.5));\nprint(str(-2147483647));' $'7\n3.500000\n-2147483647'
expect_program_output "file-roundtrip" "file f = atk(\"$TMP_DIR/roundtrip.txt\", \"w\");
zap(f, \"line1\");
zak(f);
file g = atk(\"$TMP_DIR/roundtrip.txt\", \"r\");
str t = pro(g);
print(t);
zak(g);" 'line1'
expect_program_output "nw-has-length-n" $'ptr v = nw rox[3];\nv[0] = 10;\nprint(v[0]);\nprint(v.len());\nprint(v[2]);' $'10\n3\n0'
expect_program_output "nw-empty-then-push" $'ptr v = nw rox[0];\nprint(v.len());\nv.push(5);\nprint(v[0]);' $'0\n5'
expect_program_output "deref-f64" $'f64 f = 2.5;\nptr p = &f;\nprint(*p);\n*p = 4.25;\nprint(f);' $'2.500000\n4.250000'
expect_program_output "deref-int" $'int x = 7;\nptr p = &x;\n*p = *p + 1;\nprint(x);\nprint(*p);' $'8\n8'
printf '%s\n' 'file f = atk("no-such-dir/missing.txt", "r");' 'str t = pro(f);' 'print(t);' > "$RUNTIME_DIR/missing_file.svx"
expect_runtime_error "open-missing-file" "$RUNTIME_DIR/missing_file.svx" "cannot open file 'no-such-dir/missing.txt' at line 1, column 10"
expect_compile_error "push-string-into-int-array" $'ptr v = nw rox[3];\nv.push("hello");' "элемент массива ожидает тип"

# Step 4c: lexical scope cleanup, LIFO, return and loop exits.
expect_program_output "defer-lifo-late-evaluation" "$(cat examples/defer.svx)" $'0\n2\n3'
expect_program_output "defer-branch-return" $'fn int choose(bool early) { defer print(10); if (early) { defer print(20); return 1; } else { defer print(30); } return 2; }\nprint(choose(true)); print(choose(false));' $'20\n10\n1\n30\n10\n2'
expect_program_output "defer-shadow-binding" $'fn void run() { int value = 1; defer print(value); if (true) { int value = 2; defer print(value); return; } }\nrun();' $'2\n1'
expect_program_output "defer-return-in-loop" $'fn int run() { defer print(9); for i in 0..3 { defer print(i); if (i == 1) { return 7; } } return 0; }\nprint(run());' $'0\n1\n9\n7'
expect_program_output "defer-loop-forms" $'int n = 0; while (n < 2) { n = n + 1; defer print(n); }\ncert ceh (2) { defer print(3); }\ncert usy (n < 3) { n = n + 1; defer print(n); }\nfor i in 4..6 { defer print(i); }\nsvi rox[] lor values = [6, 7]; for item in values { defer print(item); }' $'1\n2\n3\n3\n3\n4\n5\n6\n7'
expect_program_output "defer-break-continue" $'defer print(9); for i in 0..4 { defer print(i); if (i == 0) { continue; } if (i == 2) { break; } print(8); }' $'0\n8\n1\n2\n9'
expect_program_output "defer-nested-loop" $'for i in 1..3 { defer print(i); loop { defer print(7); break; } print(8); }' $'7\n8\n1\n7\n8\n2'
expect_program_output "defer-call-and-assignment" $'fn void show(int n) { print(n); }\nfn int run() { int n = 1; defer show(n); defer n = 4; return n; }\nprint(run());' $'4\n1'
expect_compile_error "defer-return-forbidden" 'fn int run() { defer return 1; return 0; }' 'defer: переходы управления и объявления пока запрещены'
expect_compile_error "defer-break-forbidden" 'loop { defer break; break; }' 'defer: переходы управления и объявления пока запрещены'
expect_compile_error "defer-continue-forbidden" 'loop { defer continue; break; }' 'defer: переходы управления и объявления пока запрещены'
expect_compile_error "defer-declaration-forbidden" 'defer int value = 1;' 'defer: переходы управления и объявления пока запрещены'
expect_compile_error "defer-nested-forbidden" 'defer defer print(1);' 'defer: ожидается обычная операция или вызов'
expect_compile_error "defer-control-forbidden" 'defer if (true) { print(1); }' 'defer: ожидается обычная операция или вызов'
expect_compile_error "defer-unknown-name" 'defer print(missing);' "неизвестное имя 'missing'"
expect_compile_error "defer-invalid-delete" 'int value = 1; defer del value;' 'освободить можно только динамический массив или ptr'

# drop: освобождение динамического массива / закрытие файла, повтор и использование после drop — ошибки компиляции.
expect_program_output "drop-dyn-array" $'ptr v = nw rox[3];\nv[0] = 7;\nprint(v[0]);\ndrop v;\nprint(1);' $'7\n1'
expect_program_output "drop-file-flushes" "file f = atk(\"$TMP_DIR/drop.txt\", \"w\");
zap(f, \"dropped\");
drop f;
file g = atk(\"$TMP_DIR/drop.txt\", \"r\");
print(pro(g));
drop g;" 'dropped'
expect_program_output "drop-in-function-and-loop" $'fn int first() { ptr v = nw rox[2]; v[0] = 4; int r = v[0]; drop v; return r; }\nprint(first());\nfor i in 0..2 { ptr w = nw rox[1]; w[0] = i; print(w[0]); drop w; }' $'4\n0\n1'
expect_program_output "defer-drop" $'fn void run() { ptr v = nw rox[2]; defer drop v; defer print(v[1]); v[1] = 5; }\nrun();\nprint(6);' $'5\n6'
expect_program_output "drop-in-one-branch" $'ptr v = nw rox[2];\nif (true) { drop v; } else { print(v[0]); }\nprint(3);' '3'
expect_compile_error "drop-use-after" $'ptr v = nw rox[3];\ndrop v;\nprint(v[0]);' "'v' используется после drop (строка 2)"
expect_compile_error "drop-use-after-branch" $'ptr v = nw rox[3];\nif (true) { drop v; }\nprint(v.len());' "'v' используется после drop (строка 2)"
expect_compile_error "drop-file-close-after" "file f = atk(\"$TMP_DIR/drop2.txt\", \"w\");
drop f;
zak(f);" "'f' используется после drop (строка 2)"
expect_compile_error "drop-twice" $'ptr v = nw rox[3];\ndrop v;\ndrop v;' "повторный drop 'v' (уже освобождено в строке 2)"
expect_compile_error "drop-after-defer-drop" $'ptr v = nw rox[3];\ndefer drop v;\ndrop v;' "повторный drop 'v' (defer drop в строке 2)"
expect_compile_error "drop-string" $'str s = "a";\ndrop s;' 'drop поддерживается только для динамического массива и file'
expect_compile_error "drop-raw-pointer" $'int x = 1;\nptr p = &x;\ndrop p;' 'drop поддерживается только для динамического массива и file'
expect_compile_error "drop-outer-in-loop" $'ptr v = nw rox[3];\nwhile (true) { drop v; }' "drop 'v' внутри цикла или функции"
expect_compile_error "drop-global-in-function" $'ptr v = nw rox[3];\nfn void f() { drop v; }\nf();' "drop 'v' внутри цикла или функции"
expect_compile_error "drop-without-name" 'drop 5;' 'после drop ожидалось имя переменной'
expect_program_output "defer-drop-after-return-value" $'fn int f(int x) {\n  ptr v = nw rox[2];\n  defer drop v;\n  v[0] = x;\n  return v[0] + 1;\n}\nprint(f(4));' '5'
expect_program_output "defer-after-return-call" $'fn int g() { print(1); return 3; }\nfn int f() { defer print(2); return g(); }\nprint(f());' $'1\n2\n3'
expect_program_output "defer-assign-after-return-value" $'fn int f() { ptr v = nw rox[1]; v[0] = 4; defer v[0] = 100; return v[0]; }\nprint(f());' '4'
expect_program_output "defer-struct-param-return" $'tip P { int x = 0; }\nfn int f(tip P value) { defer value.x = 100; return value.x; }\nsvi tip P lor p = { x = 3; };\nprint(f(p));\nprint(p.x);' $'3\n3'

# Целые (шаг 5а): деление на ноль, сдвиги, переполнение — ошибка, а не молчаливая порча значения.
int_runtime() { printf '%s' "$2" > "$RUNTIME_DIR/int_$1.svx"; expect_runtime_error "int-$1" "$RUNTIME_DIR/int_$1.svx" "$3"; }
int_runtime div-zero $'int a = 5;\nint b = 0;\nprint(a / b);' "division by zero at line 3, column 9"
int_runtime rem-zero $'int a = 5;\nint b = 0;\nprint(a % b);' "division by zero at line 3, column 9"
int_runtime min-div-minus-one $'int a = -2147483647 - 1;\nint b = -1;\nprint(a / b);' "integer overflow at line 3, column 9"
int_runtime shift-range $'int a = 1;\nint s = 40;\nprint(a << s);' "shift amount out of range at line 3, column 9"
int_runtime add-overflow $'int a = 2147483647;\nprint(a + 1);' "integer overflow at line 2, column 9"
int_runtime sub-overflow $'int a = -2147483647;\nprint(a - 2);' "integer overflow at line 2, column 9"
int_runtime mul-overflow-i64 $'i64 a = 4611686018427387904;\nprint(a * 2);' "integer overflow at line 2, column 9"
int_runtime unsigned-underflow $'u32 a = 0;\nprint(a - 1);' "integer overflow at line 2, column 9"
int_runtime negate-min $'int a = -2147483647 - 1;\nprint(-a);' "integer overflow at line 2, column 7"
int_runtime pow-overflow $'int a = 2;\nprint(a ** 40);' "integer overflow at line 2, column 9"
expect_program_output "int-checked-ok" $'int a = 7;\nint b = -2;\nprint(a / b);\nprint(a % b);\nint s = 4;\nprint(1 << s);\nint m = 2147483646;\nprint(m + 1);\nint x = 3;\nprint(2 ** 10);\nprint(x * x - 10);' $'-3\n1\n16\n2147483647\n1024\n-1'
expect_program_output "neg-power-precedence" $'print(-2 ** 2);\nint x = 3;\nprint(-x ** 2);\nprint(2 ** -1 + 1);' $'-4\n-9\n1'
expect_compile_error "int-const-div-zero" $'int a = 5;\nprint(a / 0);' 'деление на ноль'
expect_compile_error "int-const-rem-zero" $'int a = 5;\nprint(a % 0);' 'деление на ноль'
expect_compile_error "int-const-shift" $'int a = 1;\nprint(a << 40);' 'счётчик сдвига должен быть меньше ширины левого операнда'
expect_compile_error "int-const-add-overflow" 'print(2147483647 + 1);' "переполнение: значение константного выражения 2147483648 не помещается в тип 'int/i32'"
expect_compile_error "int-const-mul-overflow" 'print(65536 * 65536);' "переполнение: значение константного выражения 4294967296"
expect_compile_error "int-const-pow-overflow" 'print(2 ** 40);' "переполнение: значение константного выражения 1099511627776"

# Приведения (шаг 5б): явное — с проверкой диапазона; неявное — только без потерь.
cast_runtime() { printf '%s' "$2" > "$RUNTIME_DIR/cast_$1.svx"; expect_runtime_error "cast-$1" "$RUNTIME_DIR/cast_$1.svx" "$3"; }
cast_runtime i64-to-int $'i64 a = 5000000000;\nprint((int)a);' "value out of range for type 'i32' at line 2, column 8"
cast_runtime negative-float-to-uint $'f64 a = -1.5;\nprint((uint)a);' "value out of range for type 'u32' at line 2, column 8"
cast_runtime int-to-i8 $'int a = 300;\nprint((i8)a);' "value out of range for type 'i8' at line 2, column 8"
cast_runtime negative-to-u32 $'int a = -1;\nprint((u32)a);' "value out of range for type 'u32' at line 2, column 8"
cast_runtime nan-to-int $'f64 z = 0.0;\nf64 n = z / z;\nprint((int)n);' "value out of range for type 'i32' at line 3, column 8"
cast_runtime huge-float-to-i64 $'f64 a = 1e300;\nprint((i64)a);' "value out of range for type 'i64' at line 2, column 8"
expect_program_output "cast-in-range" $'int a = 100;\nprint((i8)a);\nf64 f = 3.75;\nprint((int)f);\nu32 u = 7;\nprint((int)u);\nint m = -5;\nprint((i64)m);\nprint((u8)255);\nprint((uint)3.9);\nprint((i64)5000000000);' $'100\n3\n7\n-5\n255\n3\n5000000000'
expect_program_output "implicit-widening" $'i8 a = 5;\nint b = a;\ni64 c = b;\nu8 d = 7;\nu32 e = d;\nint g = d;\nf64 h = c;\nf32 k = 1.5;\nf64 l = k;\nprint(b + c + e + g);\nprint(h + l);' $'24\n6.500000'
expect_program_output "small-literals-without-cast" $'u8 b = 200;\ni8 c = -5;\nb = 201;\nfn u8 f() { return 255; }\nfn void g(i8 x) { print(x); }\nprint(b);\nprint(c);\nprint(f());\ng(-128);' $'201\n-5\n255\n-128'
expect_compile_error "cast-const-int-overflow" 'print((int)5000000000);' "числовой литерал вне диапазона типа 'int/i32'"
expect_compile_error "cast-const-negative-float-to-uint" 'print((uint)-1.5);' "значение -1.5 не помещается в тип 'uint/u32'"
expect_compile_error "cast-const-i8" 'print((i8)300);' "числовой литерал вне диапазона типа 'i8'"
expect_compile_error "implicit-i64-to-int" $'i64 a = 1;\nint b = a;' 'нужно явное приведение'
expect_compile_error "implicit-int-to-uint" $'int a = 1;\nuint b = a;' 'нужно явное приведение'
expect_compile_error "implicit-i64-to-f32" $'i64 a = 1;\nf32 b = a;' 'нужно явное приведение'
expect_compile_error "implicit-float-to-int" $'f64 a = 1.5;\nint b = a;' 'нужно явное приведение'
expect_compile_error "implicit-f64-to-f32" $'f64 a = 1.5;\nf32 b = a;' 'нужно явное приведение'
expect_compile_error "implicit-narrow-argument" $'fn void f(i8 x) { print(x); }\nint a = 1;\nf(a);' "аргумент 1 функции 'f': ожидался 'i8', получен 'int/i32'; возможна потеря значения"
expect_compile_error "implicit-narrow-return" $'fn u8 f(int x) { return x; }\nprint(f(1));' "функция должна вернуть 'byte/u8', получено 'int/i32'; возможна потеря"
expect_compile_error "implicit-narrow-assign" $'u8 b = 1;\nint a = 300;\nb = a;' "нельзя присвоить 'int/i32' значению типа 'byte/u8'; возможна потеря"
expect_compile_error "small-literal-out-of-range" 'u8 b = 256;' "числовой литерал вне диапазона типа 'byte/u8'"

# Step 4b: PR22 struct parameters are aggregate copies, not aliases.
expect_program_output "struct-by-value-copy" "$(cat examples/struct_by_value.svx)" $'17\n3\n4\n3'
expect_program_output "struct-by-value-nested" $'tip Inner { int x = 2; }\ntip Outer { tip Inner inner = { x = 7; }; int y = 3; }\nfn int change(tip Outer value) { svi tip Inner lor replacement = { x = 9; }; value.inner = replacement; return value.inner.x + value.y; }\nsvi tip Outer lor original = { inner = { x = 5; }; y = 4; };\nprint(change(original)); print(original.inner.x);' $'13\n5'
expect_program_output "struct-by-value-forward" $'fn int total(int offset, tip Pair value);\ntip Pair { int x = 0; }\nfn int total(int offset, tip Pair value) { return offset + value.x; }\nsvi tip Pair lor value = { x = 5; }; print(total(10, value));' '15'
expect_program_output "struct-by-value-layout" $'tip Sample { u8 tag = 1; f64 weight = 2.5; }\nfn f64 weight(tip Sample value) { value.weight = 4.25; return value.weight; }\nsvi tip Sample lor value; print(weight(value)); print(value.weight); print(value.tag);' $'4.250000\n2.500000\n1'
expect_compile_error "struct-param-unknown-type" 'fn int read(tip Missing value) { return 0; }' "неизвестный тип параметра 'Missing'"
expect_compile_error "struct-param-wrong-type" $'tip Pair { int x = 0; }\nfn int read(tip Pair value) { return value.x; }\nprint(read(1));' "аргумент 1 функции 'read': ожидался 'Pair'"
expect_compile_error "struct-param-missing-field" $'tip Pair { int x = 0; }\nfn int read(tip Pair value) { return value.missing; }' "у структуры 'Pair' нет поля 'missing'"
expect_compile_error "struct-param-forward-mismatch" $'tip A { int x = 0; } tip B { int x = 0; }\nfn int read(tip A value); fn int read(tip B value) { return value.x; }' "сигнатура функции 'read' не совпадает"
expect_compile_error "struct-field-tip-unknown" 'tip Outer { tip Missing inner; }' "неизвестный тип поля 'Missing'"
expect_compile_error "struct-field-tip-recursive" 'tip Recursive { tip Recursive next; }' 'рекурсивное определение структуры'

# pop() expression: both PHI incoming blocks must be actual CFG predecessors.
expect_program_output "pop-expression-after-growth" 'ptr v = nw rox[0]; v.push(1); print(v.pop()); print(v.len()); del v;' $'1\n0'
expect_program_output "pop-expression-empty" 'ptr v = nw rox[0]; print(v.pop()); print(v.pop()); print(v.len()); del v;' $'0\n0\n0'
expect_program_output "pop-expression-lifo-and-control-flow" $'ptr v = nw rox[0];\nv.push(10); v.push(20); v.push(30); v.push(40); v.push(50);\nprint(v.pop()); print(v.len()); print(v.pop() + v.pop());\nif (v.len() == 2) { print(v.pop()); } else { print(-1); }\nprint(v.pop()); print(v.pop()); print(v.len()); del v;' $'50\n4\n70\n20\n10\n0\n0'
expect_program_output "pop-expression-float-zero" 'ptr v = nw f64[0]; print(v.pop()); v.push(2.5); print(v.pop()); print(v.pop()); del v;' $'0.000000\n2.500000\n0.000000'

# Объявление функции без тела (fn T name(...);) и взаимная рекурсия.
expect_program_output "forward-decl-call-before-def" $'fn int triple(int x);\nprint(triple(4));\nfn int triple(int x) { return x * 3; }' '12'
expect_program_output "forward-decl-mutual-recursion" $'fn bool ev(int n);\nfn bool od(int n) { if (n == 0) { return false; } return ev(n - 1); }\nfn bool ev(int n) { if (n == 0) { return true; } return od(n - 1); }\nprint(ev(10));\nprint(od(7));' $'1\n1'
expect_compile_error "forward-decl-without-definition" $'fn int g(int x);\nprint(1);' "функция 'g' объявлена, но не определена"
expect_compile_error "forward-decl-param-mismatch" $'fn int g(int x);\nfn int g(f64 x) { return 1; }\nprint(g(1.0));' "сигнатура функции 'g' не совпадает"
expect_compile_error "forward-decl-return-mismatch" $'fn f64 g(int x);\nfn int g(int x) { return 1; }\nprint(g(1));' "сигнатура функции 'g' не совпадает"
expect_compile_error "duplicate-function-definition" $'fn int g(int x) { return 1; }\nfn int g(int x) { return 2; }\nprint(g(1));' "повторное определение функции 'g'"
expect_compile_error "duplicate-forward-decl" $'fn int g(int x);\nfn int g(int x);\nfn int g(int x) { return 1; }\nprint(g(1));' "повторное объявление функции 'g'"

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
expect_compile_error "recursive-struct-definition" 'tip BadRecursive { rox value; BadRecursive next; }' "рекурсивное определение структуры"
expect_compile_error "struct-copy-ownership" 'tip Container { rox id; ptr data; } svi tip Container lor a; a.id = 1; a.data = null; svi tip Container lor b; b = a;' "неявное копирование структуры"
expect_compile_error "struct-ptr-field-access" 'tip Node { rox value; ptr next; } svi tip Node lor n1; svi tip Node lor n2; n1.value = 10; n1.next = &n2; n2.value = 20; n2.next = null; print(n1.next.value);' "не имеет полей"

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

# AddressSanitizer на собранных программах: каждая уже собранная и проверенная
# программа (module.ll + output.txt) пересобирается с -fsanitize=address и должна
# дать тот же вывод без отчётов ASan. Утечки не проверяются: строки в куче пока
# не освобождаются (освобождение при выходе из области — этап модели памяти).
asan_probe="$TMP_DIR/asan-probe"
mkdir -p "$asan_probe"
printf 'int main(void) { return 0; }\n' > "$asan_probe/probe.c"
if clang -fsanitize=address "$asan_probe/probe.c" -o "$asan_probe/probe" 2>/dev/null &&
   "$asan_probe/probe"; then
    asan_count=0
    for work in "$TMP_DIR"/*/; do
        work=${work%/}
        [[ -f "$work/module.ll" && -f "$work/output.txt" ]] || continue
        case "$work" in *input*) continue ;; esac
        sed -E 's/^(define [^{]*)\{$/\1 sanitize_address {/' "$work/module.ll" > "$work/asan.ll"
        if ! clang -no-pie -fsanitize=address -O0 "$work/asan.ll" -o "$work/asan-program" -lm 2>"$work/asan-build.err"; then
            echo "FAIL: could not build $work with AddressSanitizer" >&2
            cat "$work/asan-build.err" >&2
            exit 1
        fi
        if ! (cd "$work" && ASAN_OPTIONS=detect_leaks=0 timeout --signal=TERM 20s ./asan-program \
                </dev/null >"$work/asan-output.txt" 2>"$work/asan.err"); then
            echo "FAIL: AddressSanitizer reported an error in $work" >&2
            head -c 3000 "$work/asan.err" >&2
            exit 1
        fi
        if ! cmp -s "$work/output.txt" "$work/asan-output.txt"; then
            echo "FAIL: output under AddressSanitizer differs for $work" >&2
            exit 1
        fi
        asan_count=$((asan_count + 1))
    done
    echo "PASS: AddressSanitizer clean on $asan_count compiled programs"
    bash tests/memory-safety.sh "$SC_BIN" "$TMP_DIR/memory-safety"
else
    echo "SKIP: clang cannot build with -fsanitize=address here; ASan check of compiled programs skipped"
fi

echo "All compiler tests passed."
