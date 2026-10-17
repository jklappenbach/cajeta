#!/usr/bin/env bash
# Every `__cajeta_tls_*` entry point the runtime defines in
# runtime/native/cajeta_tls.c must be bound in the Windows JIT symbol bridge
# (src/cajeta/jit/CajetaJitWinSymbols.cpp). cajeta_tls.c is linked natively,
# not embedded in the runtime bitcode, so on COFF the JIT finds its symbols only
# through that table: a definition missing from it fails EVERY JIT test on
# Windows with "Symbols not found: [ __cajeta_tls_... ]", which is how the first
# v0.43.0 cut lost its Windows leg (143 of 902) over two unbound entries.
# Runs on Linux, so the gap is caught before a Windows build is attempted.
set -u
cd "$(dirname "$0")/.."
missing=0
while read -r name; do
    [ -z "$name" ] && continue
    if ! grep -q "CJ_SYM(\"$name\"" src/cajeta/jit/CajetaJitWinSymbols.cpp; then
        echo "check-jit-win-symbols: $name is defined in runtime/native/cajeta_tls.c but not bound in src/cajeta/jit/CajetaJitWinSymbols.cpp" >&2
        missing=$((missing + 1))
    fi
done < <(grep -oE '^(int|int64_t|void|void\*|const char\*|long)[a-zA-Z0-9_ *]* __cajeta_tls_[a-zA-Z0-9_]+' runtime/native/cajeta_tls.c | awk '{print $NF}' | sort -u)
if [ "$missing" -gt 0 ]; then
    echo "check-jit-win-symbols: $missing unbound" >&2
    exit 1
fi
echo "check-jit-win-symbols: OK"
