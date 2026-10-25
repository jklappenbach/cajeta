#!/usr/bin/env bash
# collection/ sequence competitors: arraylist-get/-iterate/-sort,
# linkedlist-insert-traverse, heap-sort, heap-push-pop, immutablelist-build/-get,
# collectors-to-list/-group-by, binary-search, in all five languages, same
# sizes, inputs and checks as the Cajeta benchmarks (lib/collection_driver.sh).
PROG=lists
JAVA_MAIN=ListsBench
LANGS_SUPPORTED="rust cpp go python java"
IDS="arraylist-get:100000000 arraylist-iterate:100000000 arraylist-sort:1000000 linkedlist-insert-traverse:40000
heap-sort:50000 heap-push-pop:10000000 immutablelist-build:100000000 immutablelist-get:100000000
collectors-to-list:10000000 collectors-group-by:10000000 binary-search:50000"
source "$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../../lib" &> /dev/null && pwd )/collection_driver.sh"
coll_run_all
