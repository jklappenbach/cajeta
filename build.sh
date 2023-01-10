#!/bin/bash

pushd .
cd build
cmake --build . --target all -j 14
popd
