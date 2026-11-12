#!/usr/bin/env bash
set -Eeuo pipefail
cd "$(dirname "$0")"

if ! command -v llvm-config >/dev/null 2>&1; then
    echo "Ошибка: llvm-config не найден. Установите LLVM и добавьте его в PATH." >&2
    exit 1
fi

BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT
CXX=${CXX:-clang++}
# Optional extra diagnostics/sanitizers, e.g. CXXFLAGS='-Wall -Wextra -fsanitize=address,undefined'.
read -r -a EXTRA_CXXFLAGS <<< "${CXXFLAGS:-}"
LLVM_CXXFLAGS=$(llvm-config --cxxflags)
LLVM_LDFLAGS=$(llvm-config --ldflags --libs)
read -r -a LLVM_CXXFLAG_ARRAY <<< "$LLVM_CXXFLAGS"
LLVM_COMPILE_FLAGS=()
for ((i = 0; i < ${#LLVM_CXXFLAG_ARRAY[@]}; ++i)); do
    flag=${LLVM_CXXFLAG_ARRAY[i]}
    if [[ "$flag" == -I* ]]; then
        # Every include dir from llvm-config holds LLVM headers: keep their
        # warnings out of -Werror whatever the install path is called.
        LLVM_COMPILE_FLAGS+=("-isystem" "${flag#-I}")
    else
        LLVM_COMPILE_FLAGS+=("$flag")
    fi
done
# Our code is C++20 whatever -std= llvm-config reports (LLVM 18–21 say c++17);
# this flag comes after the llvm-config flags, so it wins.
LLVM_COMPILE_FLAGS+=("-std=c++20")

for source in main lexer parser type semantic codegen; do
    "$CXX" -c "$source.cpp" "${LLVM_COMPILE_FLAGS[@]}" \
        "${EXTRA_CXXFLAGS[@]}" -fexceptions -o "$BUILD_DIR/$source.o"
done

"$CXX" "$BUILD_DIR/main.o" "$BUILD_DIR/lexer.o" \
    "$BUILD_DIR/parser.o" "$BUILD_DIR/semantic.o" "$BUILD_DIR/codegen.o" \
    "$BUILD_DIR/type.o" \
    $LLVM_LDFLAGS "${EXTRA_CXXFLAGS[@]}" -fexceptions -o sc
chmod +x sc run.sh build.sh test.sh test-conformance.sh 2>/dev/null || true
echo "Сборка SC завершена"
