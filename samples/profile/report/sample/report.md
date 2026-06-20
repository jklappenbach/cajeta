# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 1.440 | 1.457 | 438.54M/s | 616 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 3.831 | 4.042 | 450.84M/s | 1888 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 8.689 | 8.915 | 259.06M/s | 3472 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.159 | 0.160 | 3.98G/s | 2576 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.387 | 0.391 | 4.46G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.717 | 0.736 | 3.14G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 11.166 | 11.353 | 154.68M/s | 479368 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 12.531 | 13.964 | 137.84M/s | 595212 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 33.164 | 34.876 | 52.08M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.026 | 0.027 | 544.56K/s | 864 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 10.526 | 10.743 | 99.61M/s | 136556 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 11.895 | 11.999 | 88.15M/s | 102420 | ok |
| json-dom |  | rust | serde_json | 631514 | 0.927 | 1.072 | 681.31M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.718 | 0.724 | 879.21M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 2.276 | 2.317 | 758.73M/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.565 | 1.581 | 1.10G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 4.120 | 4.158 | 546.41M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 4.178 | 4.237 | 538.77M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.096 | 0.100 | 6.59G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.137 | 0.137 | 4.62G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.264 | 0.264 | 6.55G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.361 | 0.365 | 4.78G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.235 | 1.239 | 1.82G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.326 | 1.341 | 1.70G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.444 | 3.933 | 183.38M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.358 | 2.607 | 267.86M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 8.053 | 8.958 | 214.48M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 3.444 | 4.274 | 501.52M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 14.707 | 15.024 | 153.06M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 12.351 | 12.886 | 182.26M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.766 | 1.794 | 357.61M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.695 | 0.715 | 908.34M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.922 | 1.507 | 685.11M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.136 | 1.183 | 555.77M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 3.923 | 4.044 | 440.31M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.060 | 2.076 | 838.60M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.644 | 2.674 | 653.21M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 2.953 | 2.988 | 584.80M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 32.023 | 33.354 | 70.29M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.658 | 5.697 | 397.87M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 5.811 | 5.948 | 387.38M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.511 | 8.947 | 264.47M/s | -1 | ok |
| json-dom |  | java | jackson | 631514 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 1727204 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 2251051 |  |  |  | -1 | skipped |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 0.996 | 1.007 | 100.42M/s | 45368 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 2.555 | 2.622 | 19.57M/s | 69128 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 12.676 | 12.975 | 2.37M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 3.378 | 3.435 | 29.61M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 1.320 | 1.338 | 30.30M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 4.382 | 4.641 | 11.41M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 3.286 | 3.386 | 6.09M/s | 25016 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 0.834 | 0.840 | 59.97M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.278 | 0.281 | 179.90M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.094 | 3.110 | 9.70M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 2.864 | 2.887 | 10.47M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.246 | 1.252 | 80.23M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.025 | 0.027 | 4.02G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.850 | 0.863 | 58.83M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.220 | 0.222 | 227.09M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.658 | 2.708 | 11.29M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.494 | 1.551 | 20.09M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.981 | 0.988 | 101.98M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.035 | 0.035 | 2.89G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.805 | 0.912 | 62.14M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.682 | 3.916 | 11.18M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.776 | 2.203 | 56.30M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.213 | 0.409 | 470.23M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.572 | 3.626 | 14.00M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.057 | 6.094 | 4.95M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.733 | 3.798 | 26.79M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.905 | 1.908 | 52.50M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 3.045 | 3.080 | 16.42M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | stdlib | 50000 | 3.310 | 3.343 | 15.11M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 3.534 | 3.552 | 14.15M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 5.349 | 5.439 | 9.35M/s | 11736 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.338 | 0.351 | 147.93M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.010 | 0.010 | 5.10G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.013 | 0.014 | 3.71G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.086 | 0.086 | 584.45M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.586 | 0.603 | 85.38M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.739 | 0.749 | 67.64M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.825 | 1.847 | 27.40M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.731 | 0.737 | 68.43M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.258 | 0.263 | 194.01M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.53G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.150 | 0.151 | 332.73M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.041 | 1.21G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.500 | 0.503 | 100.08M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.078 | 0.078 | 640.39M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.030 | 2.058 | 24.63M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.262 | 2.287 | 22.10M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.287 | 2.300 | 21.86M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.014 | 0.014 | 3.51G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.022 | 0.037 | 2.30G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.080 | 0.087 | 623.74M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.197 | 6.249 | 8.07M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.784 | 2.816 | 17.96M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.480 | 7.569 | 6.68M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.100 | 0.102 | 500.86M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.104 | 0.105 | 481.53M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.781 | 0.786 | 64.05M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.470 | 7.635 | 6.69M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.567 | 4.601 | 10.95M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 10.082 | 13.048 | 396.75K/s | 1891392 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.211 | 0.211 | 1.71G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 1.018 | 1.028 | 354.12M/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.258 | 0.262 | 1.40G/s | 17604 | ok |
| string-search |  | rust | std-find | 360448 | 0.019 | 0.019 | 18.91G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.007 | 0.007 | 53.94G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.094 | 0.094 | 3.85G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.013 | 0.013 | 26.83G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 12.90G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.034 | 0.034 | 10.51G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.100 | 0.110 | 3.62G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.406 | 0.411 | 888.05M/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.03G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.048 | 0.048 | 7.55G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.124 | 0.189 | 2.90G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.403 | 0.435 | 894.24M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.002 | 3.53G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.045 | 0.045 | 8.05G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.341 | 0.342 | 1.06G/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.072 | 0.072 | 5.02G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.010 | 0.010 | 398.84M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.024 | 0.024 | 44.57G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.246 | 4.26G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 2.261 | 2.276 | 463.83M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.649 | 1.658 | 636.08M/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.027 | 0.027 | 39.15G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.495 | 0.499 | 2.12G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.240 | 1.247 | 845.36M/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.008 | 0.009 | 124.74G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.497 | 0.499 | 2.11G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.936 | 0.937 | 1.12G/s | -1 | ok |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.018 | 0.019 | 56.73G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.492 | 0.500 | 2.13G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.926 | 0.930 | 1.13G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 44.77G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.418 | 0.420 | 2.51G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.930 | 0.933 | 1.13G/s | -1 | ok |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 75.602 | 77.362 | 13.23M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 32.605 | 33.972 | 30.67M/s | 0 | ok |
| stream-filter-map-reduce |  | rust | Iterator | 1000000 | 0.594 | 0.695 | 1.68G/s | -1 | ok |
| stream-parallel-reduce |  | rust | rayon | 1000000 | 0.081 | 0.127 | 12.38G/s | -1 | ok |
| stream-filter-map-reduce |  | cpp | hand-loop | 1000000 | 0.064 | 0.065 | 15.69G/s | -1 | ok |
| stream-parallel-reduce |  | cpp | OpenMP | 1000000 | 0.081 | 0.095 | 12.34G/s | -1 | ok |
| stream-filter-map-reduce |  | go | hand-loop | 1000000 | 0.586 | 0.590 | 1.71G/s | -1 | ok |
| stream-parallel-reduce |  | go | goroutines | 1000000 | 0.046 | 0.084 | 21.78G/s | -1 | ok |
| stream-filter-map-reduce |  | python | genexpr | 1000000 | 19.123 | 19.369 | 52.29M/s | -1 | ok |
| stream-parallel-reduce |  | python | sum (GIL) | 1000000 | 3.622 | 3.636 | 276.06M/s | -1 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.589 | 0.597 | 1.70G/s | 0 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.392 | 0.396 | 2.55G/s | 0 | ok |
| matmul |  | cajeta | stdlib | 40000 | 2.864 | 2.955 | 13.96M/s | 0 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.659 | 0.748 | 1.52G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.391 | 0.394 | 2.56G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.301 | 2.324 | 17.38M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.453 | 0.458 | 2.21G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.727 | 0.734 | 1.38G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.496 | 2.786 | 16.02M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.405 | 0.413 | 2.47G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.391 | 0.395 | 2.56G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 3.405 | 3.423 | 11.75M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.398 | 0.441 | 2.52G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 1.864 | 3.110 | 536.58M/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.069 | 0.071 | 577.53M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.944 | 29.108 | 22.11M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 161.541 | 163.969 | 62/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 3.533 | 3.841 | 28.31K/s | 0 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 27.070 | 27.172 | 23.64M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 162.641 | 163.277 | 61/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.316 | 0.329 | 316.17K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.753 | 24.002 | 26.94M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 156.875 | 158.075 | 64/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.136 | 0.137 | 732.94K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 27.861 | 27.919 | 22.97M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 147.731 | 149.162 | 68/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.316 | 0.338 | 316.07K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 120.743 | 126.582 | 5.30M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4277.893 | 4333.645 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 37.354 | 37.793 | 2.68K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 51.121 | 51.274 | 19.56M/s | 0 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 5.138 | 5.189 | 19.46M/s | 0 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 6.957 | 7.332 | 143.75M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.569 | 0.572 | 175.74M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.109 | 0.109 | 9.14G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.011 | 0.011 | 9.12G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 2.944 | 3.309 | 339.71M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 1.531 | 1.543 | 65.33M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 350.701 | 357.611 | 2.85M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 16.938 | 17.313 | 5.90M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 9.528 | 9.552 | 104.95M/s | 0 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 179.924 | 183.912 | 111.16K/s | 0 | ok |
| atomic-fetchadd |  | rust | AtomicI64 | 1000000 | 4.512 | 4.533 | 221.63M/s | -1 | ok |
| task-spawn-await |  | rust | std::thread | 20000 | 328.609 | 428.245 | 60.86K/s | -1 | ok |
| atomic-fetchadd |  | cpp | std::atomic | 1000000 | 3.917 | 3.933 | 255.28M/s | -1 | ok |
| task-spawn-await |  | cpp | std::thread | 20000 | 310.419 | 355.381 | 64.43K/s | -1 | ok |
| atomic-fetchadd |  | go | atomic.Int64 | 1000000 | 3.956 | 3.979 | 252.76M/s | -1 | ok |
| task-spawn-await |  | go | goroutine | 20000 | 5.156 | 5.676 | 3.88M/s | -1 | ok |
| atomic-fetchadd |  | python | atomic | 1000000 |  |  |  | -1 | skipped |
| task-spawn-await |  | python | asyncio | 20000 | 68.151 | 69.624 | 293.47K/s | -1 | ok |
