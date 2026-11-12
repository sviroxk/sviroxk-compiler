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
