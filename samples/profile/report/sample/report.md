# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 1.390 | 1.442 | 454.18M/s | 620 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 3.634 | 3.958 | 475.29M/s | 1980 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 8.305 | 8.724 | 271.06M/s | 3608 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.135 | 0.138 | 4.69G/s | 2708 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.417 | 0.482 | 4.15G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.730 | 0.814 | 3.08G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 10.218 | 10.543 | 169.04M/s | 479368 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 13.481 | 14.586 | 128.12M/s | 595212 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 33.520 | 35.038 | 51.53M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.025 | 0.027 | 567.79K/s | 864 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 10.406 | 10.757 | 100.77M/s | 136556 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 11.432 | 11.813 | 91.72M/s | 102420 | ok |
| json-dom |  | rust | serde_json | 631514 | 0.985 | 1.005 | 640.82M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.770 | 0.776 | 819.70M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 2.264 | 2.459 | 762.77M/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.575 | 1.591 | 1.10G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 4.126 | 4.151 | 545.59M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 4.222 | 4.829 | 533.23M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.085 | 0.088 | 7.43G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.119 | 0.124 | 5.29G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.230 | 0.231 | 7.50G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.316 | 0.318 | 5.47G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.238 | 1.242 | 1.82G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.327 | 1.333 | 1.70G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.598 | 3.909 | 175.52M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 1.765 | 2.455 | 357.82M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 8.490 | 9.808 | 203.43M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 4.515 | 5.512 | 382.52M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 15.090 | 15.862 | 149.18M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 13.188 | 13.546 | 170.69M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.746 | 1.830 | 361.79M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.689 | 0.703 | 916.13M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.905 | 0.914 | 697.96M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.133 | 1.147 | 557.19M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 4.027 | 4.089 | 428.90M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.041 | 2.056 | 846.35M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.629 | 2.657 | 657.03M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 3.049 | 3.073 | 566.57M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 32.079 | 32.472 | 70.17M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.483 | 5.586 | 410.59M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 5.725 | 5.956 | 393.22M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.476 | 8.666 | 265.59M/s | -1 | ok |
| json-dom |  | java | jackson | 631514 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 1727204 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 2251051 |  |  |  | -1 | skipped |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 0.963 | 1.007 | 103.84M/s | 45368 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 2.464 | 2.528 | 20.29M/s | 69128 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 11.668 | 12.590 | 2.57M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 3.203 | 3.286 | 31.22M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 1.251 | 1.285 | 31.97M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 4.307 | 4.378 | 11.61M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 3.388 | 3.481 | 5.90M/s | 25016 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 3.109 | 3.133 | 16.08M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | stdlib | 50000 | 3.344 | 3.668 | 14.95M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 3.436 | 3.511 | 14.55M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 5.255 | 5.375 | 9.51M/s | 11736 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.422 | 0.435 | 118.62M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.013 | 0.013 | 3.73G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.017 | 0.017 | 2.98G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.107 | 0.107 | 469.39M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.737 | 0.746 | 67.87M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.924 | 0.930 | 54.09M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.840 | 2.140 | 27.18M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.718 | 0.731 | 69.65M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.257 | 0.273 | 194.81M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.52G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.150 | 0.150 | 334.18M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.042 | 1.21G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.498 | 0.506 | 100.37M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.078 | 0.078 | 640.15M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.056 | 2.107 | 24.32M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.246 | 2.286 | 22.26M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.261 | 2.770 | 22.12M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.017 | 0.031 | 2.93G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.026 | 0.041 | 1.90G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.092 | 0.101 | 541.28M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.151 | 6.822 | 8.13M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.778 | 2.814 | 18.00M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.438 | 7.548 | 6.72M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.106 | 0.110 | 470.94M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.110 | 0.112 | 454.76M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.807 | 0.815 | 61.92M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.534 | 7.659 | 6.64M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.563 | 4.621 | 10.96M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 10.117 | 12.379 | 395.38K/s | 1891392 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.211 | 0.211 | 1.71G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 1.016 | 1.034 | 354.89M/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.278 | 0.283 | 1.30G/s | 17604 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.026 | 0.026 | 40.47G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.247 | 4.26G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 2.244 | 2.277 | 467.21M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.723 | 1.732 | 608.57M/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.029 | 0.029 | 35.74G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.420 | 0.423 | 2.50G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.252 | 1.256 | 837.30M/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.009 | 0.009 | 114.64G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.538 | 0.541 | 1.95G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.936 | 1.089 | 1.12G/s | -1 | ok |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.020 | 0.020 | 51.97G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.419 | 0.422 | 2.50G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.937 | 0.938 | 1.12G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 45.21G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.420 | 0.422 | 2.50G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.936 | 0.938 | 1.12G/s | -1 | ok |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 73.834 | 77.582 | 13.54M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 32.444 | 34.879 | 30.82M/s | 0 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.597 | 0.609 | 1.67G/s | 0 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.452 | 0.466 | 2.21G/s | 0 | ok |
| matmul |  | cajeta | stdlib | 40000 | 2.947 | 2.977 | 13.57M/s | 0 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.666 | 28.793 | 22.33M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 161.585 | 163.055 | 62/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 3.508 | 3.594 | 28.51K/s | 0 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 51.077 | 52.281 | 19.58M/s | 0 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 5.100 | 5.115 | 19.61M/s | 0 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 9.522 | 9.677 | 105.02M/s | 0 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 190.893 | 214.373 | 104.77K/s | 0 | ok |
