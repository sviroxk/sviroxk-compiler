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
tests/build-flags.sh
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
"${CXX:-clang++}" -std=c++20 "${PARSER_TEST_FLAGS[@]}" -I. \
    tests/parser-state.cpp lexer.cpp parser.cpp -o "$TMP_DIR/parser-state"
timeout --signal=TERM 10s "$TMP_DIR/parser-state" 2>"$TMP_DIR/parser-state.err"
if ! grep -Fq 'error[syntax]' "$TMP_DIR/parser-state.err"; then
    echo "FAIL: parser API regression did not report the expected syntax error" >&2
    exit 1
fi

# Lexer keyword table: every keyword maps to the same token as before.
"${CXX:-clang++}" -std=c++20 "${PARSER_TEST_FLAGS[@]}" -I. \
    tests/lexer-keywords.cpp lexer.cpp -o "$TMP_DIR/lexer-keywords"
timeout --signal=TERM 10s "$TMP_DIR/lexer-keywords"

# Every AST node has a source position: examples, fixtures, stdlib and all spec programs.
"${CXX:-clang++}" -std=c++20 "${PARSER_TEST_FLAGS[@]}" -I. \
    tests/ast-positions.cpp lexer.cpp parser.cpp -o "$TMP_DIR/ast-positions"
