# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 1.551 | 1.747 | 407.22M/s | 620 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 3.685 | 3.890 | 468.74M/s | 1936 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 8.973 | 9.023 | 250.87M/s | 3472 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.147 | 0.153 | 4.29G/s | 2572 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.383 | 0.386 | 4.51G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.730 | 0.741 | 3.08G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 10.489 | 10.615 | 164.67M/s | 479368 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 13.638 | 14.511 | 126.65M/s | 595212 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 33.628 | 35.139 | 51.36M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.024 | 0.026 | 576.46K/s | 864 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 11.252 | 11.294 | 93.19M/s | 136556 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 12.388 | 12.461 | 84.64M/s | 102420 | ok |
| json-dom |  | rust | serde_json | 631514 | 1.039 | 1.191 | 608.05M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.782 | 0.790 | 807.94M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 1.632 | 2.245 | 1.06G/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.801 | 1.818 | 959.04M/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 3.422 | 3.432 | 657.81M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 4.424 | 4.529 | 508.86M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.104 | 0.107 | 6.09G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.143 | 0.144 | 4.41G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.288 | 0.289 | 5.99G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.318 | 0.382 | 5.44G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.234 | 1.242 | 1.82G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.357 | 1.385 | 1.66G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.922 | 4.176 | 161.04M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.421 | 2.680 | 260.83M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 9.011 | 10.530 | 191.68M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 4.289 | 5.922 | 402.69M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 15.756 | 16.561 | 142.87M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 13.662 | 15.351 | 164.76M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.738 | 1.757 | 363.41M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.667 | 0.675 | 947.09M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.887 | 0.894 | 712.22M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.111 | 1.127 | 568.31M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 4.075 | 4.111 | 423.83M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.102 | 2.112 | 821.69M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.701 | 2.762 | 639.48M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 3.028 | 3.083 | 570.49M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 31.165 | 32.207 | 72.23M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.681 | 5.770 | 396.24M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 6.105 | 6.159 | 368.70M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.866 | 8.888 | 253.90M/s | -1 | ok |
| json-dom |  | java | jackson | 631514 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 1727204 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 2251051 |  |  |  | -1 | skipped |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 1.093 | 1.098 | 91.45M/s | 45368 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 2.547 | 2.577 | 19.63M/s | 69128 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 11.944 | 12.273 | 2.51M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 3.394 | 3.437 | 29.47M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 1.366 | 1.381 | 29.27M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 4.648 | 4.689 | 10.76M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 3.401 | 3.468 | 5.88M/s | 25016 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 0.916 | 0.928 | 54.60M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.311 | 0.316 | 161.02M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.681 | 3.697 | 8.15M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 3.447 | 3.462 | 8.70M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.226 | 1.232 | 81.53M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.034 | 0.036 | 2.90G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.929 | 0.940 | 53.81M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.217 | 0.217 | 230.65M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.756 | 2.777 | 10.89M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.471 | 1.532 | 20.39M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.985 | 0.992 | 101.48M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.033 | 0.035 | 3.00G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.817 | 1.128 | 61.18M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.940 | 3.287 | 10.21M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.765 | 2.194 | 56.65M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.387 | 0.468 | 258.39M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.516 | 3.973 | 14.22M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.265 | 6.365 | 4.79M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.861 | 3.866 | 25.90M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.824 | 1.827 | 54.82M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 3.094 | 3.119 | 16.16M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | stdlib | 50000 | 3.307 | 3.332 | 15.12M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 3.488 | 3.506 | 14.33M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 5.676 | 5.723 | 8.81M/s | 11736 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.380 | 0.390 | 131.48M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.011 | 0.011 | 4.50G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.015 | 0.015 | 3.27G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.103 | 0.104 | 483.26M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.665 | 0.672 | 75.23M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.831 | 0.833 | 60.19M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 2.088 | 2.109 | 23.94M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.834 | 0.844 | 59.94M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.254 | 0.257 | 197.17M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.54G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.154 | 0.154 | 324.93M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.041 | 1.22G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.490 | 0.494 | 102.04M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.077 | 0.077 | 647.87M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.042 | 2.068 | 24.49M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.241 | 2.269 | 22.31M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.247 | 2.580 | 22.25M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.016 | 0.022 | 3.14G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.024 | 0.024 | 2.06G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.085 | 0.094 | 591.51M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.088 | 6.121 | 8.21M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.767 | 2.772 | 18.07M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.386 | 7.477 | 6.77M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.104 | 0.108 | 480.05M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.110 | 0.111 | 455.22M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.809 | 0.815 | 61.82M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.549 | 7.597 | 6.62M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.658 | 4.693 | 10.73M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 9.388 | 12.183 | 426.07K/s | 1891392 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.210 | 0.210 | 1.72G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 1.015 | 1.018 | 355.14M/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.256 | 0.260 | 1.41G/s | 17604 | ok |
| string-search |  | rust | std-find | 360448 | 0.024 | 0.024 | 14.88G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.005 | 0.005 | 68.40G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.118 | 0.118 | 3.05G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.011 | 0.011 | 34.07G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 9.50G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.036 | 0.036 | 10.09G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.117 | 0.117 | 3.09G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.422 | 0.430 | 853.26M/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.10G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.050 | 0.050 | 7.26G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.123 | 0.201 | 2.93G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.452 | 0.545 | 797.89M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.002 | 3.17G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.044 | 0.044 | 8.17G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.336 | 0.339 | 1.07G/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.071 | 0.071 | 5.10G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.010 | 0.010 | 411.61M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.024 | 0.024 | 44.57G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.247 | 0.247 | 4.25G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 2.262 | 2.274 | 463.50M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.719 | 1.724 | 609.86M/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.039 | 0.039 | 27.21G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.486 | 0.488 | 2.16G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.241 | 1.444 | 844.68M/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.008 | 0.009 | 123.42G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.485 | 0.487 | 2.16G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.929 | 1.075 | 1.13G/s | -1 | ok |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.018 | 0.018 | 57.26G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.483 | 0.487 | 2.17G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.929 | 1.076 | 1.13G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 45.40G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.416 | 0.422 | 2.52G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.927 | 0.930 | 1.13G/s | -1 | ok |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 82.221 | 82.449 | 12.16M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 34.540 | 35.825 | 28.95M/s | 0 | ok |
| stream-filter-map-reduce |  | rust | Iterator | 1000000 | 0.588 | 0.685 | 1.70G/s | -1 | ok |
| stream-parallel-reduce |  | rust | rayon | 1000000 | 0.090 | 0.111 | 11.10G/s | -1 | ok |
| stream-filter-map-reduce |  | cpp | hand-loop | 1000000 | 0.063 | 0.063 | 15.92G/s | -1 | ok |
| stream-parallel-reduce |  | cpp | OpenMP | 1000000 | 0.071 | 0.091 | 14.17G/s | -1 | ok |
| stream-filter-map-reduce |  | go | hand-loop | 1000000 | 0.587 | 0.590 | 1.70G/s | -1 | ok |
| stream-parallel-reduce |  | go | goroutines | 1000000 | 0.053 | 0.076 | 18.87G/s | -1 | ok |
| stream-filter-map-reduce |  | python | genexpr | 1000000 | 20.454 | 20.534 | 48.89M/s | -1 | ok |
| stream-parallel-reduce |  | python | sum (GIL) | 1000000 | 3.757 | 3.782 | 266.19M/s | -1 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.585 | 0.595 | 1.71G/s | 0 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.390 | 0.392 | 2.56G/s | 0 | ok |
| matmul |  | cajeta | stdlib | 40000 | 2.823 | 2.922 | 14.17M/s | 0 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.635 | 0.728 | 1.57G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.393 | 0.394 | 2.55G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.292 | 2.297 | 17.45M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.643 | 0.657 | 1.55G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.642 | 0.645 | 1.56G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.461 | 2.468 | 16.25M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.472 | 0.480 | 2.12G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.455 | 0.460 | 2.20G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 3.039 | 3.058 | 13.16M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.469 | 0.477 | 2.13G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 2.003 | 2.999 | 499.34M/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.072 | 0.075 | 552.82M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.647 | 28.667 | 22.34M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 159.253 | 160.953 | 63/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 3.845 | 3.852 | 26.01K/s | 0 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 26.757 | 26.791 | 23.92M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 159.529 | 160.189 | 63/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.319 | 0.325 | 313.28K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.603 | 23.652 | 27.12M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 157.452 | 157.908 | 64/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.137 | 0.137 | 731.38K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 27.834 | 27.890 | 22.99M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 149.037 | 151.074 | 67/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.313 | 0.332 | 319.48K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 116.813 | 120.216 | 5.48M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4427.068 | 4460.538 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 37.391 | 37.417 | 2.67K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 52.919 | 54.626 | 18.90M/s | 0 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 5.383 | 5.390 | 18.58M/s | 0 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 6.843 | 6.851 | 146.13M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.562 | 0.564 | 177.99M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.110 | 0.110 | 9.10G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.011 | 0.011 | 9.08G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 2.936 | 2.964 | 340.63M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 1.516 | 1.519 | 65.98M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 350.808 | 358.573 | 2.85M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 17.954 | 18.489 | 5.57M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 9.548 | 9.652 | 104.73M/s | 0 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 181.159 | 181.696 | 110.40K/s | 0 | ok |
| atomic-fetchadd |  | rust | AtomicI64 | 1000000 | 3.915 | 3.918 | 255.45M/s | -1 | ok |
| task-spawn-await |  | rust | std::thread | 20000 | 379.187 | 448.288 | 52.74K/s | -1 | ok |
| atomic-fetchadd |  | cpp | std::atomic | 1000000 | 3.912 | 3.916 | 255.60M/s | -1 | ok |
| task-spawn-await |  | cpp | std::thread | 20000 | 336.907 | 353.408 | 59.36K/s | -1 | ok |
| atomic-fetchadd |  | go | atomic.Int64 | 1000000 | 3.908 | 3.936 | 255.87M/s | -1 | ok |
| task-spawn-await |  | go | goroutine | 20000 | 5.095 | 5.507 | 3.93M/s | -1 | ok |
| atomic-fetchadd |  | python | atomic | 1000000 |  |  |  | -1 | skipped |
| task-spawn-await |  | python | asyncio | 20000 | 69.558 | 69.796 | 287.53K/s | -1 | ok |
