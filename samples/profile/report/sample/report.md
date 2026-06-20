# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 1.564 | 1.576 | 403.77M/s | 620 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 3.816 | 4.079 | 452.64M/s | 2064 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 8.811 | 8.964 | 255.49M/s | 3760 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.154 | 0.161 | 4.09G/s | 2860 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.344 | 0.362 | 5.02G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.727 | 0.737 | 3.09G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 11.027 | 11.317 | 156.64M/s | 479368 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 14.203 | 14.981 | 121.61M/s | 595212 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 34.126 | 36.517 | 50.61M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.026 | 0.028 | 544.77K/s | 864 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 10.702 | 10.835 | 97.98M/s | 136556 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 12.092 | 12.580 | 86.72M/s | 102420 | ok |
| json-dom |  | rust | serde_json | 631514 | 1.099 | 1.110 | 574.58M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.720 | 0.726 | 877.25M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 2.290 | 2.308 | 754.31M/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.602 | 1.633 | 1.08G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 4.165 | 4.232 | 540.41M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 4.188 | 4.286 | 537.47M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.103 | 0.104 | 6.13G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.144 | 0.145 | 4.40G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.276 | 0.278 | 6.26G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.381 | 0.384 | 4.53G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.237 | 1.244 | 1.82G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.340 | 1.356 | 1.68G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.754 | 4.235 | 168.21M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.482 | 3.087 | 254.40M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 8.435 | 10.543 | 204.76M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 4.581 | 5.365 | 377.01M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 16.724 | 18.853 | 134.60M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 14.965 | 17.718 | 150.42M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 2.015 | 2.050 | 313.48M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.743 | 0.810 | 849.83M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.907 | 0.916 | 696.14M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.129 | 1.166 | 559.42M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 4.061 | 4.112 | 425.33M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.103 | 2.121 | 821.43M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.678 | 2.742 | 645.04M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 3.043 | 3.100 | 567.57M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 32.376 | 32.827 | 69.53M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.670 | 5.720 | 397.02M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 6.070 | 6.186 | 370.85M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.864 | 8.984 | 253.96M/s | -1 | ok |
| json-dom |  | java | jackson | 631514 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 1727204 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 2251051 |  |  |  | -1 | skipped |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 1.029 | 1.040 | 97.14M/s | 45368 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 2.556 | 2.594 | 19.56M/s | 69128 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 12.177 | 12.605 | 2.46M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 3.525 | 3.564 | 28.37M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 1.337 | 1.386 | 29.92M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 4.623 | 4.692 | 10.82M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 3.484 | 3.559 | 5.74M/s | 25016 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 0.846 | 0.854 | 59.13M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.285 | 0.288 | 175.48M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.169 | 3.185 | 9.47M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 2.929 | 2.939 | 10.24M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.248 | 1.255 | 80.16M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.024 | 0.024 | 4.13G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.958 | 0.984 | 52.18M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.196 | 0.197 | 254.91M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.690 | 2.709 | 11.15M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.530 | 1.545 | 19.60M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.961 | 0.970 | 104.02M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.035 | 0.035 | 2.88G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.801 | 0.970 | 62.39M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.521 | 3.178 | 11.90M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.697 | 2.093 | 58.91M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.437 | 0.560 | 228.66M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.414 | 3.522 | 14.65M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.151 | 6.244 | 4.88M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.743 | 3.804 | 26.72M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.814 | 1.891 | 55.12M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 3.187 | 3.221 | 15.69M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | stdlib | 50000 | 3.398 | 3.436 | 14.72M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 3.632 | 3.652 | 13.77M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 6.078 | 6.121 | 8.23M/s | 11736 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.432 | 0.446 | 115.74M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.017 | 0.017 | 2.97G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.017 | 0.022 | 2.89G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.110 | 0.110 | 455.47M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.593 | 0.598 | 84.29M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.756 | 0.766 | 66.11M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.839 | 1.854 | 27.19M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.730 | 0.735 | 68.50M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.280 | 0.284 | 178.57M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.50G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.171 | 0.171 | 292.14M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.042 | 0.042 | 1.19G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.502 | 0.505 | 99.63M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.079 | 0.079 | 633.72M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.068 | 2.096 | 24.18M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.290 | 2.299 | 21.83M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.321 | 2.976 | 21.54M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.014 | 0.024 | 3.46G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.022 | 0.028 | 2.28G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.108 | 0.149 | 464.98M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.163 | 6.357 | 8.11M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.830 | 2.842 | 17.67M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.557 | 7.598 | 6.62M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.106 | 0.106 | 470.68M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.111 | 0.111 | 451.23M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.799 | 0.805 | 62.54M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.667 | 7.712 | 6.52M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.665 | 4.713 | 10.72M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 10.163 | 13.364 | 393.59K/s | 1891392 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.216 | 0.217 | 1.67G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 1.045 | 1.051 | 345.05M/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.262 | 0.268 | 1.37G/s | 17604 | ok |
| string-search |  | rust | std-find | 360448 | 0.023 | 0.023 | 15.62G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.008 | 0.008 | 44.58G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.113 | 0.118 | 3.20G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.016 | 0.016 | 22.33G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 10.53G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.036 | 0.036 | 9.95G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.115 | 0.116 | 3.13G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.363 | 0.415 | 993.34M/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.29G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.050 | 0.050 | 7.18G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.149 | 0.197 | 2.41G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.381 | 0.456 | 945.67M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.002 | 4.12G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.053 | 0.053 | 6.77G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.342 | 0.408 | 1.05G/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.072 | 0.072 | 4.99G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.010 | 0.010 | 401.24M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.024 | 0.024 | 43.54G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.252 | 0.254 | 4.15G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 2.322 | 2.335 | 451.52M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.690 | 1.696 | 620.48M/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.030 | 0.030 | 34.77G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.555 | 0.558 | 1.89G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.259 | 1.271 | 833.19M/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.010 | 0.010 | 101.91G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.551 | 0.555 | 1.90G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.949 | 0.950 | 1.10G/s | -1 | ok |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.021 | 0.021 | 50.34G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.426 | 0.467 | 2.46G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.949 | 0.953 | 1.10G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 45.43G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.424 | 0.427 | 2.47G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.946 | 0.948 | 1.11G/s | -1 | ok |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 76.188 | 80.440 | 13.13M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 33.232 | 35.297 | 30.09M/s | 0 | ok |
| stream-filter-map-reduce |  | rust | Iterator | 1000000 | 0.734 | 0.739 | 1.36G/s | -1 | ok |
| stream-parallel-reduce |  | rust | rayon | 1000000 | 0.085 | 0.120 | 11.83G/s | -1 | ok |
| stream-filter-map-reduce |  | cpp | hand-loop | 1000000 | 0.064 | 0.065 | 15.53G/s | -1 | ok |
| stream-parallel-reduce |  | cpp | OpenMP | 1000000 | 0.060 | 0.072 | 16.65G/s | -1 | ok |
| stream-filter-map-reduce |  | go | hand-loop | 1000000 | 0.726 | 0.730 | 1.38G/s | -1 | ok |
| stream-parallel-reduce |  | go | goroutines | 1000000 | 0.061 | 0.101 | 16.41G/s | -1 | ok |
| stream-filter-map-reduce |  | python | genexpr | 1000000 | 19.103 | 19.282 | 52.35M/s | -1 | ok |
| stream-parallel-reduce |  | python | sum (GIL) | 1000000 | 3.623 | 3.634 | 276.02M/s | -1 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.605 | 0.611 | 1.65G/s | 0 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.393 | 0.397 | 2.54G/s | 0 | ok |
| matmul |  | cajeta | stdlib | 40000 | 2.884 | 2.960 | 13.87M/s | 0 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.693 | 0.705 | 1.44G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.397 | 0.470 | 2.52G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.298 | 2.306 | 17.40M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.468 | 0.522 | 2.14G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.756 | 0.760 | 1.32G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.483 | 2.504 | 16.11M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.466 | 0.470 | 2.15G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.396 | 0.398 | 2.52G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 3.423 | 3.445 | 11.68M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.438 | 0.470 | 2.28G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 2.000 | 5.000 | 500.12M/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.083 | 0.083 | 484.64M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.982 | 29.024 | 22.08M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 161.899 | 167.220 | 62/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 3.606 | 3.617 | 27.73K/s | 0 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 27.054 | 27.082 | 23.66M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 159.927 | 161.120 | 63/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.313 | 0.324 | 319.76K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.606 | 23.637 | 27.11M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 157.684 | 158.537 | 63/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.137 | 0.137 | 727.75K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 27.939 | 28.096 | 22.91M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 147.913 | 149.156 | 68/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.315 | 0.331 | 317.25K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 125.669 | 132.045 | 5.09M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4310.189 | 4380.275 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 37.115 | 37.658 | 2.69K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 51.510 | 51.987 | 19.41M/s | 0 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 5.032 | 5.041 | 19.87M/s | 0 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 6.883 | 6.899 | 145.30M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.565 | 0.567 | 176.95M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.116 | 0.116 | 8.65G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.012 | 0.012 | 8.63G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 2.962 | 2.975 | 337.63M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 1.523 | 1.530 | 65.65M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 359.479 | 376.656 | 2.78M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 17.452 | 17.706 | 5.73M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 9.693 | 9.706 | 103.17M/s | 0 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 188.161 | 194.031 | 106.29K/s | 0 | ok |
