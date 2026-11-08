#!/usr/bin/env bash
set -Eeuo pipefail
cd "$(dirname "$0")"

if ! command -v llvm-config >/dev/null 2>&1; then
    echo "Ошибка: llvm-config не найден. Установите LLVM и добавьте его в PATH." >&2
    exit 1
fi

CXX=${CXX:-clang++}
# When coverage instrumentation is requested, keep the build directory on disk
# so the .gcno/.gcda files it produces survive past this script. llvm-cov
# needs those files to map runtime counts back to source lines.
if [[ "${CXXFLAGS:-}" == *"--coverage"* ]]; then
    BUILD_DIR="$PWD/.sc-coverage-build"
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
else
    BUILD_DIR=$(mktemp -d)
    trap 'rm -rf "$BUILD_DIR"' EXIT
fi
# Optional extra diagnostics/sanitizers, e.g. CXXFLAGS='-Wall -Wextra -fsanitize=address,undefined'.
read -r -a EXTRA_CXXFLAGS <<< "${CXXFLAGS:-}"
LLVM_CXXFLAGS=$(llvm-config --cxxflags)
LLVM_LDFLAGS=$(llvm-config --ldflags --libs)
read -r -a LLVM_CXXFLAG_ARRAY <<< "$LLVM_CXXFLAGS"
LLVM_COMPILE_FLAGS=()
for ((i = 0; i < ${#LLVM_CXXFLAG_ARRAY[@]}; ++i)); do
    flag=${LLVM_CXXFLAG_ARRAY[i]}
    if [[ "$flag" == -I* ]]; then
        include_dir=${flag#-I}
        if [[ "$include_dir" == *llvm* ]]; then
            LLVM_COMPILE_FLAGS+=("-isystem" "$include_dir")
        else
            LLVM_COMPILE_FLAGS+=("$flag")
        fi
    else
        LLVM_COMPILE_FLAGS+=("$flag")
    fi
done

for source in main lexer parser semantic codegen; do
    "$CXX" -c "$source.cpp" "${LLVM_COMPILE_FLAGS[@]}" \
        "${EXTRA_CXXFLAGS[@]}" -fexceptions -o "$BUILD_DIR/$source.o"
done

# Default output path is ./sc so the normal (non-coverage) build matches the
# documented CLI. Under coverage instrumentation, write the binary into the
# build directory so .gcda files land next to the matching .gcno files.
if [[ "${CXXFLAGS:-}" == *"--coverage"* ]]; then
    OUTPUT="$BUILD_DIR/sc"
else
    OUTPUT="${SC_BIN:-sc}"
fi
"$CXX" "$BUILD_DIR/main.o" "$BUILD_DIR/lexer.o" \
    "$BUILD_DIR/parser.o" "$BUILD_DIR/semantic.o" "$BUILD_DIR/codegen.o" \
    $LLVM_LDFLAGS "${EXTRA_CXXFLAGS[@]}" -fexceptions -o "$OUTPUT"
chmod +x "$OUTPUT" run.sh build.sh test.sh test-conformance.sh 2>/dev/null || true
echo "Сборка SC завершена"
