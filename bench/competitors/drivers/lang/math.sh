#!/usr/bin/env bash
# The math/math program measures saxpy and dot-product (lang/, Math.md's SIMD
# rows) beside matmul (math/); lang.sh keeps its two rows, math.sh the third.
exec bash "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )/../math/math.sh" "$@"
