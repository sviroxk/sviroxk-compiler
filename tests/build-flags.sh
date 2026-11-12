#!/usr/bin/env bash
# build.sh must pass LLVM headers as -isystem even when the include path has no
# lowercase "llvm" in it (e.g. official release tarballs in /opt/LLVM-23.1.5-Linux-X64):
# otherwise -Wextra -Werror fails on warnings inside LLVM's own headers.
set -Eeuo pipefail
cd "$(dirname "$0")/.."
real_config=$(command -v llvm-config)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/src"
ln -s "$("$real_config" --includedir)" "$work/HEADERS"
cat > "$work/bin/llvm-config" <<EOF
#!/usr/bin/env bash
if [[ "\$*" == "--cxxflags" ]]; then
    "$real_config" --cxxflags | sed "s|-I$("$real_config" --includedir)|-I$work/HEADERS|g"
else
    exec "$real_config" "\$@"
fi
EOF
chmod +x "$work/bin/llvm-config"
cp ./*.cpp ./*.h build.sh "$work/src/"
if ! (cd "$work/src" && PATH="$work/bin:$PATH" CXX="${CXX:-clang++}" \
        CXXFLAGS="-Wall -Wextra -Werror" ./build.sh) >"$work/build.log" 2>&1; then
    echo "FAIL: build.sh with LLVM include path without 'llvm' in its name" >&2
    tail -20 "$work/build.log" >&2
    exit 1
fi
echo "PASS: build.sh treats llvm-config include paths as system headers"

# The compiler is C++20: an older -std must be rejected with a clear message.
read -r -a llvm_flags <<< "$("$real_config" --cxxflags)"
if "${CXX:-clang++}" -fsyntax-only "${llvm_flags[@]}" -std=c++17 -fexceptions main.cpp \
        >"$work/std.log" 2>&1; then
    echo "FAIL: main.cpp compiled as C++17" >&2
    exit 1
fi
if ! grep -Fq 'SC собирается как C++20' "$work/std.log"; then
    echo "FAIL: C++17 build did not report the C++20 requirement" >&2
    tail -5 "$work/std.log" >&2
    exit 1
fi
echo "PASS: compiler sources require C++20"
