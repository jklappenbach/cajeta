#!/usr/bin/env bash
# collection/ ordered-map competitors: redblacktree-insert-lookup,
# redblacktree-range, bplustree-insert, bplustree-point-lookup, bplustree-range:
# C++ std::map, Rust BTreeMap, Go google/btree, Java TreeMap, Python
# sortedcontainers; same sizes, inputs and checks as the Cajeta benchmarks.
PROG=ordered
JAVA_MAIN=OrderedBench
LANGS_SUPPORTED="rust cpp go python java"
IDS="redblacktree-insert-lookup:1000000 redblacktree-range:10000000 bplustree-insert:1000000
bplustree-point-lookup:10000000 bplustree-range:10000000"
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/collection_driver.sh"
coll_run_all
