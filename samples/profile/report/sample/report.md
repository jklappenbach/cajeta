# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 1.435 | 1.532 | 440.03M/s | 620 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 3.641 | 3.808 | 474.31M/s | 1884 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 8.270 | 8.666 | 272.20M/s | 3472 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.159 | 0.159 | 3.98G/s | 2572 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.383 | 0.387 | 4.51G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.712 | 0.757 | 3.16G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 10.457 | 11.020 | 165.17M/s | 479368 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 12.139 | 14.318 | 142.28M/s | 595212 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 32.270 | 35.037 | 53.52M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.025 | 0.027 | 565.04K/s | 864 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 10.938 | 11.011 | 95.86M/s | 136556 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 11.638 | 12.153 | 90.10M/s | 102420 | ok |
| json-dom |  | rust | serde_json | 631514 | 0.907 | 0.915 | 695.89M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.712 | 0.717 | 887.58M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 2.272 | 2.343 | 760.19M/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.583 | 1.595 | 1.09G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 4.143 | 4.222 | 543.28M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 4.111 | 4.168 | 547.54M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.084 | 0.085 | 7.54G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.119 | 0.120 | 5.32G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.233 | 0.234 | 7.42G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.308 | 0.310 | 5.60G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.234 | 1.238 | 1.82G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.326 | 1.330 | 1.70G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.242 | 3.434 | 194.76M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 1.727 | 2.215 | 365.74M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 7.547 | 8.556 | 228.86M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 3.304 | 3.993 | 522.83M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 14.266 | 15.342 | 157.79M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 12.214 | 12.947 | 184.30M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.749 | 1.769 | 361.05M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.682 | 0.700 | 926.31M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.907 | 0.916 | 696.01M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.136 | 1.146 | 556.13M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 4.013 | 4.058 | 430.36M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.069 | 2.093 | 834.92M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.709 | 2.765 | 637.56M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 3.001 | 3.138 | 575.61M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 31.902 | 32.614 | 70.56M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.671 | 5.810 | 396.94M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 5.835 | 5.884 | 385.78M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.634 | 8.766 | 260.71M/s | -1 | ok |
| json-dom |  | java | jackson | 631514 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 1727204 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 2251051 |  |  |  | -1 | skipped |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 1.077 | 1.101 | 92.86M/s | 45368 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 2.565 | 2.609 | 19.49M/s | 69128 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 12.145 | 12.364 | 2.47M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 3.313 | 3.372 | 30.18M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 1.312 | 1.327 | 30.49M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 4.509 | 4.534 | 11.09M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 3.338 | 3.425 | 5.99M/s | 25016 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 0.913 | 0.919 | 54.79M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.303 | 0.309 | 165.22M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.101 | 3.477 | 9.67M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 2.903 | 2.922 | 10.33M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.230 | 1.238 | 81.27M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.025 | 0.025 | 4.02G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.787 | 0.790 | 63.55M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.179 | 0.184 | 279.68M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.740 | 2.817 | 10.95M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.494 | 1.558 | 20.09M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.958 | 0.964 | 104.38M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.034 | 0.035 | 2.90G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.787 | 0.920 | 63.54M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.624 | 3.368 | 11.43M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.708 | 2.167 | 58.55M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.397 | 0.610 | 252.11M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.534 | 3.563 | 14.15M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.049 | 6.170 | 4.96M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.416 | 3.506 | 29.27M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.814 | 1.835 | 55.13M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 3.041 | 3.082 | 16.44M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | stdlib | 50000 | 3.338 | 3.355 | 14.98M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 3.543 | 3.560 | 14.11M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 5.566 | 5.577 | 8.98M/s | 11736 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.385 | 0.399 | 129.94M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.012 | 0.012 | 4.06G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.016 | 0.016 | 3.22G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.098 | 0.098 | 508.57M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.587 | 0.594 | 85.22M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.735 | 0.746 | 68.02M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.817 | 1.835 | 27.52M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.711 | 0.720 | 70.32M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.249 | 0.256 | 200.70M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.55G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.150 | 0.150 | 333.89M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.041 | 1.22G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.494 | 0.498 | 101.31M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.078 | 0.078 | 644.78M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.032 | 2.039 | 24.61M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.260 | 2.273 | 22.13M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.237 | 2.284 | 22.35M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.024 | 0.025 | 2.08G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.021 | 0.038 | 2.34G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.083 | 0.087 | 600.41M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.027 | 6.178 | 8.30M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.747 | 2.784 | 18.20M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.424 | 7.523 | 6.73M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.106 | 0.107 | 471.61M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.110 | 0.111 | 455.39M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.812 | 0.820 | 61.61M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.531 | 7.668 | 6.64M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.653 | 4.707 | 10.75M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 10.019 | 12.773 | 399.23K/s | 1891392 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.211 | 0.211 | 1.71G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 1.015 | 1.020 | 355.11M/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.258 | 0.261 | 1.40G/s | 17604 | ok |
| string-search |  | rust | std-find | 360448 | 0.023 | 0.023 | 15.45G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.008 | 0.008 | 47.46G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.104 | 0.105 | 3.45G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.015 | 0.015 | 23.92G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 11.43G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.030 | 0.030 | 12.09G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.096 | 0.097 | 3.74G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.355 | 0.356 | 1.01G/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.35G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.047 | 0.047 | 7.67G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.123 | 0.196 | 2.94G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.377 | 0.389 | 956.86M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.002 | 4.07G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.050 | 0.050 | 7.20G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.380 | 0.383 | 948.83M/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.080 | 0.080 | 4.51G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.011 | 0.011 | 363.64M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.023 | 0.023 | 44.73G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.246 | 4.26G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 2.251 | 2.262 | 465.80M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.647 | 1.650 | 636.54M/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.026 | 0.027 | 39.61G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.417 | 0.486 | 2.51G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.240 | 1.243 | 845.59M/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.009 | 0.009 | 122.41G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.418 | 0.421 | 2.51G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.927 | 0.929 | 1.13G/s | -1 | ok |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.018 | 0.018 | 57.63G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.482 | 0.485 | 2.18G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.927 | 0.929 | 1.13G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.024 | 44.63G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.417 | 0.419 | 2.51G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.925 | 0.928 | 1.13G/s | -1 | ok |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 81.361 | 81.673 | 12.29M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 33.142 | 35.318 | 30.17M/s | 0 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.593 | 0.604 | 1.69G/s | 0 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.391 | 0.392 | 2.56G/s | 0 | ok |
| matmul |  | cajeta | stdlib | 40000 | 2.874 | 2.933 | 13.92M/s | 0 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.592 | 0.603 | 1.69G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.392 | 0.395 | 2.55G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.295 | 2.300 | 17.43M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.403 | 0.407 | 2.48G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.642 | 0.645 | 1.56G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.458 | 2.464 | 16.27M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.412 | 0.418 | 2.43G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.391 | 0.394 | 2.56G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 3.372 | 3.379 | 11.86M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.421 | 0.427 | 2.38G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 0.011 | 0.012 | 87.63G/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.074 | 0.076 | 538.65M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.674 | 28.708 | 22.32M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 162.781 | 164.396 | 61/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 3.637 | 3.739 | 27.50K/s | 0 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 49.057 | 50.361 | 20.38M/s | 0 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 5.004 | 5.023 | 19.99M/s | 0 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 9.580 | 9.593 | 104.39M/s | 0 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 174.856 | 177.668 | 114.38K/s | 0 | ok |
