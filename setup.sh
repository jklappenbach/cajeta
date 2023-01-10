#!/bin/bash

mkdir -p build
pushd .
cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
popd