mkdir -p "$TMP_DIR/spec-programs"
awk -v dir="$TMP_DIR/spec-programs" '
    /^```/ { if (inside) { inside = 0; file = ""; next }
             inside = 1; if (substr($0, 4) == "sc") file = dir "/" (++n) ".svx"; next }
    inside && file != "" { print > file }
' docs/spec.md
timeout --signal=TERM 10s "$TMP_DIR/ast-positions" examples/*.svx tests/fixtures/*.svx \
    stdlib/*/*.svx "$TMP_DIR"/spec-programs/*.svx

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
            echo "FAIL: new int[n] must create n zeroed elements" >&2
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
    # 4-й аргумент (если задан) подаётся программе на stdin.
    if (ulimit -c 0; printf '%s' "${4:-}" | timeout --signal=TERM 10s "$work/program" >"$work/output.log" 2>"$work/error.log"); then
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
    # Диагностика выполнения — только в stderr (D-007).
    if ! grep -Fq "$expected" "$work/error.log" ||
       grep -Fq "SC runtime error" "$work/output.log"; then
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
expect_garbage_input "struct-param-type" $'struct P { int x = 0; }\nfn int getx(Q p) { return 1; }\nprint(1);\n' "неизвестный тип параметра 'Q'"
expect_garbage_input "keyword-function-name" $'fn int f32(int a) { return a; }\nprint(1);\n' "'f32' — зарезервированное слово"
expect_garbage_input "truncated-fn" 'fn int f(' ""
expect_garbage_input "truncated-let" 'let x: int = ' "ожидалось выражение"
expect_garbage_input "truncated-if" 'if (1) {' ""
expect_garbage_input "truncated-struct" 'struct P {' ""
expect_garbage_input "truncated-call" 'print(max(1, ' ""
expect_garbage_input "truncated-cop" 'f(1 2 3' ""
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
expect_compile_error_file "examples/a5_positions.svx" "a5_positions.svx:5:14: error[semantic]"
expect_compile_error_file "examples/array_bounds_negative.svx" "индекс массива выходит за границы"
expect_compile_error_file "examples/array_bounds_upper.svx" "индекс массива выходит за границы"
expect_compile_error_file "examples/continue_counted_loop.svx" "недостижимый код"
expect_compile_error_file "examples/diagnostic_caret.svx" "^"
expect_compile_error_file "examples/p0_struct_copy_ownership.svx" "неявное копирование структуры"
expect_compile_error_file "examples/p0_struct_recursive.svx" "рекурсивное определение структуры"

RUNTIME_DIR="$TMP_DIR/runtime-sources"
mkdir -p "$RUNTIME_DIR"
printf '%s\n' 'int[] values = [1, 2, 3];' 'let i: int = 5;' 'print(values[i]);' > "$RUNTIME_DIR/static_read.svx"
printf '%s\n' 'int[] values = [1, 2, 3];' 'let i: int = 0 - 1;' 'values[i] = 4;' > "$RUNTIME_DIR/static_write.svx"
printf '%s\n' 'ptr values = new int[2];' 'values.push(7);' 'let i: int = 3;' 'print(values[i]);' > "$RUNTIME_DIR/dynamic_read.svx"
expect_runtime_error "static-array-read-oob" "$RUNTIME_DIR/static_read.svx" "array index out of bounds at line 3, column 7"
expect_runtime_error "static-array-write-oob" "$RUNTIME_DIR/static_write.svx" "array index out of bounds at line 3, column 1"
expect_runtime_error "dynamic-array-read-oob" "$RUNTIME_DIR/dynamic_read.svx" "array index out of bounds at line 4, column 7"
# D-007: обычный вывод программы — в stdout, диагностика выполнения — только в stderr.
printf '%s\n' 'print("before");' 'int[] values = [1];' 'let i: int = 2;' 'print(values[i]);' > "$RUNTIME_DIR/stderr_split.svx"
expect_runtime_error "runtime-error-stderr" "$RUNTIME_DIR/stderr_split.svx" "SC runtime error: array index out of bounds at line 4, column 7"
if [[ "$(cat "$TMP_DIR/runtime-error-runtime-error-stderr/output.log")" != "before" ]]; then
    echo "FAIL: stdout before a runtime error must be exactly the program output" >&2
    cat "$TMP_DIR/runtime-error-runtime-error-stderr/output.log" >&2
    exit 1
fi
echo "PASS: runtime error goes to stderr, stdout keeps program output"

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
    # 4-й аргумент (если задан) подаётся программе на stdin.
    if ! (ulimit -c 0; printf '%s' "${4:-}" | timeout --signal=TERM 10s "$work/program" >"$work/output.txt" 2>"$work/runtime.err"); then
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
expect_program_output "global-array-in-fn" $'int[] arr = [1, 2, 3];\nfn int second() { return arr[1]; }\nprint(second());' '2'
expect_program_output "global-const-and-param-shadow" $'const limit: int = 10;\nlet x: int = 1;\nfn int f(int x) { return x + limit; }\nprint(f(5));\nprint(x);' $'15\n1'
expect_program_output "block-local-vs-global" $'if (1) { let inner: int = 3; print(inner); }\nlet inner: int = 9;\nfn int f() { return inner; }\nprint(f());' $'3\n9'
expect_program_output "globals-named-like-runtime" $'let printf = 3;\nlet puts = 4;\nfn int f() { return printf + puts; }\nprint(f());' '7'
expect_program_output "struct-in-fn" $'struct P { int x = 4; }\nfn int f() { P p; return p.x; }\nprint(f());' '4'
expect_program_output "ar-and-rep-together" $'print(len("abc"));\nprint(replace("hello world", "world", "SC"));' $'3\nhello SC'
# Аудит, класс 3: % для дробных падал в codegen (assertion LLVM, код 134).
expect_program_output "float-remainder" $'f64 x = 5.5;\nprint(x % 2.0);\nprint(-5.5 % 2.0);\nf32 y = 7.5;\nprint(y % 2.0);\nprint(x % 2);\nf64 z = 9.0;\nz %= 4.0;\nprint(z);' $'1.500000\n-1.500000\n1.500000\n1.500000\n1.000000'
expect_compile_error "float-integer-division" 'f64 x = 7.5; print(x // 2.0);' "оператор // требует целочисленные операнды"
# Аудит, класс 6: литерал больше i32 без контекста — i64, а не ложная ошибка.
expect_program_output "i64-min-expression" $'print(-9223372036854775807 - 1);\ni64 m = -9223372036854775807 - 1;\nprint(m);\nprint(2147483648 + 0);' $'-9223372036854775808\n-9223372036854775808\n2147483648'
expect_compile_error "literal-beyond-i64" 'print(9223372036854775808);' "не помещается в тип i64"
expect_compile_error "i64-constant-overflow" 'print(9223372036854775807 + 1);' "переполнение"
# Аудит, класс 5: литерал в объявлении массива вне диапазона типа элемента молча обрезался.
expect_compile_error "array-literal-i32-range" 'int[] a = [1, 2147483648];' "элемент массива 2147483648 вне диапазона типа 'int/i32'"
expect_compile_error "array-literal-u8-range" 'u8[] b = [300];' "элемент массива 300 вне диапазона типа"
expect_compile_error "array-literal-u8-negative" 'u8[] b = [1, -1];' "элемент массива -1 вне диапазона типа"
expect_program_output "array-literal-edges" $'u8[] b = [0, 255];\nint[] a = [-2147483648, 2147483647];\nprint(b[1]);\nprint(a[0]);\nprint(a[1]);' $'255\n-2147483648\n2147483647'
# Аудит, класс 2: ветви тернарного оператора разной ширины давали неверный IR (код 3).
expect_program_output "ternary-mixed-int-width" $'short s = 1;\nprint(true ? 5 : s);\nprint(false ? 5 : s);\ni64 big = 5000000000;\nint k = 2;\nprint(k > 1 ? big : k);' $'5\n1\n5000000000'
expect_program_output "ternary-f32-f64" $'f32 s = 1.5;\nprint(true ? 5.0 : s);\nprint(false ? 5.0 : s);' $'5.000000\n1.500000'
expect_program_output "user-fn-named-strlen" $'fn int strlen(int x) { return x + 1; }\nprint(strlen(4));\nprint(len("abcd"));' $'5\n4'
expect_compile_error "user-main-reserved" $'fn void main() { print(111); }\nprint(1);' "имя функции 'main' зарезервировано"
expect_compile_error "return-dynamic-array" $'fn ptr make() { ptr v = new int[3]; return v; }\nprint(1);' "возврат динамического массива из функции пока не поддержан"

# Шаг 3: строки в куче, файлы, new, push, разыменование.
expect_program_output "str-huge-double" 'print(len(str(1e300)));' '308'
expect_program_output "str-returned-from-fn" $'fn str mk(int n) { return str(n); }\nstr a = mk(12);\nstr b = mk(345);\nprint(a);\nprint(b);' $'12\n345'
expect_program_output "str-locals-in-fn" $'fn str both(int a, int b) { str x = str(a); str y = str(b); return x; }\nprint(both(7, 8));\nprint(str(3.5));\nprint(str(-2147483647));' $'7\n3.500000\n-2147483647'
expect_program_output "file-roundtrip" "file f = open(\"$TMP_DIR/roundtrip.txt\", \"w\");
write(f, \"line1\");
close(f);
file g = open(\"$TMP_DIR/roundtrip.txt\", \"r\");
str t = read(g);
print(t);
close(g);" 'line1'
expect_program_output "new-has-length-n" $'ptr v = new int[3];\nv[0] = 10;\nprint(v[0]);\nprint(v.len());\nprint(v[2]);' $'10\n3\n0'
expect_program_output "new-empty-then-push" $'ptr v = new int[0];\nprint(v.len());\nv.push(5);\nprint(v[0]);' $'0\n5'
expect_program_output "deref-f64" $'f64 f = 2.5;\nptr p = &f;\nprint(*p);\n*p = 4.25;\nprint(f);' $'2.500000\n4.250000'
expect_program_output "deref-int" $'int x = 7;\nptr p = &x;\n*p = *p + 1;\nprint(x);\nprint(*p);' $'8\n8'
printf '%s\n' 'file f = open("no-such-dir/missing.txt", "r");' 'str t = read(f);' 'print(t);' > "$RUNTIME_DIR/missing_file.svx"
expect_runtime_error "open-missing-file" "$RUNTIME_DIR/missing_file.svx" "cannot open file 'no-such-dir/missing.txt' at line 1, column 10"
expect_compile_error "push-string-into-int-array" $'ptr v = new int[3];\nv.push("hello");' "элемент массива ожидает тип"

# Step 4c: lexical scope cleanup, LIFO, return and loop exits.
expect_program_output "defer-lifo-late-evaluation" "$(cat examples/defer.svx)" $'0\n2\n3'
expect_program_output "defer-branch-return" $'fn int choose(bool early) { defer print(10); if (early) { defer print(20); return 1; } else { defer print(30); } return 2; }\nprint(choose(true)); print(choose(false));' $'20\n10\n1\n30\n10\n2'
expect_program_output "defer-shadow-binding" $'fn void run() { int value = 1; defer print(value); if (true) { int value = 2; defer print(value); return; } }\nrun();' $'2\n1'
expect_program_output "range-variable-start" $'int s = 1;\nint e = 3;\nfor i in s..e { print(i); }\nfor j in s..=e { print(j); }\nint[] b = [2, 4];\nfor k in b[0]..b[1] { print(k); }' $'1\n2\n1\n2\n3\n2\n3'
expect_program_output "struct-nested-defaults" $'struct In { int x = 7; u8 y = 3; }\nstruct Out { In i; int z = 2; }\nOut o;\nprint(o.i.x);\nprint(o.i.y);\nprint(o.z);\nOut q = { i = { x = 1; }; };\nprint(q.i.x);\nprint(q.i.y);\nstruct A { int a = 4; }\nstruct B { A a; }\nstruct C { B b; }\nC c;\nprint(c.b.a.a);' $'7\n3\n2\n1\n3\n4'
expect_program_output "unsigned-argument-widening" $'fn void g(i64 x) { print(x); }\nu32 a = 4294967295;\ng(a);\nfn i64 h(i64 y) { return y; }\nprint(h(a));\nu8 b = 200;\nfn void k(i32 z) { print(z); }\nk(b);' $'4294967295\n4294967295\n200'
expect_program_output "repeat-count-widths" $'i64 n = 2;\nrepeat (n) { print(7); }\nu8 m = 200;\nint c = 0;\nrepeat (m) { c += 1; }\nprint(c);\nu64 big = 3;\nrepeat (big) { print(1); }\ni8 neg = -1;\nrepeat (neg) { print(0); }' $'7\n7\n200\n1\n1\n1'
expect_compile_error "read-not-file" $'int x = 1;\nprint(read(x));' "ожидался file, получен 'int/i32'"
expect_compile_error "read-string-not-file" $'string s = "a";\nstring t = read(s);' "ожидался file, получен 'str'"
expect_compile_error "array-assign-whole" $'int[] a = [1, 2];\nint[] b = [3, 4];\na = b;' "массив 'a' нельзя присвоить целиком"
expect_compile_error "array-assign-scalar" $'int[] a = [1, 2];\na = 5;' "массив 'a' нельзя присвоить целиком"
expect_compile_error "let-void-call" $'fn void f() { print(1); }\nlet x = f();' "функция без результата (void)"
expect_compile_error "int-void-call" $'fn void f() { print(1); }\nint y = f();' "функция без результата (void)"
expect_compile_error "print-void-call" $'fn void f() { print(1); }\nprint(f());' "print: функция без результата (void)"
expect_program_output "static-array-len-cap" $'int[] a = [1, 2, 3];\nprint(a.len());\nprint(a.cap());\nint[] e = [];\nprint(e.len());\nfor i in 0..a.len() { print(a[i]); }' $'3\n3\n0\n1\n2\n3'
expect_compile_error "static-array-pop-expression" $'int[] a = [1, 2];\nprint(a.pop());' "pop поддерживается только для динамических массивов"
expect_program_output "math-integer-arguments" $'print(pow(2, 10));\ni64 b = 3;\nprint(pow(b, 30));\nprint(sqrt(2));\nprint(floor(7));\nprint(ceil(7));\nprint(round(7));\nf64 r = sqrt(16);\nprint(r);\nprint(pow(2.0, 3));' $'1024\n205891132094649\n1.414214\n7.000000\n7.000000\n7.000000\n4.000000\n8.000000'
expect_compile_error "deref-assign-float-to-int" $'int x = 1;\nptr p = &x;\n*p = 3.5;' "'p' указывает на тип 'int/i32', записать значение типа 'double/f64' нельзя"
expect_compile_error "deref-assign-string-to-int" $'int x = 1;\nptr p = &x;\n*p = "hello";' "'p' указывает на тип 'int/i32'"
expect_compile_error "ptr-index-assign-float" $'int x = 1;\nptr p = &x;\np[0] = 2.5;' "'p' указывает на тип 'int/i32'"
expect_compile_error "ptr-retarget-other-type" $'int x = 1;\nf64 y = 2.0;\nptr p = &x;\np = &y;' "присвоить ему адрес значения типа 'double/f64' нельзя"
expect_compile_error "ptr-assign-other-pointee" $'int x = 1;\nf64 y = 2.0;\nptr p = &x;\nptr q = &y;\np = q;' "присвоить ему адрес значения типа 'double/f64' нельзя"
expect_program_output "ptr-typed-access" $'i64 x = 5000000000;\nptr p = &x;\nprint(p[0]);\np[0] = 6000000000;\nprint(x);\nint a = 1;\nint b = 2;\nptr q = &a;\nq = &b;\n*q = 7;\nprint(b);\nf64 d = 1.5;\nptr r = &d;\n*r = 2;\nprint(d);' $'5000000000\n6000000000\n7\n2.000000'
expect_compile_error "max-signed-unsigned" $'u32 a = 5;\nint b = -1;\nprint(max(a, b));' "max: нельзя смешивать signed- и unsigned-типы"
expect_compile_error "min-signed-unsigned" $'u8 a = 5;\ni64 b = -1;\nprint(min(b, a));' "min: нельзя смешивать signed- и unsigned-типы"
expect_program_output "max-unsigned-literal" $'u32 a = 5;\nprint(max(a, 3));\nprint(min(7, a));\nint b = -1;\nprint(max((int) a, b));' $'5\n5\n5'
expect_compile_error "abs-unsigned" $'u32 a = 4294967295;\nprint(abs(a));' "abs: значение беззнакового типа"
expect_program_output "abs-integers" $'int a = -5;\ni64 b = -5000000000;\nprint(abs(a));\nprint(abs(b));\nprint(abs(7));' $'5\n5000000000\n7'
expect_compile_error "print-static-array" $'int[] a = [1, 2];\nprint(a);' "print: массив напечатать нельзя"
expect_compile_error "print-dyn-array" $'ptr d = new int[2];\nprint(d);' "print: динамический массив напечатать нельзя"
expect_compile_error "print-struct" $'struct P { int a = 1; }\nP p;\nprint(p);' "напечатать нельзя: печатайте элементы или поля"
expect_compile_error "print-file" $'file f = open("print-file.txt", "w");\nprint(f);' "напечатать нельзя"
expect_compile_error "addr-static-array" $'int[] a = [1, 2];\nptr p = &a;' "адрес массива 'a' взять нельзя"
expect_compile_error "addr-dyn-array" $'ptr d = new int[2];\nptr p = &d;' "адрес массива 'd' взять нельзя"
expect_program_output "str-argument-types" $'print(str("abc"));\nu8 a = 200;\nprint(str(a));\ni8 b = -5;\nprint(str(b));\nu16 c = 60000;\nprint(str(c));\ni16 d = -300;\nprint(str(d));\nprint(str(3 > 5));' $'abc\n200\n-5\n60000\n-300\n0'
expect_compile_error "str-of-pointer" $'int x = 1;\nptr p = &x;\nprint(str(p));' "str: значение типа 'ptr' в строку не преобразуется"
expect_compile_error "str-of-struct" $'struct P { int a = 1; }\nP p;\nprint(str(p));' "в строку не преобразуется"
expect_compile_error "const-array-element" $'const a: int[] = [1, 2];\na[0] = 5;' "нельзя изменять константный массив 'a'"
expect_compile_error "const-dyn-array-push" $'const p: ptr = new int[1];\np.push(3);' "нельзя изменять константный массив 'p'"
expect_compile_error "const-dyn-array-pop" $'const p: ptr = new int[1];\nint x = p.pop();' "нельзя изменять константный массив 'p'"
expect_program_output "const-array-read" $'const a: int[] = [1, 2];\nprint(a[1]);\nprint(a.len());' $'2\n2'
expect_program_output "char-print-and-compare" $'char c = \'a\';\nprint(c == \'a\');\nprint(c != \'b\');\nprint(c < \'b\');\nprint(\'z\');\nprint((char) 66);\nint x = 1;\nprint(x > 0 ? \'y\' : \'n\');\nchar[] cs = [\'p\', \'q\'];\nprint(cs[1]);\nprint(c + 1);' $'1\n1\n1\nz\nB\ny\nq\n98'
expect_program_output "digit-separators" $'print(1_000);\nprint(1_000_000 + 1);\nprint(0xFF_FF);\nprint(0b1010_1010);\nprint(1_000.5);\nprint(1e1_0);' $'1000\n1000001\n65535\n170\n1000.500000\n10000000000.000000'
expect_compile_error "digit-separator-double" 'print(1__000);' "error"
expect_compile_error "array-literal-no-commas" $'int[] a = [1 2 3];\nprint(a[2]);' "ожидалась ',' между элементами массива"
expect_compile_error "array-literal-no-commas-let" $'let a: int[] = [1, 2 3];' "ожидалась ',' между элементами массива"
expect_program_output "array-literal-trailing-comma" $'int[] a = [1, 2, 3,];\nprint(a[2]);' '3'
expect_program_output "defer-return-in-loop" $'fn int run() { defer print(9); for i in 0..3 { defer print(i); if (i == 1) { return 7; } } return 0; }\nprint(run());' $'0\n1\n9\n7'
expect_program_output "defer-loop-forms" $'int n = 0; while (n < 2) { n = n + 1; defer print(n); }\nrepeat (2) { defer print(3); }\nwhile (n < 3) { n = n + 1; defer print(n); }\nfor i in 4..6 { defer print(i); }\nint[] values = [6, 7]; for item in values { defer print(item); }' $'1\n2\n3\n3\n3\n4\n5\n6\n7'
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
expect_program_output "drop-dyn-array" $'ptr v = new int[3];\nv[0] = 7;\nprint(v[0]);\ndrop v;\nprint(1);' $'7\n1'
expect_program_output "drop-file-flushes" "file f = open(\"$TMP_DIR/drop.txt\", \"w\");
write(f, \"dropped\");
drop f;
file g = open(\"$TMP_DIR/drop.txt\", \"r\");
print(read(g));
drop g;" 'dropped'
expect_program_output "drop-in-function-and-loop" $'fn int first() { ptr v = new int[2]; v[0] = 4; int r = v[0]; drop v; return r; }\nprint(first());\nfor i in 0..2 { ptr w = new int[1]; w[0] = i; print(w[0]); drop w; }' $'4\n0\n1'
expect_program_output "defer-drop" $'fn void run() { ptr v = new int[2]; defer drop v; defer print(v[1]); v[1] = 5; }\nrun();\nprint(6);' $'5\n6'
expect_program_output "drop-in-one-branch" $'ptr v = new int[2];\nif (true) { drop v; } else { print(v[0]); }\nprint(3);' '3'
expect_compile_error "drop-use-after" $'ptr v = new int[3];\ndrop v;\nprint(v[0]);' "'v' используется после drop (строка 2)"
expect_compile_error "drop-use-after-branch" $'ptr v = new int[3];\nif (true) { drop v; }\nprint(v.len());' "'v' используется после drop (строка 2)"
expect_compile_error "drop-file-close-after" "file f = open(\"$TMP_DIR/drop2.txt\", \"w\");
drop f;
close(f);" "'f' используется после drop (строка 2)"
expect_compile_error "drop-twice" $'ptr v = new int[3];\ndrop v;\ndrop v;' "повторный drop 'v' (уже освобождено в строке 2)"
expect_compile_error "drop-after-defer-drop" $'ptr v = new int[3];\ndefer drop v;\ndrop v;' "повторный drop 'v' (defer drop в строке 2)"
expect_compile_error "drop-string" $'str s = "a";\ndrop s;' 'drop поддерживается только для динамического массива и file'
expect_compile_error "drop-raw-pointer" $'int x = 1;\nptr p = &x;\ndrop p;' 'drop поддерживается только для динамического массива и file'
expect_compile_error "drop-outer-in-loop" $'ptr v = new int[3];\nwhile (true) { drop v; }' "drop 'v' внутри цикла или функции"
expect_compile_error "drop-global-in-function" $'ptr v = new int[3];\nfn void f() { drop v; }\nf();' "drop 'v' внутри цикла или функции"
expect_compile_error "drop-without-name" 'drop 5;' 'после drop ожидалось имя переменной'
# Аудит §2: ошибки лексера — обычная диагностика «файл:строка:столбец: error[lexer]» и код 1 (раньше std::exit).
expect_compile_error "lexer-unclosed-comment" '/* abc' ":1:1: error[lexer]: незакрытый блочный комментарий"
expect_compile_error "lexer-bad-number" 'int x = 0x;' ":1:9: error[lexer]: некорректный числовой литерал '0x'"
expect_compile_error "lexer-empty-char" "char c = '';" ":1:10: error[lexer]: ожидался один символ между одинарными кавычками"
expect_compile_error "lexer-unclosed-string" 'string s = "abc;' ":1:12: error[lexer]: строковый литерал не закрыт кавычкой"
expect_compile_error "lexer-unknown-char" 'int x = 1 @ 2;' ":1:11: error[lexer]: неизвестный символ: @"
# Константный индекс и сдвиг в 0x/0b-записи проверяются как десятичные (stoll читал `0x5` как 0).
expect_compile_error "hex-index-out-of-bounds" $'int[] a = [1, 2, 3];\nprint(a[0x5]);' "индекс массива выходит за границы [3]"
expect_compile_error "binary-index-store-out-of-bounds" $'int[] a = [1, 2, 3];\na[0b11] = 1;' "индекс массива выходит за границы [3]"
expect_compile_error "hex-shift-too-wide" 'int x = 1 << 0x40;' "счётчик сдвига должен быть меньше ширины левого операнда"
expect_program_output "hex-index-in-bounds" $'int[] a = [1, 2, 3];\nprint(a[0x2]);' '3'

# Аудит, класс 1: вложенность до 256 уровней работает, глубже — синтаксическая ошибка, а не segfault;
# длинные цепочки и else-if не упираются в стек (компиляция идёт в потоке с большим стеком).
nest_open=$(printf 'if (x == 1) {\n%.0s' $(seq 256)); nest_close=$(printf '}%.0s' $(seq 256))
expect_program_output "nesting-256-if" $'int x = 1;\n'"$nest_open"$'\nprint(x);\n'"$nest_close" '1'
expect_compile_error "nesting-257-if" $'int x = 1;\nif (x == 1) {\n'"$nest_open"$'\nprint(x);\n'"$nest_close"$'\n}' "слишком глубокая вложенность: больше 256 уровней"
parens_open=$(printf '(%.0s' $(seq 256)); parens_close=$(printf ')%.0s' $(seq 256))
expect_program_output "nesting-256-parens" "int x = ${parens_open}1${parens_close};"$'\nprint(x);' '1'
expect_compile_error "nesting-257-parens" "int x = (${parens_open}1${parens_close});" "слишком глубокая вложенность: больше 256 уровней"
expect_compile_error "nesting-100000-parens" "int x = $(printf '(%.0s' $(seq 100000))1;" "слишком глубокая вложенность"
expect_compile_error "nesting-300-not" "bool b = $(printf '!%.0s' $(seq 300))true;" "слишком глубокая вложенность"
# Фаззер: цепочка тернарных операторов вкладывается вправо — 256 уровней работают, 100000 — ошибка, а не segfault.
ternary_chain=$(printf '0 ? 1 : %.0s' $(seq 256))
expect_program_output "ternary-chain-256" "int x = ${ternary_chain}7;"$'\nprint(x);' '7'
expect_compile_error "ternary-chain-100000" "int x = $(printf '1 ? 1 : %.0s' $(seq 100000))2;" "слишком глубокая вложенность"

# Цепочка из 10000 операторов: линейное время (было O(n²): 10000 слагаемых ≈ 29 с), 10001 — ошибка, не segfault.
long_sum="1$(printf ' + 1%.0s' $(seq 10000))"
expect_program_output "sum-10000-operators" "int x = ${long_sum};"$'\nprint(x);' '10001'
# Время меряется только у ./sc: llc 23 сам по себе тратит на эти 130000 строк IR ~8 с.
printf 'int x = %s;\nprint(x);\n' "$long_sum" > "$TMP_DIR/sum-10000.svx"
start_ms=$(date +%s%N)
timeout --signal=TERM 60s ./sc "$TMP_DIR/sum-10000.svx" > /dev/null
compile_seconds=$(( ($(date +%s%N) - start_ms) / 1000000000 ))
if (( compile_seconds > 20 )); then
    echo "FAIL: 10000-operator sum took ${compile_seconds} s to compile (expected linear time)" >&2
    exit 1
fi
# Граница предела длины выражения: ровно 250000 операторов компилируется (без llc и запуска —
# под ASan это ~2 мин), 250001 — ошибка, а не переполнение стека. Ошибка на строке в 1 МБ
# печатается быстро (раньше отступ под `^` писался посимвольно: >10 с под ASan в CI).
limit_sum="1$(printf ' + 1%.0s' $(seq 250000))"
printf 'int x = %s;\nprint(x);\n' "$limit_sum" > "$TMP_DIR/sum-250000.svx"
if (ulimit -c 0; timeout --signal=TERM 600s ./sc "$TMP_DIR/sum-250000.svx" >"$TMP_DIR/sum-250000.ll" 2>"$TMP_DIR/sum-250000.err") \
        && grep -q 'define' "$TMP_DIR/sum-250000.ll"; then
    echo "PASS: program 'sum-250000-operators' compiled"
else
    echo "FAIL: 250000-operator sum did not compile" >&2
    head -5 "$TMP_DIR/sum-250000.err" >&2
    exit 1
fi
# 250001 — та же работа парсера, что и у 250000 (под ASan в CI дольше 10 с), поэтому
# свой таймаут, как у теста выше: код выхода 1 и текст ошибки, не падение.
printf 'int x = %s + 1;\n' "$limit_sum" > "$TMP_DIR/sum-250001.svx"
limit_status=0
(ulimit -c 0; timeout --signal=TERM 600s ./sc "$TMP_DIR/sum-250001.svx" >/dev/null 2>"$TMP_DIR/sum-250001.err") || limit_status=$?
if [[ $limit_status -eq 1 ]] && grep -q "слишком длинное выражение: больше 250000 операторов в одной инструкции" "$TMP_DIR/sum-250001.err"; then
    echo "PASS: invalid input 'sum-250001-operators' reported an error without crashing"
else
    echo "FAIL: 250001-operator sum: status $limit_status" >&2
    head -c 500 "$TMP_DIR/sum-250001.err" >&2
    exit 1
fi
expect_program_output "sum-two-long-statements" "int x = ${long_sum};"$'\n'"int y = ${long_sum};"$'\nprint(x + y);' '20002'
expect_program_output "sum-2000-terms" "int x = 1$(printf ' + 1%.0s' $(seq 1999));"$'\nprint(x);' '2000'
elif_chain=$(for i in $(seq 0 999); do printf ' else if (x == %d) { print(%d); }' "$i" "$i"; done)
expect_program_output "else-if-chain-1000" $'int x = 999;\nif (x < 0) { print(-1); }'"$elif_chain" '999'
# Аудит, находка 10: 20000 звеньев давали segfault (ASan/UBSan — уже 10000); предел 2500.
elif_chain=$(for i in $(seq 0 2499); do printf ' else if (x == %d) { print(%d); }' "$i" "$i"; done)
expect_program_output "else-if-chain-2500" $'int x = 2499;\nif (x < 0) { print(-1); }'"$elif_chain" '2499'
elif_chain=$(for i in $(seq 0 19999); do printf ' else if (x == %d) { print(%d); }' "$i" "$i"; done)
expect_compile_error "else-if-chain-20000" $'int x = 1;\nif (x < 0) { print(-1); }'"$elif_chain" "слишком длинная цепочка else if: больше 2500 звеньев"

# D-007 / аудит 07f: вернуть адрес локальной переменной или параметра — ошибка компиляции.
expect_compile_error "return-addr-local" 'fn ptr g() { int x = 5; return &x; }' "возврат адреса локальной переменной 'x'"
expect_compile_error "return-addr-param" 'fn ptr g(int a) { return &a; }' "возврат адреса локальной переменной 'a'"
expect_compile_error "return-ptr-to-local" 'fn ptr g() { int x = 5; ptr p = &x; return p; }' "возврат 'p' — адреса локальной переменной"
expect_program_output "return-addr-global" $'int G = 3;\nfn ptr g() { return &G; }\nptr p = g();\nprint(*p);' '3'
expect_program_output "return-ptr-param" $'fn ptr g(ptr q) { return q; }\nint y = 4;\nptr p = g(&y);\nprint(*p);' '4'
# D-007 / аудит 07c: close(f) освобождает файл так же, как drop f.
expect_compile_error "close-then-write" $'file f = open("c.txt", "w");\nclose(f);\nwrite(f, "x");' "'f' используется после close (строка 2)"
expect_compile_error "close-then-read" $'file f = open("c.txt", "r");\nclose(f);\nprint(read(f));' "'f' используется после close (строка 2)"
expect_compile_error "close-twice" $'file f = open("c.txt", "w");\nclose(f);\nclose(f);' "'f' используется после close (строка 2)"
expect_compile_error "drop-after-close" $'file f = open("c.txt", "w");\nclose(f);\ndrop f;' "повторный drop 'f' (уже освобождено в строке 2)"
expect_compile_error "close-after-branch" $'file f = open("c.txt", "w");\nif (true) { close(f); }\nwrite(f, "x");' "'f' используется после close (строка 2)"
expect_compile_error "close-outer-in-loop" $'file f = open("c.txt", "w");\nwhile (true) { close(f); }' "close 'f' внутри цикла или функции"
close_file="$TMP_DIR/close-ok.tmp"
expect_program_output "close-in-loop-own-file" "int i = 0;"$'\nwhile (i < 2) {\n'"    file f = open(\"$close_file\", \"w\");"$'\n    write(f, "x");\n    close(f);\n    i = i + 1;\n}\nprint(i);' '2'
expect_program_output "defer-drop-after-return-value" $'fn int f(int x) {\n  ptr v = new int[2];\n  defer drop v;\n  v[0] = x;\n  return v[0] + 1;\n}\nprint(f(4));' '5'
expect_program_output "defer-after-return-call" $'fn int g() { print(1); return 3; }\nfn int f() { defer print(2); return g(); }\nprint(f());' $'1\n2\n3'
expect_program_output "defer-assign-after-return-value" $'fn int f() { ptr v = new int[1]; v[0] = 4; defer v[0] = 100; return v[0]; }\nprint(f());' '4'
expect_program_output "defer-struct-param-return" $'struct P { int x = 0; }\nfn int f(P value) { defer value.x = 100; return value.x; }\nP p = { x = 3; };\nprint(f(p));\nprint(p.x);' $'3\n3'

# Целые (шаг 5а): деление на ноль, сдвиги, переполнение — ошибка, а не молчаливая порча значения.
int_runtime() { printf '%s' "$2" > "$RUNTIME_DIR/int_$1.svx"; expect_runtime_error "int-$1" "$RUNTIME_DIR/int_$1.svx" "$3"; }
int_runtime div-zero $'int a = 5;\nint b = 0;\nprint(a / b);' "division by zero at line 3, column 9"
int_runtime rem-zero $'int a = 5;\nint b = 0;\nprint(a % b);' "division by zero at line 3, column 9"
int_runtime min-div-minus-one $'int a = -2147483647 - 1;\nint b = -1;\nprint(a / b);' "integer overflow at line 3, column 9"
int_runtime shift-range $'int a = 1;\nint s = 40;\nprint(a << s);' "shift amount out of range at line 3, column 9"
int_runtime pow-overflow $'int a = 2;\nprint(pow(a, 31));' "integer overflow at line 2"
int_runtime abs-min $'int m = -2147483647 - 1;\nprint(abs(m));' "integer overflow at line 2"
int_runtime abs-min-i64 $'i64 m = -9223372036854775807 - 1;\nprint(abs(m));' "integer overflow at line 2"
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

# Шаг 5в: null, вложенные и составные присваивания, мелочи раздела D.
expect_program_output "null-pointer-ok" $'ptr p = null;\nif (p == null) { print(1); }\nint x = 5;\np = &x;\nif (p != null) { print(*p); }\nfn ptr none() { return null; }\nfn void check(ptr q) { if (q == null) { print(2); } }\ncheck(none());' $'1\n5\n2'
null_runtime() { printf '%s' "$2" > "$RUNTIME_DIR/null_$1.svx"; expect_runtime_error "null-$1" "$RUNTIME_DIR/null_$1.svx" "$3"; }
null_runtime deref-read $'ptr p = null;\nprint(*p);' "null pointer dereference at line 2, column 7"
null_runtime deref-write $'ptr p = null;\n*p = 3;' "null pointer dereference at line 2, column 1"
expect_compile_error "null-into-int" 'int a = null;' "null и указатели допустимы только для типа ptr"
expect_compile_error "null-compare-int" $'int x = 1;\nif (x == null) { print(2); }' "указатели можно сравнивать только между собой и с null"
expect_compile_error "null-ordering" $'ptr p = null;\nif (p < null) { print(1); }' "указатели можно сравнивать только между собой и с null"
expect_program_output "nested-field-assign" $'struct In { int x = 0; }\nstruct Out { In a; int y = 0; }\nOut r = { };\nr.a.x = 5;\nr.a.x += 2;\nr.y = 1;\nprint(r.a.x);\nprint(r.y);' $'7\n1'
expect_program_output "compound-field-and-element" $'struct P { int x = 1; }\nP a = { x = 1; };\na.x += 2;\na.x *= 5;\na.x -= 3;\na.x /= 2;\na.x %= 4;\nprint(a.x);\nint[] s = [1, 2];\ns[0] += 5;\ns[1] *= 10;\nprint(s[0]);\nprint(s[1]);' $'2\n6\n20'
expect_program_output "compound-index-evaluated-once" $'fn int k() { print(9); return 1; }\nptr v = new int[2];\nv[k()] += 4;\nv[k()] -= 1;\nprint(v[1]);' $'9\n9\n3'
compound_runtime() { printf '%s' "$2" > "$RUNTIME_DIR/compound_$1.svx"; expect_runtime_error "compound-$1" "$RUNTIME_DIR/compound_$1.svx" "$3"; }
compound_runtime element-overflow $'int[] a = [2147483647, 0];\na[0] += 1;' "integer overflow at line 2, column 6"
compound_runtime field-division-by-zero $'struct P { int x = 0; }\nP a = { x = 1; };\nint z = 0;\na.x %= z;' "division by zero at line 4, column 5"
expect_compile_error "compound-field-const-zero" $'struct P { int x = 0; }\nP a = { x = 1; };\na.x /= 0;' 'деление на ноль'
expect_compile_error "nested-field-unknown" $'struct In { int x = 0; }\nstruct Out { In a; }\nOut r = { };\nr.a.y = 5;' "у структуры 'In' нет поля 'y'"
expect_compile_error "nested-field-type" $'struct In { int x = 0; }\nstruct Out { In a; }\nOut r = { };\nr.a.x = 1.5;' "поле ожидает тип 'int/i32', получен 'double/f64'"
expect_compile_error "call-args-need-comma" 'print(max(1 2));' "ожидалась ',' между аргументами"
expect_program_output "loop-without-break-returns" $'fn int f() { loop { return 1; } }\nprint(f());' '1'
expect_compile_error "loop-with-break-not-returning" 'fn int f(int n) { loop { if (n > 3) { break; } return 1; } }' 'функция должна возвращать значение на всех путях выполнения'
expect_program_output "char-arithmetic" $'char c = \'a\';\nprint(c + 1);\nint d = c - 32;\nprint(d);' $'98\n65'
expect_compile_error "char-plus-float" $'char c = \'a\';\nprint(c + 1.5);' "для char требует целый операнд"

# Шаг 5г: составное присваивание в узкий тип проверяет диапазон; чтение p[i] через null.
narrow_runtime() { printf '%s' "$2" > "$RUNTIME_DIR/narrow_$1.svx"; expect_runtime_error "narrow-$1" "$RUNTIME_DIR/narrow_$1.svx" "$3"; }
narrow_runtime decl-i8 $'i8 a = 100;\ni8 c = a + a;\nprint(c);' "integer overflow at line 2, column 1"
narrow_runtime decl-i16 $'i16 a = 30000;\ni16 b = a + a;\nprint(b);' "integer overflow at line 2, column 1"
narrow_runtime let-u8 $'u8 a = 200;\nlet x: u8 = a + a;\nprint(x);' "integer overflow at line 2, column 1"
narrow_runtime return-i8 $'fn i8 f(i8 a) { return a + a; }\nprint(f(100));' "integer overflow at line 1, column 17"
narrow_runtime argument-i8 $'fn void g(i8 x) { print(x); }\ni8 a = 100;\ng(a + a);' "integer overflow at line 3, column 5"
narrow_runtime array-literal-u8 $'u8 a = 200;\nu8[] arr = [a + a];\nprint(arr[0]);' "integer overflow at line 2, column 15"
narrow_runtime push-u8 $'u8 a = 200;\nptr v = new u8[0];\nv.push(a + a);' "integer overflow at line 3, column 10"
narrow_runtime struct-literal-u8 $'struct S { u8 v = 0; }\nu8 a = 200;\nS s = { v = a + a; };\nprint(s.v);' "integer overflow at line 3, column 15"
expect_program_output "narrow-store-in-range" $'i8 a = 50;\ni8 c = a + a;\nfn i8 f(i8 x) { return x + x; }\nfn void g(u8 y) { print(y); }\nu8 b = 100;\ng(b + b);\nu8[] arr = [b + b];\nprint(c);\nprint(f(60));\nprint(arr[0]);' $'200\n100\n120\n200'
narrow_runtime u8-plus $'u8 x = 255;\nx += 1;\nprint(x);' "integer overflow at line 2, column 1"
narrow_runtime i8-increment $'i8 x = 127;\nx++;\nprint(x);' "integer overflow at line 2, column 1"
narrow_runtime i8-decrement $'i8 x = -128;\nx--;\nprint(x);' "integer overflow at line 2, column 1"
narrow_runtime i16-times $'i16 x = 30000;\nx *= 2;\nprint(x);' "integer overflow at line 2, column 1"
narrow_runtime u16-minus $'u16 x = 5;\nx -= 6;\nprint(x);' "integer overflow at line 2, column 3"
narrow_runtime u8-field $'struct P { u8 x = 0; }\nP a = { x = 255; };\na.x += 1;\nprint(a.x);' "integer overflow at line 3, column 5"
narrow_runtime u8-element $'u8[] a = [255, 1];\na[0] += 1;\nprint(a[0]);' "integer overflow at line 2, column 1"
narrow_runtime i8-element-var $'i8[] a = [100, 1];\nint k = 100;\na[0] += k;\nprint(a[0]);' "integer overflow at line 3, column 1"
narrow_runtime u8-sum $'u8 x = 200;\nu8 y = 100;\nx = x + y;\nprint(x);' "integer overflow at line 3, column 1"
narrow_runtime null-index-read $'ptr p = null;\nprint(p[0]);' "null pointer dereference at line 2, column 7"
expect_program_output "narrow-compound-in-range" $'u8 x = 200;\nx /= 2;\nx %= 7;\nprint(x);\ni8 y = 5;\nint k = 3;\ny += k;\nprint(y);\nstruct P { u8 v = 0; }\nP a = { v = 200; };\na.v += 55;\nprint(a.v);\nu8[] b = [250, 1];\nb[0] += 5;\nprint(b[0]);\nu8 z = 254;\nz++;\nprint(z);' $'2\n8\n255\n255\n255'
expect_compile_error "narrow-compound-constant" $'u8 x = 10;\nx += 300;' "нельзя присвоить 'int/i32' значению типа 'byte/u8'"

# Шаг 6а: одно слово на одно действие — старые синонимы дают ошибку с подсказкой.
expect_compile_error "removed-word-fucn" "fucn void f() { }" "слово 'fucn' удалено: пишите 'fn'"
expect_compile_error "removed-word-rev" "fn int f() { rev 1; }" "слово 'rev' удалено: пишите 'return'"
expect_compile_error "removed-word-slov" "slov(1);" "слово 'slov' удалено: пишите 'print'"
expect_compile_error "removed-word-eles" "eles (true) { print(1); }" "слово 'eles' удалено: пишите 'if'"
expect_compile_error "removed-word-sele" "if (true) { print(1); } sele { print(2); }" "слово 'sele' удалено: пишите 'else'"
expect_compile_error "removed-word-brk" "loop { brk; }" "слово 'brk' удалено: пишите 'break'"
expect_compile_error "removed-word-cont" "for i in 0..2 { cont; }" "слово 'cont' удалено: пишите 'continue'"
expect_compile_error "removed-word-rox" "rox x = 1;" "слово 'rox' удалено: пишите 'int'"
expect_compile_error "removed-word-rox64" "rox64 x = 1;" "слово 'rox64' удалено: пишите 'long'"
expect_compile_error "removed-word-dor" "dor x = 1.5;" "слово 'dor' удалено: пишите 'float'"
expect_compile_error "removed-word-cos" "cos s = \"a\";" "слово 'cos' удалено: пишите 'string'"
expect_compile_error "removed-word-bue" "bue b = true;" "слово 'bue' удалено: пишите 'bool'"
expect_compile_error "removed-word-chr" "chr c = 'a';" "слово 'chr' удалено: пишите 'char'"
expect_compile_error "removed-word-cop" "fn int f() { return 1; } print(cop f());" "слово 'cop' удалено"
expect_compile_error "removed-form-cert-usy" "int n = 0; cert usy (n < 2) { n = n + 1; }" "форма 'cert usy (...)' удалена: пишите 'while (...)'"
expect_program_output "removed-word-in-string-ok" 'print("fucn slov rox");' 'fucn slov rox'
expect_program_output "repeat-loop" 'repeat (2) { print(1); }' $'1\n1'
# Шаг 6в: понятные английские слова; старые — ошибка с подсказкой.
expect_compile_error "removed-word-tip" "tip P { int x = 0; }" "слово 'tip' удалено: пишите 'struct'"
expect_compile_error "removed-word-nw" "ptr v = nw int[2];" "слово 'nw' удалено: пишите 'new'"
expect_compile_error "removed-word-atk" "file f = atk(\"x.txt\", \"w\");" "слово 'atk' удалено: пишите 'open'"
expect_compile_error "removed-word-zap" "zap(f, \"a\");" "слово 'zap' удалено: пишите 'write'"
expect_compile_error "removed-word-pro" "print(pro(f));" "слово 'pro' удалено: пишите 'read'"
expect_compile_error "removed-word-zak" "zak(f);" "слово 'zak' удалено: пишите 'close'"
expect_compile_error "removed-word-vd" "int n = vd(\"\");" "слово 'vd' удалено: пишите 'input_int'"
expect_compile_error "removed-word-vv_s" "string s = vv_s(\"\");" "слово 'vv_s' удалено: пишите 'input_str'"
expect_compile_error "removed-word-flo" "print(flo(1.5));" "слово 'flo' удалено: пишите 'floor'"
expect_compile_error "removed-word-cel" "print(cel(1.5));" "слово 'cel' удалено: пишите 'ceil'"
expect_compile_error "removed-word-ron" "print(ron(1.5));" "слово 'ron' удалено: пишите 'round'"
expect_compile_error "removed-word-ran" "print(ran());" "слово 'ran' удалено: пишите 'rand'"
expect_compile_error "removed-word-ar" "print(ar(\"ab\"));" "слово 'ar' удалено: пишите 'len'"
expect_compile_error "removed-word-rep" "print(rep(\"a\", \"a\", \"b\"));" "слово 'rep' удалено: пишите 'replace'"
expect_compile_error "removed-form-svi-lor" "svi int lor x = 5;" "форма 'svi T lor x' удалена: пишите 'T x = …;'"
expect_compile_error "removed-form-cert-ceh" "cert ceh (2) { print(1); }" "форма 'cert ceh (n)' удалена: пишите 'repeat (n)'"
expect_compile_error "removed-word-ser" 'print(ser("a", "b"));' "слово 'ser' удалено: строки сравниваются через 'a == b'"
expect_compile_error "removed-word-imp" 'imp "x.svx";' "слово 'imp' удалено: пишите 'import'"
expect_program_output "struct-variable-forms" $'struct P { int x = 1; int y = 2; }\nfn int sum(P p) { return p.x + p.y; }\nP a;\nP b = { x = 5; y = 6; };\nprint(sum(a));\nprint(sum(b));' $'3\n11'
expect_program_output "string-equality" $'string a = "abc";\nstring b = "abc";\nstring c = "abd";\nprint(a == b);\nprint(a != c);\nif (a == "abc") { print(7); }' $'1\n1\n7'
expect_compile_error "string-ordering" $'string a = "a";\nprint(a < "b");' "строки сравниваются только через == и !="
expect_program_output "new-words-builtins" $'print(floor(2.7));\nprint(ceil(2.1));\nprint(round(2.5));\nprint(len("abcd"));\nptr r = replace("a-b", "-", "+");\nprint(r);\nint n = rand();\nprint(n >= 0);' $'2.000000\n3.000000\n3.000000\n4\na+b\n1'
expect_compile_error "dyn-array-argument" $'fn int g(ptr p) { return 1; }\nptr v = new int[2];\nprint(g(v));' "динамический массив 'v' пока нельзя передать в функцию"
expect_program_output "print-bool-builtin" $'string s = "a";\nprint((s == "a"));\nprint((s == "b"));' $'1\n0'

# Step 4b: PR22 struct parameters are aggregate copies, not aliases.
expect_program_output "struct-by-value-copy" "$(cat examples/struct_by_value.svx)" $'17\n3\n4\n3'
expect_program_output "struct-by-value-nested" $'struct Inner { int x = 2; }\nstruct Outer { Inner inner = { x = 7; }; int y = 3; }\nfn int change(Outer value) { Inner replacement = { x = 9; }; value.inner = replacement; return value.inner.x + value.y; }\nOuter original = { inner = { x = 5; }; y = 4; };\nprint(change(original)); print(original.inner.x);' $'13\n5'
expect_program_output "struct-by-value-forward" $'fn int total(int offset, Pair value);\nstruct Pair { int x = 0; }\nfn int total(int offset, Pair value) { return offset + value.x; }\nPair value = { x = 5; }; print(total(10, value));' '15'
expect_program_output "struct-by-value-layout" $'struct Sample { u8 tag = 1; f64 weight = 2.5; }\nfn f64 weight(Sample value) { value.weight = 4.25; return value.weight; }\nSample value; print(weight(value)); print(value.weight); print(value.tag);' $'4.250000\n2.500000\n1'
expect_compile_error "struct-param-unknown-type" 'fn int read(Missing value) { return 0; }' "неизвестный тип параметра 'Missing'"
expect_compile_error "struct-param-wrong-type" $'struct Pair { int x = 0; }\nfn int take(Pair value) { return value.x; }\nprint(take(1));' "аргумент 1 функции 'take': ожидался 'Pair'"
expect_compile_error "struct-param-missing-field" $'struct Pair { int x = 0; }\nfn int read(Pair value) { return value.missing; }' "у структуры 'Pair' нет поля 'missing'"
expect_compile_error "struct-param-forward-mismatch" $'struct A { int x = 0; } struct B { int x = 0; }\nfn int read(A value); fn int read(B value) { return value.x; }' "сигнатура функции 'read' не совпадает"
expect_compile_error "struct-field-unknown" 'struct Outer { Missing inner; }' "неизвестный тип поля 'Missing'"
expect_compile_error "struct-field-recursive" 'struct Recursive { Recursive next; }' 'рекурсивное определение структуры'

# pop() expression: both PHI incoming blocks must be actual CFG predecessors.
expect_program_output "pop-expression-after-growth" 'ptr v = new int[0]; v.push(1); print(v.pop()); print(v.len()); del v;' $'1\n0'
expect_program_output "pop-expression-empty" 'ptr v = new int[0]; print(v.pop()); print(v.pop()); print(v.len()); del v;' $'0\n0\n0'
expect_program_output "pop-expression-lifo-and-control-flow" $'ptr v = new int[0];\nv.push(10); v.push(20); v.push(30); v.push(40); v.push(50);\nprint(v.pop()); print(v.len()); print(v.pop() + v.pop());\nif (v.len() == 2) { print(v.pop()); } else { print(-1); }\nprint(v.pop()); print(v.pop()); print(v.len()); del v;' $'50\n4\n70\n20\n10\n0\n0'
expect_program_output "pop-expression-float-zero" 'ptr v = new f64[0]; print(v.pop()); v.push(2.5); print(v.pop()); print(v.pop()); del v;' $'0.000000\n2.500000\n0.000000'

# Объявление функции без тела (fn T name(...);) и взаимная рекурсия.
expect_program_output "forward-decl-call-before-def" $'fn int triple(int x);\nprint(triple(4));\nfn int triple(int x) { return x * 3; }' '12'
expect_program_output "forward-decl-mutual-recursion" $'fn bool ev(int n);\nfn bool od(int n) { if (n == 0) { return false; } return ev(n - 1); }\nfn bool ev(int n) { if (n == 0) { return true; } return od(n - 1); }\nprint(ev(10));\nprint(od(7));' $'1\n1'
expect_compile_error "forward-decl-without-definition" $'fn int g(int x);\nprint(1);' "функция 'g' объявлена, но не определена"
expect_compile_error "forward-decl-param-mismatch" $'fn int g(int x);\nfn int g(f64 x) { return 1; }\nprint(g(1.0));' "сигнатура функции 'g' не совпадает"
expect_compile_error "forward-decl-return-mismatch" $'fn f64 g(int x);\nfn int g(int x) { return 1; }\nprint(g(1));' "сигнатура функции 'g' не совпадает"
expect_compile_error "duplicate-function-definition" $'fn int g(int x) { return 1; }\nfn int g(int x) { return 2; }\nprint(g(1));' "повторное определение функции 'g'"
expect_compile_error "duplicate-forward-decl" $'fn int g(int x);\nfn int g(int x);\nfn int g(int x) { return 1; }\nprint(g(1));' "повторное объявление функции 'g'"

expect_compile_error "array-negative-index" "int[] values = [1, 2]; print(values[-1]);" "индекс массива выходит за границы"
expect_compile_error "array-upper-index" "int[] values = [1, 2]; print(values[2]);" "индекс массива выходит за границы"

expect_compile_error "missing-expression" 'let value: int = ;' "ожидалось выражение"
# exact-array-not-yet-supported is now supported (i16, u16, f32, u8, i8, u32, u64, i64)
# Keep a test for an array type that should still be rejected
expect_compile_error "array-unsupported-element" 'ptr[] values = [1, 2];'
expect_compile_error "unsigned-mixed-sign" 'u32 positive = 1; i32 signed = -1; let result: int = positive + signed;'
expect_compile_error "unsigned-width-mix" 'u64 large = 2; i64 signed = -1; let result: i64 = large + signed;'
expect_compile_error "mixed-signedness-comparison" 'u32 unsigned_value = 1; i32 signed_value = 1; if (unsigned_value > signed_value) { print(1); }'
expect_compile_error "array-index-type" 'int[] values = [1, 2]; float index = 0.5; values[index] = 9;'
expect_compile_error "unknown-type" 'mystery value = 1;'
expect_compile_error "duplicate-variable" 'let result: int = 1; let result: int = 2;'
expect_compile_error "break-outside-loop" 'break;'
expect_compile_error "builtin-function-collision" 'fn int max(int left, int right) { return left + right; }'
# Ошибка в инициализаторе поля указывает на само поле, а не на 1:1.
expect_compile_error "struct-field-init-position" $'struct P { int a = 0; }\n\nP p = { a = "x"; };' ":3:9: error[semantic]: поле 'a' ожидает тип"
expect_compile_error "nested-function" 'if (1) { fn int inner(int value) { return value; } }'
expect_compile_error "unreachable-after-return" 'fn void dead() { return; print(2); }'
expect_compile_error "unreachable-after-break" 'fn void dead() { repeat (2) { break; print(3); } }'
expect_compile_error "unreachable-after-continue" 'fn void dead() { repeat (2) { continue; print(4); } }'
expect_compile_error "continue-in-counted-loop" 'let total: int = 0; repeat (5) { continue; total = total + 1; } print(total);'
expect_compile_error "string-arithmetic" 'let bad: int = "x" + "y";'
expect_compile_error "int-overflow-negative" 'int overflow = -2147483649;'
expect_compile_error "array-index-float" 'int[] values = [1, 2]; float index = 0.5; print(values[index]);'
expect_compile_error "builtin-no-args" 'sqrt();'
expect_compile_error "type-mismatch-initializer" 'int count = "wrong";'
expect_compile_error "recursive-struct-definition" 'struct BadRecursive { int value; BadRecursive next; }' "рекурсивное определение структуры"
expect_compile_error "struct-copy-ownership" 'struct Container { int id; ptr data; } Container a; a.id = 1; a.data = null; Container b; b = a;' "неявное копирование структуры"
expect_compile_error "struct-ptr-field-access" 'struct Node { int value; ptr next; } Node n1; Node n2; n1.value = 10; n1.next = &n2; n2.value = 20; n2.next = null; print(n1.next.value);' "не имеет полей"

IMPORT_DIR="$TMP_DIR/imports"
mkdir -p "$IMPORT_DIR/lib"
cat > "$IMPORT_DIR/lib/base.svx" <<'SC'
fn base() {
    return 21;
}
SC
cat > "$IMPORT_DIR/lib/math.svx" <<'SC'
import "base.svx";
fn twice(int value) {
    return value * 2;
}
SC
cat > "$IMPORT_DIR/main.svx" <<'SC'
import "lib/math.svx";
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

expect_import_error "missing-module" "не удалось открыть модуль" 'import "missing.svx";'
# Аудит §2: ошибки импорта — в общем формате «файл:строка:столбец: error[import]» у строки import.
expect_import_error "missing-module-position" ":2:1: error[import]: не удалось открыть модуль" $'print(1);\nimport "missing.svx";'
expect_import_error "import-without-path" ":1:1: error[import]: после 'import' ожидался путь в кавычках" 'import 5;'
missing_root="$TMP_DIR/no-such-dir/main.svx"
if (timeout --signal=TERM 10s "$SC_BIN" "$missing_root" >/dev/null 2>"$TMP_DIR/missing-root.err"); then
    echo "FAIL: missing source file compiled" >&2; exit 1
fi
if ! grep -Fq "$missing_root: error[import]: не удалось открыть модуль" "$TMP_DIR/missing-root.err"; then
    echo "FAIL: missing source file diagnostic has wrong format" >&2; cat "$TMP_DIR/missing-root.err" >&2; exit 1
fi
echo "PASS: missing source file reported as 'file: error[import]'"
# Аудит, класс 4: каталог или устройство вместо файла давали abort (код 134).
expect_import_error "empty-path" "это не обычный файл" 'import "";'
expect_import_error "directory" "это не обычный файл" 'import ".";'
expect_import_error "device" "это не обычный файл" 'import "/dev/zero";'
mkdir -p "$TMP_DIR/import-cycle"
printf '%s\n' 'import "b.svx";' > "$TMP_DIR/import-cycle/a.svx"
printf '%s\n' 'import "a.svx";' > "$TMP_DIR/import-cycle/b.svx"
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

# Аудит 07a/07b: размер new T[n] / resize(n) — отрицательный или огромный даёт SC runtime error, а не abort/segfault.
printf '%s\n' 'int n = -1;' 'ptr v = new int[n];' 'print(v.len());' > "$RUNTIME_DIR/new_negative.svx"
printf '%s\n' 'ptr v = new int[2];' 'v.resize(-1);' 'print(v.len());' > "$RUNTIME_DIR/resize_negative.svx"
printf '%s\n' 'i64 n = 4611686018427387904;' 'ptr v = new int[n];' 'print(v.len());' > "$RUNTIME_DIR/new_huge.svx"
printf '%s\n' 'i64 n = 4611686018427387904;' 'ptr v = new int[2];' 'v.resize(n);' 'print(v.len());' > "$RUNTIME_DIR/resize_huge.svx"
printf '%s\n' 'u64 n = 18446744073709551615;' 'ptr v = new int[n];' 'print(v.len());' > "$RUNTIME_DIR/new_u64_max.svx"
expect_runtime_error "new-negative-size" "$RUNTIME_DIR/new_negative.svx" "SC runtime error: negative array size at line 2, column 9"
expect_runtime_error "resize-negative" "$RUNTIME_DIR/resize_negative.svx" "SC runtime error: negative array size at line 2, column 1"
expect_runtime_error "new-huge-size" "$RUNTIME_DIR/new_huge.svx" "SC runtime error: out of memory at line 2, column 9"
expect_runtime_error "resize-huge" "$RUNTIME_DIR/resize_huge.svx" "SC runtime error: out of memory at line 3, column 1"
expect_runtime_error "new-u64-max" "$RUNTIME_DIR/new_u64_max.svx" "SC runtime error: array size too large at line 2, column 9"
expect_program_output "resize-zero-fill" $'ptr v = new int[2];\nv[0] = 5;\nv[1] = 7;\nv.resize(1);\nv.resize(3);\nprint(v[0]);\nprint(v[1]);\nprint(v[2]);\nprint(v.len());' $'5\n0\n0\n3'
expect_program_output "new-zero-then-push" $'ptr v = new int[0];\nv.push(3);\nv.push(4);\nv.push(5);\nprint(v[2]);\nprint(v.len());' $'5\n3'

# Ввод читает строку целиком (D-007, аудит 07d/07e): без обрезки до 255 байт и не по словам.
expect_program_output "input-prompt-variable" $'string p = "Age: ";\nint n = input_int(p);\nstring q = "Name %d%s: ";\nstring s = input_str(q);\nprint(n);\nprint(s);' 'Age: Name %d%s: 7'$'\nBob' $'7\nBob\n'
expect_compile_error "input-prompt-not-string" $'int n = input_int(5);' "подсказка ввода должна иметь тип str"
expect_program_output "input-line-whole" $'int n = input_int("");\nstring s = input_str("");\nprint(n * 2);\nprint(s);\nprint(len(s));' $'42\nhello world\n11' $'  21  \nhello world\n'
expect_program_output "input-line-crlf" $'string s = input_str("");\nprint(len(s));' '2' $'ab\r\n'
expect_program_output "input-line-long" $'string s = input_str("");\nprint(len(s));' '100000' "$(printf 'x%.0s' $(seq 100000))"
expect_program_output "input-str-eof-empty" $'string s = input_str("");\nprint(len(s));' '0' ''
printf '%s\n' 'int n = input_int("");' 'print(n);' > "$RUNTIME_DIR/input_int.svx"
expect_runtime_error "input-int-garbage" "$RUNTIME_DIR/input_int.svx" "SC runtime error: invalid integer input at line 1, column 9" $'abc\n'
expect_runtime_error "input-int-trailing" "$RUNTIME_DIR/input_int.svx" "invalid integer input" $'12abc\n'
expect_runtime_error "input-int-overflow" "$RUNTIME_DIR/input_int.svx" "integer input out of range" $'99999999999999\n'
expect_runtime_error "input-int-eof" "$RUNTIME_DIR/input_int.svx" "unexpected end of input" ''
lines_file="$TMP_DIR/input-lines.tmp"
expect_program_output "file-read-lines" "file o = open(\"$lines_file\", \"w\");"$'\nwrite(o, "line one\\nline two");\nclose(o);\n'"file i = open(\"$lines_file\", \"r\");"$'\nprint(read(i));\nprint(read(i));\nprint(len(read(i)));\nclose(i);' $'line one\nline two\n0'

# Ограниченный фаззер (аудит 2026-11-12): 400 входов с фиксированным seed; любой код 3, сигнал,
# зависание, ошибка без текста или отчёт санитайзера — провал. Проверяет и ASan-сборку в CI.
if command -v python3 >/dev/null 2>&1; then
    if ! timeout --signal=TERM 600s python3 tests/fuzz.py "$SC_BIN" 400 20251112 "$TMP_DIR/fuzz"; then
        echo "FAIL: fuzzer found compiler crashes (inputs saved in $TMP_DIR/fuzz)" >&2
        exit 1
    fi
    echo "PASS: fuzzer (400 inputs, seed 20251112)"
else
    echo "SKIP: python3 not found; fuzzer skipped"
fi

# Каждый пример из docs/spec.md компилируется, запускается и даёт указанный вывод.
bash tests/spec-examples.sh
echo "All compiler tests passed."
