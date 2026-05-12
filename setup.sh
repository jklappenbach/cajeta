#!/bin/bash
set -e

# Allow override; default to LLVM 18 from Ubuntu 24.04 (llvm-18-dev).
LLVM_DIR="${LLVM_DIR:-/usr/lib/llvm-18/lib/cmake/llvm}"

mkdir -p build
pushd build > /dev/null
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Debug -DLLVM_DIR="${LLVM_DIR}"
popd > /dev/null
