#!/bin/bash
set -e

pushd build > /dev/null
cmake --build . --target all -j "$(nproc)"
popd > /dev/null
