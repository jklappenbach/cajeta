#!/usr/bin/env bash
# collection/ Cache competitors: cache-hit, cache-miss, cache-evict, cache-ttl:
# Java Caffeine, Rust moka and lru, Go ristretto, Python functools.lru_cache,
# and a hand-written C++ LRU (the page names no C++ library); same sizes,
# inputs and checks as the Cajeta benchmarks (lib/collection_driver.sh).
PROG=cache
JAVA_MAIN=CacheBench
JAVA_JARS="caffeine jspecify error_prone_annotations"
LANGS_SUPPORTED="rust cpp go python java"
IDS="cache-hit:10000000 cache-miss:10000000 cache-evict:10000000 cache-ttl:1000000"
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/collection_driver.sh"
coll_run_all
