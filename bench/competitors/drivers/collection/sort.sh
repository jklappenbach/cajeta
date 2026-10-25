#!/usr/bin/env bash
# collection/ sort competitors: sort-int64 (random, ascending, descending,
# dups), sort-f64 and sort-stable-int64 at the page shape (1e6; n/100 under
# CAJETA_BENCH_SMOKE=1): C++ std::sort and pdqsort and std::stable_sort, Rust
# sort_unstable and sort_by_key, Go slices.Sort and SortStableFunc, Java
# Arrays.sort (primitives; objects for the stable row), Python list.sort; same
# inputs and checks as the Cajeta benchmarks (lib/collection_driver.sh).
PROG=sort
JAVA_MAIN=SortBench
LANGS_SUPPORTED="rust cpp go python java"
IDS="sort-int64:1000000 sort-f64:1000000 sort-stable-int64:1000000"
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/collection_driver.sh"
coll_run_all
