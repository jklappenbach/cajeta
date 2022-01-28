#! /bin/zsh

# llvm-g++ -emit-llvm -S -c $1
clang -S -emit-llvm $1
