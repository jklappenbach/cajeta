#!/usr/bin/env bash
# collection/ltm competitors: ltm-insert and ltm-lookup against RocksDB and LMDB
# through their Rust bindings (crates rocksdb, heed) and Java bindings
# (rocksdbjni, lmdbjava), the LtmBPlusTree.md rows. C++, Go and Python rows are
# skipped: the page measures those two stores through Rust and Java only. The
# workload size, page budget and directory are the CAJETA_BENCH_LTM_* variables
# the Cajeta side reads (bench/src/main/cajeta/bench/collection/LtmEnv.cajeta);
# the directory defaults to competitors/.build/ltm. Without the disk budget the
# rows are skipped with the reason; every row records the storage device.
PROG=ltm
JAVA_MAIN=LtmBench
JAVA_JARS="rocksdbjni lmdbjava lmdbjava-native jnr-ffi jnr-constants jffi jffi-native jnr-a64asm jnr-x86asm asm asm-commons asm-analysis asm-tree asm-util"
LANGS_SUPPORTED="rust java"
UNSUPPORTED_REASON="LtmBPlusTree.md measures RocksDB and LMDB through their Rust and Java bindings only"
IDS="ltm-insert:${CAJETA_BENCH_LTM_N:-1000000} ltm-lookup:${CAJETA_BENCH_LTM_LOOKUPS:-100000}"
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/collection_driver.sh"
export CAJETA_BENCH_LTM_DIR="${CAJETA_BENCH_LTM_DIR:-$BUILD/ltm}"
coll_run_all
