# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 2.801 | 2.903 | 225.48M/s | 620 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 7.129 | 7.622 | 242.28M/s | 2148 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 16.912 | 18.433 | 133.10M/s | 3852 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.276 | 0.286 | 2.29G/s | 2952 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.718 | 0.749 | 2.40G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 1.340 | 1.412 | 1.68G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 17.833 | 19.937 | 96.85M/s | 479368 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 25.150 | 29.360 | 68.68M/s | 595212 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 59.433 | 67.649 | 29.06M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.043 | 0.045 | 325.12K/s | 864 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 16.789 | 17.889 | 62.46M/s | 136556 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 19.197 | 20.105 | 54.62M/s | 102420 | ok |
| json-dom |  | rust | serde_json | 631514 | 1.358 | 1.473 | 465.10M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 1.025 | 1.083 | 616.12M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 3.312 | 3.699 | 521.56M/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 2.500 | 2.570 | 691.00M/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 5.537 | 5.603 | 406.57M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 6.452 | 6.543 | 348.92M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.143 | 0.147 | 4.40G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.248 | 0.256 | 2.55G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.407 | 0.414 | 4.24G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.609 | 0.634 | 2.84G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.993 | 2.027 | 1.13G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 2.295 | 2.363 | 980.73M/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 6.519 | 7.509 | 96.87M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 3.320 | 4.554 | 190.19M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 14.295 | 17.026 | 120.83M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 7.370 | 8.828 | 234.35M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 29.428 | 30.526 | 76.49M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 27.046 | 29.949 | 83.23M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 3.527 | 3.599 | 179.05M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 1.255 | 1.317 | 503.24M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 1.787 | 1.803 | 353.43M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 2.195 | 2.257 | 287.71M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 10.852 | 12.862 | 159.16M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 4.298 | 4.699 | 401.89M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 5.844 | 6.838 | 295.55M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 7.936 | 9.095 | 217.65M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 54.585 | 57.504 | 41.24M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 11.321 | 12.043 | 198.84M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 12.611 | 15.101 | 178.49M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 17.550 | 19.507 | 128.26M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 631514 | 1.936 | 2.149 | 326.24M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 1727204 | 2.594 | 3.493 | 665.96M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 2251051 | 27.505 | 30.955 | 81.84M/s | -1 | ok |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 1.744 | 1.830 | 57.33M/s | 45368 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 3.611 | 3.966 | 13.85M/s | 69128 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 28.320 | 33.292 | 1.06M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 4.834 | 5.295 | 20.69M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 2.078 | 2.145 | 19.25M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 5.541 | 5.859 | 9.02M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 5.539 | 5.956 | 3.61M/s | 25016 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 1.051 | 1.055 | 47.57M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.351 | 0.357 | 142.44M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.767 | 4.263 | 7.96M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 3.694 | 3.743 | 8.12M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.473 | 1.482 | 67.89M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.034 | 0.034 | 2.94G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.880 | 0.888 | 56.84M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.187 | 0.201 | 267.73M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.767 | 2.806 | 10.84M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.548 | 1.564 | 19.39M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.994 | 1.001 | 100.57M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.035 | 0.036 | 2.87G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.799 | 0.927 | 62.59M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.874 | 3.633 | 10.44M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.753 | 2.059 | 57.06M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.307 | 0.449 | 326.00M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.545 | 3.625 | 14.10M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.263 | 7.099 | 4.79M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.767 | 3.891 | 26.55M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.723 | 1.810 | 58.04M/s | -1 | ok |
| hashmap-int |  | java | HashMap | 50000 | 0.512 | 1.515 | 97.57M/s | -1 | ok |
| hashmap-string |  | java | HashMap | 30000 | 1.879 | 3.042 | 15.97M/s | -1 | ok |
| hashset-dedup |  | java | HashSet | 100000 | 1.055 | 1.386 | 94.76M/s | -1 | ok |
| arraylist-append |  | java | ArrayList | 100000 | 0.589 | 0.695 | 169.75M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 3.637 | 3.757 | 13.75M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | stdlib | 50000 | 3.484 | 3.782 | 14.35M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 3.590 | 3.976 | 13.93M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 6.694 | 7.215 | 7.47M/s | 11736 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.496 | 0.518 | 100.85M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.016 | 0.016 | 3.19G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.015 | 0.015 | 3.30G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.132 | 0.138 | 378.96M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.781 | 0.793 | 64.03M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.684 | 0.711 | 73.09M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 2.215 | 2.307 | 22.58M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 1.003 | 1.029 | 49.87M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.345 | 0.381 | 145.11M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.034 | 0.035 | 1.46G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.250 | 0.254 | 199.75M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.066 | 0.071 | 752.16M/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.722 | 0.761 | 69.22M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.116 | 0.123 | 430.04M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.515 | 2.535 | 19.88M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.650 | 2.721 | 18.86M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.758 | 2.811 | 18.13M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.024 | 0.039 | 2.08G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.036 | 0.040 | 1.39G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.153 | 0.184 | 327.12M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 9.291 | 9.519 | 5.38M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 3.213 | 3.367 | 15.56M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 11.655 | 11.975 | 4.29M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.161 | 0.181 | 311.15M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.191 | 0.200 | 261.25M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 1.300 | 1.419 | 38.45M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 14.156 | 15.987 | 3.53M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 5.687 | 6.370 | 8.79M/s | -1 | ok |
| sort-int64 | random | java | Arrays.sort | 50000 | 3.160 | 3.673 | 15.82M/s | -1 | ok |
| sort-int64 | ascending | java | Arrays.sort | 50000 | 0.097 | 0.106 | 513.65M/s | -1 | ok |
| sort-int64 | descending | java | Arrays.sort | 50000 | 0.063 | 0.164 | 794.69M/s | -1 | ok |
| sort-int64 | dups | java | Arrays.sort | 50000 | 0.577 | 0.729 | 86.71M/s | -1 | ok |
| sort-stable-int64 |  | java | Arrays.sort(Timsort) | 50000 | 8.202 | 25.023 | 6.10M/s | -1 | ok |
| sort-f64 |  | java | Arrays.sort | 50000 | 0.479 | 0.552 | 104.47M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 14.147 | 17.555 | 282.75K/s | 1891392 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.231 | 0.249 | 1.56G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 0.971 | 1.013 | 371.16M/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.383 | 0.398 | 939.93M/s | 17604 | ok |
| string-search |  | rust | std-find | 360448 | 0.023 | 0.023 | 15.52G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.005 | 0.005 | 74.03G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.105 | 0.105 | 3.43G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.070 | 0.070 | 5.17G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 13.79G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.033 | 0.033 | 11.05G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.106 | 0.107 | 3.40G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.389 | 0.392 | 926.50M/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.31G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.043 | 0.043 | 8.48G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.130 | 0.176 | 2.78G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.390 | 0.423 | 923.12M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.004 | 3.33G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.045 | 0.045 | 7.99G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.343 | 0.346 | 1.05G/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.072 | 0.073 | 4.98G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.011 | 0.011 | 380.26M/s | -1 | ok |
| string-search |  | java | String.indexOf | 360448 | 0.062 | 0.574 | 5.80G/s | -1 | ok |
| string-replace |  | java | String.replace | 360448 | 0.637 | 0.648 | 566.23M/s | -1 | ok |
| string-uppercase |  | java | String.toUpperCase | 360448 | 0.285 | 0.295 | 1.27G/s | -1 | ok |
| string-build-concat |  | java | StringBuilder | 4000 | 0.019 | 0.022 | 208.27M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.026 | 0.026 | 40.72G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.281 | 0.287 | 3.73G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 2.506 | 3.134 | 418.47M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.948 | 2.034 | 538.24M/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.015 | 0.015 | 72.08G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.462 | 0.466 | 2.27G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.244 | 1.254 | 842.78M/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.008 | 0.009 | 124.15G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.425 | 0.428 | 2.47G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.970 | 0.972 | 1.08G/s | -1 | ok |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.018 | 0.020 | 57.44G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.464 | 0.467 | 2.26G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 1.057 | 1.068 | 992.17M/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.028 | 0.028 | 38.06G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.467 | 0.472 | 2.25G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 1.058 | 1.061 | 990.86M/s | -1 | ok |
| xxhash3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| sha256 |  | java | java.security.MessageDigest | 1048576 | 0.495 | 0.509 | 2.12G/s | -1 | ok |
| md5 |  | java | java.security.MessageDigest | 1048576 | 1.009 | 1.017 | 1.04G/s | -1 | ok |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | java | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 133.358 | 142.198 | 7.50M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 56.489 | 59.926 | 17.70M/s | 4 | ok |
| stream-filter-map-reduce |  | rust | Iterator | 1000000 | 0.638 | 0.653 | 1.57G/s | -1 | ok |
| stream-parallel-reduce |  | rust | rayon | 1000000 | 0.061 | 0.335 | 16.51G/s | -1 | ok |
| stream-filter-map-reduce |  | cpp | hand-loop | 1000000 | 0.082 | 0.087 | 12.24G/s | -1 | ok |
| stream-parallel-reduce |  | cpp | OpenMP | 1000000 | 0.041 | 0.047 | 24.65G/s | -1 | ok |
| stream-filter-map-reduce |  | go | hand-loop | 1000000 | 0.645 | 0.655 | 1.55G/s | -1 | ok |
| stream-parallel-reduce |  | go | goroutines | 1000000 | 0.360 | 0.521 | 2.78G/s | -1 | ok |
| stream-filter-map-reduce |  | python | genexpr | 1000000 | 41.010 | 41.197 | 24.38M/s | -1 | ok |
| stream-parallel-reduce |  | python | sum (GIL) | 1000000 | 5.538 | 5.644 | 180.57M/s | -1 | ok |
| stream-filter-map-reduce |  | java | java.util.stream | 1000000 | 0.357 | 0.412 | 2.80G/s | -1 | ok |
| stream-parallel-reduce |  | java | parallel-stream | 1000000 | 0.251 | 0.835 | 3.98G/s | -1 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.805 | 0.835 | 1.24G/s | 0 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.407 | 0.413 | 2.46G/s | 0 | ok |
| matmul |  | cajeta | stdlib | 40000 | 3.493 | 3.535 | 11.45M/s | 0 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.622 | 0.632 | 1.61G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.398 | 0.404 | 2.51G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.294 | 2.309 | 17.43M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.649 | 0.652 | 1.54G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.659 | 0.663 | 1.52G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.495 | 2.513 | 16.03M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.511 | 0.560 | 1.96G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.406 | 0.409 | 2.47G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 3.077 | 3.120 | 13.00M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.526 | 0.537 | 1.90G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 8.000 | 11.993 | 125.00M/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.078 | 5.039 | 514.62M/s | -1 | ok |
| saxpy |  | java | scalar | 1000000 | 0.502 | 1.493 | 1.99G/s | -1 | ok |
| dot-product |  | java | scalar | 1000000 | 0.430 | 0.898 | 2.32G/s | -1 | ok |
| matmul |  | java | scalar | 40000 | 2.504 | 2.635 | 15.97M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 31.317 | 31.452 | 20.44M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 175.878 | 181.014 | 57/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 6.931 | 7.049 | 14.43K/s | 0 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 25.588 | 25.637 | 25.01M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 160.459 | 161.436 | 62/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.212 | 0.213 | 471.25K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.786 | 24.353 | 26.91M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 162.127 | 163.515 | 62/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.140 | 0.140 | 715.70K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 24.394 | 24.448 | 26.24M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 152.268 | 153.777 | 66/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.320 | 0.338 | 312.65K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 121.057 | 124.513 | 5.29M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4337.170 | 8033.847 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 71.795 | 72.334 | 1.39K/s | -1 | ok |
| clbg-mandelbrot |  | java | stdlib | 640000 | 28.309 | 29.010 | 22.61M/s | -1 | ok |
| clbg-fannkuch-redux |  | java | stdlib | 10 | 196.909 | 201.321 | 51/s | -1 | ok |
| clbg-spectral-norm |  | java | stdlib | 100 | 0.369 | 1.962 | 271.21K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 89.818 | 96.264 | 11.13M/s | 0 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 8.572 | 9.097 | 11.67M/s | 0 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 11.248 | 11.286 | 88.90M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.918 | 0.926 | 108.98M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.176 | 0.185 | 5.68G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.026 | 0.028 | 3.78G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 5.852 | 5.954 | 170.89M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 2.840 | 3.007 | 35.21M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 671.150 | 730.738 | 1.49M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 29.957 | 30.371 | 3.34M/s | -1 | ok |
| time-instant-arith |  | java | java.time | 1000000 | 0.222 | 0.223 | 4.50G/s | -1 | ok |
| time-localdate-arith |  | java | java.time | 100000 | 1.126 | 1.716 | 88.82M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 15.635 | 16.691 | 63.96M/s | 0 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 2286.052 | 2414.476 | 8.75K/s | 0 | ok |
| atomic-fetchadd |  | rust | AtomicI64 | 1000000 | 4.896 | 4.981 | 204.23M/s | -1 | ok |
| task-spawn-await |  | rust | std::thread | 20000 | 14134.956 | 18258.537 | 1.41K/s | -1 | ok |
| atomic-fetchadd |  | cpp | std::atomic | 1000000 | 4.361 | 4.400 | 229.29M/s | -1 | ok |
| task-spawn-await |  | cpp | std::thread | 20000 | 328.233 | 341.332 | 60.93K/s | -1 | ok |
| atomic-fetchadd |  | go | atomic.Int64 | 1000000 | 3.910 | 3.912 | 255.74M/s | -1 | ok |
| task-spawn-await |  | go | goroutine | 20000 | 5.059 | 5.405 | 3.95M/s | -1 | ok |
| atomic-fetchadd |  | python | atomic | 1000000 |  |  |  | -1 | skipped |
| task-spawn-await |  | python | asyncio | 20000 | 67.578 | 69.075 | 295.95K/s | -1 | ok |
| atomic-fetchadd |  | java | AtomicLong | 1000000 | 3.906 | 3.908 | 255.98M/s | -1 | ok |
| task-spawn-await |  | java | virtual-threads | 20000 | 37.321 | 42.672 | 535.89K/s | -1 | ok |
