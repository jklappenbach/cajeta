# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | 631514 | 1.415 | 1.460 | 446.22M/s | 624 | ok |
| json-tokenize | citm_catalog | cajeta | 1727204 | 3.424 | 3.557 | 504.38M/s | 2124 | ok |
| json-tokenize | canada | cajeta | 2251051 | 7.854 | 8.436 | 286.62M/s | 3712 | ok |
| json-bind-skip | twitter | cajeta | 631514 | 0.153 | 0.154 | 4.13G/s | 2812 | ok |
| json-bind-skip | citm_catalog | cajeta | 1727204 | 0.374 | 0.662 | 4.62G/s | 0 | ok |
| json-bind-skip | canada | cajeta | 2251051 | 0.642 | 0.661 | 3.51G/s | 4 | ok |
| json-dom | twitter | cajeta | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | 1727204 | 9.847 | 10.054 | 175.41M/s | 479344 | ok |
| json-dom | canada | cajeta | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | 1727204 | 12.761 | 14.253 | 135.35M/s | 595216 | ok |
| json-serialize | canada | cajeta | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | 1727204 | 31.409 | 33.661 | 54.99M/s | 1074752 | ok |
| json-roundtrip | canada | cajeta | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | 14 | 0.025 | 0.026 | 560.74K/s | 864 | ok |
| base64-encode |  | cajeta | 1048576 | 10.808 | 10.898 | 97.02M/s | 136556 | ok |
| base64-decode |  | cajeta | 1048576 | 11.620 | 11.846 | 90.24M/s | 102416 | ok |

## collection

| benchmark | variant | lang | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | 100000 | 0.999 | 1.009 | 100.10M/s | 45360 | ok |
| hashmap-int |  | cajeta | 50000 | 2.553 | 2.574 | 19.58M/s | 69128 | ok |
| hashmap-string |  | cajeta | 30000 | 11.412 | 11.648 | 2.63M/s | 338920 | ok |
| hashset-dedup |  | cajeta | 100000 | 3.212 | 3.310 | 31.14M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | 40000 | 1.243 | 1.262 | 32.18M/s | 56268 | ok |
| heap-sort |  | cajeta | 50000 | 4.443 | 4.569 | 11.25M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | 20000 | 3.241 | 3.363 | 6.17M/s | 25016 | ok |

## sort

| benchmark | variant | lang | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | 50000 | 3.059 | 3.078 | 16.34M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | 50000 | 3.306 | 3.342 | 15.12M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | 50000 | 3.538 | 3.554 | 14.13M/s | 23456 | ok |
| binary-search |  | cajeta | 50000 | 5.258 | 5.363 | 9.51M/s | 11732 | ok |

## string

| benchmark | variant | lang | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | 4000 | 10.179 | 12.697 | 392.97K/s | 1891396 | ok |
| string-search |  | cajeta | 360448 | 0.138 | 0.140 | 2.61G/s | 0 | ok |
| string-replace |  | cajeta | 360448 | 1.147 | 1.157 | 314.26M/s | 18800 | ok |
| string-uppercase |  | cajeta | 360448 | 0.327 | 0.331 | 1.10G/s | 17600 | ok |

## hash

| benchmark | variant | lang | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | 1048576 | 0.024 | 0.024 | 44.57G/s | 0 | ok |
| siphash |  | cajeta | 1048576 | 0.246 | 0.246 | 4.26G/s | 0 | ok |
| sha256 |  | cajeta | 1048576 | 2.232 | 2.252 | 469.80M/s | 0 | ok |
| md5 |  | cajeta | 1048576 | 1.719 | 1.726 | 609.94M/s | 0 | ok |

## stream

| benchmark | variant | lang | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | 1000000 | 71.525 | 74.613 | 13.98M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | 1000000 | 32.281 | 33.488 | 30.98M/s | 0 | ok |

## math

| benchmark | variant | lang | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | 1000000 | 0.586 | 0.593 | 1.71G/s | 0 | ok |
| dot-product |  | cajeta | 1000000 | 0.391 | 0.392 | 2.56G/s | 0 | ok |
| matmul |  | cajeta | 40000 | 3.139 | 3.143 | 12.74M/s | 0 | ok |

## clbg

| benchmark | variant | lang | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | 640000 | 28.683 | 28.881 | 22.31M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | 10 | 161.333 | 164.446 | 62/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | 100 | 3.406 | 3.617 | 29.36K/s | 0 | ok |

## time

| benchmark | variant | lang | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | 1000000 | 55.308 | 55.791 | 18.08M/s | 0 | ok |
| time-localdate-arith |  | cajeta | 100000 | 5.424 | 5.576 | 18.44M/s | 0 | ok |

## concurrent

| benchmark | variant | lang | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | 1000000 | 9.500 | 9.537 | 105.27M/s | 0 | ok |
| task-spawn-await |  | cajeta | 20000 | 194.137 | 194.811 | 103.02K/s | 0 | ok |
