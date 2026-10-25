#!/usr/bin/env bash
# collection/ hash competitors: hashmap-lookup-hit/-miss/-iterate,
# hashset-contains, immutablemap-build/-lookup, immutableset-build/-contains, in
# all five languages (C++ std and unordered_dense, Rust std and hashbrown), same
# sizes, inputs and checks as the Cajeta benchmarks (lib/collection_driver.sh).
PROG=maps
JAVA_MAIN=MapsBench
LANGS_SUPPORTED="rust cpp go python java"
IDS="hashmap-lookup-hit:10000000 hashmap-lookup-miss:10000000 hashmap-iterate:10000000 hashset-contains:10000000
immutablemap-build:10000000 immutablemap-lookup:10000000 immutableset-build:10000000 immutableset-contains:10000000"
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/collection_driver.sh"
coll_run_all
