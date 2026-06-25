# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## Languages

| Language | Version |
|---|---|
| cajeta ★ | — |
| rust | 1.91.1 |
| cpp | g++ 15.2.0 |
| go | 1.26.0 |
| python | 3.12.9 |
| java | 25.0.1 |

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 0.589 | 0.619 | 1.07G/s | 620 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 1.412 | 1.437 | 1.22G/s | 2120 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 2.519 | 2.528 | 893.48M/s | 3700 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.167 | 0.168 | 3.79G/s | 2804 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.415 | 0.417 | 4.16G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.787 | 0.792 | 2.86G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 6.607 | 6.901 | 261.42M/s | 479400 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 2.408 | 2.909 | 717.40M/s | 595216 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 15.110 | 15.597 | 114.31M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.014 | 0.015 | 1.01M/s | 848 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 1.171 | 1.179 | 895.19M/s | 136560 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 1.167 | 1.181 | 898.88M/s | 102416 | ok |
| json-dom |  | rust | serde_json | 631514 | 1.002 | 1.012 | 630.37M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.720 | 0.729 | 877.11M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 1.637 | 1.647 | 1.06G/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.625 | 1.633 | 1.06G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 3.468 | 3.493 | 649.09M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 3.760 | 3.797 | 598.65M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.094 | 0.098 | 6.68G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.131 | 0.132 | 4.83G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.255 | 0.257 | 6.78G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.351 | 0.362 | 4.92G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.390 | 1.402 | 1.62G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.522 | 1.527 | 1.48G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.322 | 3.988 | 190.11M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 1.978 | 2.584 | 319.20M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 9.369 | 9.945 | 184.35M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 4.343 | 5.785 | 397.72M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 15.238 | 16.405 | 147.72M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 13.545 | 15.104 | 166.19M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.736 | 1.753 | 363.79M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.673 | 0.685 | 938.03M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.897 | 0.903 | 704.38M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.136 | 1.151 | 556.09M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 4.164 | 4.198 | 414.80M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.096 | 2.115 | 824.12M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.691 | 2.729 | 641.85M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 3.013 | 3.098 | 573.21M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 31.981 | 32.904 | 70.39M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.580 | 5.890 | 403.42M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 5.976 | 6.029 | 376.69M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.728 | 8.785 | 257.90M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 631514 | 1.117 | 1.188 | 565.40M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 1727204 | 1.634 | 2.143 | 1.06G/s | -1 | ok |
| json-dom |  | java | jackson-databind | 2251051 | 15.544 | 16.935 | 144.82M/s | -1 | ok |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 0.174 | 0.179 | 574.32M/s | 45360 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 0.452 | 0.461 | 110.71M/s | 34572 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 6.266 | 6.387 | 4.79M/s | 267936 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 1.019 | 1.030 | 98.16M/s | 46108 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 0.710 | 0.716 | 56.30M/s | 56264 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 3.524 | 3.568 | 14.19M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 1.949 | 2.000 | 10.26M/s | 25016 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 0.943 | 0.950 | 53.02M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.313 | 0.316 | 159.93M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.728 | 3.751 | 8.05M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 3.667 | 3.706 | 8.18M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.464 | 1.474 | 68.29M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.034 | 0.034 | 2.93G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.807 | 0.815 | 61.93M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.176 | 0.184 | 284.75M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.771 | 2.807 | 10.82M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.481 | 1.538 | 20.26M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.988 | 0.989 | 101.18M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.033 | 0.035 | 3.02G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.788 | 1.012 | 63.44M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.560 | 3.781 | 11.72M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.683 | 1.753 | 59.42M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.264 | 0.414 | 378.59M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.422 | 3.751 | 14.61M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.214 | 6.248 | 4.83M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.866 | 3.886 | 25.87M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.753 | 1.816 | 57.04M/s | -1 | ok |
| hashmap-int |  | java | HashMap | 50000 | 0.345 | 1.619 | 145.04M/s | -1 | ok |
| hashmap-string |  | java | HashMap | 30000 | 1.589 | 2.744 | 18.88M/s | -1 | ok |
| hashset-dedup |  | java | HashSet | 100000 | 1.021 | 1.346 | 97.97M/s | -1 | ok |
| arraylist-append |  | java | ArrayList | 100000 | 0.573 | 0.900 | 174.64M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 1.840 | 1.851 | 27.17M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | 50000 | 0.207 | 0.210 | 241.28M/s | 11752 | ok |
| sort-int64 | descending | cajeta | stdlib | 50000 | 0.213 | 0.216 | 234.97M/s | 11752 | ok |
| sort-int64 | dups | cajeta | stdlib | 50000 | 0.789 | 0.795 | 63.40M/s | 11752 | ok |
| sort-f64 |  | cajeta | stdlib | 50000 | 2.333 | 2.343 | 21.44M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 1.674 | 1.685 | 29.86M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 3.583 | 3.590 | 13.95M/s | 11736 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.388 | 0.400 | 128.73M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.010 | 0.010 | 5.09G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.012 | 0.012 | 4.09G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.103 | 0.104 | 483.87M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.574 | 0.579 | 87.09M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.502 | 0.514 | 99.55M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.808 | 1.835 | 27.65M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.725 | 0.728 | 68.93M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.226 | 0.226 | 221.72M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.55G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.145 | 0.145 | 344.94M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.041 | 1.21G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.490 | 0.496 | 102.08M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.077 | 0.078 | 645.36M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.035 | 2.054 | 24.57M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.229 | 2.252 | 22.43M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.271 | 2.681 | 22.02M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.014 | 0.015 | 3.55G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.024 | 0.036 | 2.06G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.092 | 0.111 | 545.06M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.082 | 6.140 | 8.22M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.769 | 2.789 | 18.06M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.413 | 7.512 | 6.74M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.102 | 0.106 | 490.62M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.110 | 0.111 | 453.11M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.824 | 0.829 | 60.69M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.644 | 7.720 | 6.54M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.671 | 4.722 | 10.71M/s | -1 | ok |
| sort-int64 | random | java | Arrays.sort | 50000 | 0.305 | 0.351 | 163.93M/s | -1 | ok |
| sort-int64 | ascending | java | Arrays.sort | 50000 | 0.036 | 0.058 | 1.39G/s | -1 | ok |
| sort-int64 | descending | java | Arrays.sort | 50000 | 0.090 | 0.090 | 555.25M/s | -1 | ok |
| sort-int64 | dups | java | Arrays.sort | 50000 | 0.416 | 0.503 | 120.22M/s | -1 | ok |
| sort-stable-int64 |  | java | Arrays.sort(Timsort) | 50000 | 5.678 | 13.836 | 8.81M/s | -1 | ok |
| sort-f64 |  | java | Arrays.sort | 50000 | 0.310 | 0.362 | 161.46M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 0.015 | 0.017 | 261.99M/s | 2880 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.004 | 0.004 | 81.40G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 0.144 | 0.145 | 2.50G/s | 18804 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.048 | 0.049 | 7.57G/s | 17600 | ok |
| string-search |  | rust | std-find | 360448 | 0.025 | 0.036 | 14.70G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.005 | 0.005 | 70.41G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.110 | 0.112 | 3.28G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.063 | 0.064 | 5.70G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 12.46G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.030 | 0.030 | 12.13G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.096 | 0.097 | 3.77G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.354 | 0.357 | 1.02G/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.51G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.047 | 0.048 | 7.59G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.123 | 0.189 | 2.93G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.382 | 0.438 | 942.82M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.002 | 3.44G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.044 | 0.044 | 8.18G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.335 | 0.338 | 1.08G/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.071 | 0.071 | 5.11G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.010 | 0.010 | 409.92M/s | -1 | ok |
| string-search |  | java | String.indexOf | 360448 | 0.061 | 0.526 | 5.89G/s | -1 | ok |
| string-replace |  | java | String.replace | 360448 | 0.623 | 0.637 | 578.68M/s | -1 | ok |
| string-uppercase |  | java | String.toUpperCase | 360448 | 0.215 | 0.229 | 1.67G/s | -1 | ok |
| string-build-concat |  | java | StringBuilder | 4000 | 0.017 | 0.020 | 230.37M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.008 | 0.008 | 130.18G/s | 72 | ok |
| xxhash3_128 |  | cajeta | stdlib | 1048576 | 0.008 | 0.008 | 128.25G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.289 | 0.295 | 3.63G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 0.425 | 0.427 | 2.47G/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.375 | 1.397 | 762.76M/s | 0 | ok |
| blake3 |  | cajeta | stdlib | 1048576 | 0.268 | 0.273 | 3.91G/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.015 | 0.016 | 71.49G/s | -1 | ok |
| xxhash3_128 |  | rust | xxhash-rust | 1048576 | 0.014 | 0.015 | 72.78G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.464 | 0.469 | 2.26G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.266 | 1.269 | 828.20M/s | -1 | ok |
| blake3 |  | rust | blake3 | 1048576 | 0.097 | 0.098 | 10.76G/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.010 | 0.011 | 102.31G/s | -1 | ok |
| xxhash3_128 |  | cpp | xxHash | 1048576 | 0.010 | 0.010 | 107.46G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.468 | 0.472 | 2.24G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 1.063 | 1.068 | 986.53M/s | -1 | ok |
| blake3 |  | cpp | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.017 | 0.017 | 60.29G/s | -1 | ok |
| xxhash3_128 |  | go | zeebo/xxh3 | 1048576 | 0.017 | 0.017 | 60.19G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.463 | 0.464 | 2.26G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 1.058 | 1.061 | 990.87M/s | -1 | ok |
| blake3 |  | go | lukechampine/blake3 | 1048576 | 0.316 | 0.344 | 3.31G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.028 | 0.030 | 37.07G/s | -1 | ok |
| xxhash3_128 |  | python | xxhash | 1048576 | 0.028 | 0.029 | 36.81G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.462 | 0.465 | 2.27G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 1.052 | 1.056 | 996.48M/s | -1 | ok |
| blake3 |  | python | blake3 | 1048576 |  |  |  | -1 | skipped |
| xxhash3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3_128 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| sha256 |  | java | java.security.MessageDigest | 1048576 | 0.530 | 0.564 | 1.98G/s | -1 | ok |
| md5 |  | java | java.security.MessageDigest | 1048576 | 1.085 | 1.087 | 966.11M/s | -1 | ok |
| blake3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | java | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 22.237 | 22.856 | 44.97M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 10.540 | 11.277 | 94.87M/s | 4 | ok |
| stream-filter-map-reduce |  | rust | Iterator | 1000000 | 0.648 | 0.651 | 1.54G/s | -1 | ok |
| stream-parallel-reduce |  | rust | rayon | 1000000 | 0.079 | 0.099 | 12.61G/s | -1 | ok |
| stream-filter-map-reduce |  | cpp | hand-loop | 1000000 | 0.064 | 0.069 | 15.57G/s | -1 | ok |
| stream-parallel-reduce |  | cpp | OpenMP | 1000000 | 0.025 | 0.025 | 40.56G/s | -1 | ok |
| stream-filter-map-reduce |  | go | hand-loop | 1000000 | 0.586 | 0.589 | 1.71G/s | -1 | ok |
| stream-parallel-reduce |  | go | goroutines | 1000000 | 0.049 | 0.065 | 20.37G/s | -1 | ok |
| stream-filter-map-reduce |  | python | genexpr | 1000000 | 19.018 | 19.164 | 52.58M/s | -1 | ok |
| stream-parallel-reduce |  | python | sum (GIL) | 1000000 | 3.698 | 3.718 | 270.45M/s | -1 | ok |
| stream-filter-map-reduce |  | java | java.util.stream | 1000000 | 0.161 | 0.165 | 6.20G/s | -1 | ok |
| stream-parallel-reduce |  | java | parallel-stream | 1000000 | 0.103 | 0.115 | 9.72G/s | -1 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.601 | 0.614 | 1.66G/s | 16 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.390 | 0.393 | 2.56G/s | 16 | ok |
| matmul |  | cajeta | stdlib | 40000 | 0.446 | 0.447 | 89.79M/s | 16 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.637 | 0.722 | 1.57G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.391 | 0.396 | 2.56G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.257 | 2.261 | 17.72M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.713 | 0.717 | 1.40G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.641 | 0.644 | 1.56G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.460 | 2.467 | 16.26M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.403 | 0.410 | 2.48G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.392 | 0.395 | 2.55G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 2.977 | 3.098 | 13.44M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.408 | 0.412 | 2.45G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 0.010 | 0.011 | 97.47G/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.074 | 0.077 | 539.96M/s | -1 | ok |
| saxpy |  | java | scalar | 1000000 | 1.057 | 1.109 | 946.44M/s | -1 | ok |
| dot-product |  | java | scalar | 1000000 | 0.557 | 0.819 | 1.80G/s | -1 | ok |
| matmul |  | java | scalar | 40000 | 2.279 | 2.290 | 17.55M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.735 | 28.773 | 22.27M/s | 16 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 145.552 | 147.040 | 69/s | 16 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 1.632 | 1.638 | 61.27K/s | 88 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 25.600 | 25.642 | 25.00M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 156.945 | 160.313 | 64/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.206 | 0.206 | 486.39K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.613 | 23.668 | 27.10M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 156.636 | 157.078 | 64/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.137 | 0.137 | 732.03K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 23.759 | 23.780 | 26.94M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 147.943 | 149.185 | 68/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.313 | 0.331 | 319.79K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 111.300 | 113.666 | 5.75M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4219.063 | 4296.064 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 37.400 | 37.457 | 2.67K/s | -1 | ok |
| clbg-mandelbrot |  | java | stdlib | 640000 | 26.070 | 26.677 | 24.55M/s | -1 | ok |
| clbg-fannkuch-redux |  | java | stdlib | 10 | 157.633 | 157.919 | 63/s | -1 | ok |
| clbg-spectral-norm |  | java | stdlib | 100 | 0.322 | 1.189 | 310.59K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 20.729 | 20.757 | 48.24M/s | 20 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 2.032 | 2.035 | 49.21M/s | 16 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 6.626 | 6.725 | 150.93M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.539 | 0.545 | 185.45M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.113 | 0.113 | 8.83G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.011 | 0.011 | 8.83G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 2.939 | 2.956 | 340.23M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 1.515 | 1.519 | 66.01M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 346.845 | 347.216 | 2.88M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 17.250 | 17.492 | 5.80M/s | -1 | ok |
| time-instant-arith |  | java | java.time | 1000000 | 0.207 | 0.208 | 4.83G/s | -1 | ok |
| time-localdate-arith |  | java | java.time | 100000 | 0.811 | 1.151 | 123.27M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 5.278 | 5.284 | 189.46M/s | 20 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 85.427 | 86.483 | 234.12K/s | 16 | ok |
| atomic-fetchadd |  | rust | AtomicI64 | 1000000 | 3.910 | 4.320 | 255.78M/s | -1 | ok |
| task-spawn-await |  | rust | std::thread | 20000 | 415.653 | 421.178 | 48.12K/s | -1 | ok |
| atomic-fetchadd |  | cpp | std::atomic | 1000000 | 3.908 | 3.911 | 255.88M/s | -1 | ok |
| task-spawn-await |  | cpp | std::thread | 20000 | 323.148 | 331.335 | 61.89K/s | -1 | ok |
| atomic-fetchadd |  | go | atomic.Int64 | 1000000 | 3.904 | 3.910 | 256.13M/s | -1 | ok |
| task-spawn-await |  | go | goroutine | 20000 | 4.830 | 5.362 | 4.14M/s | -1 | ok |
| atomic-fetchadd |  | python | atomic | 1000000 |  |  |  | -1 | skipped |
| task-spawn-await |  | python | asyncio | 20000 | 68.973 | 69.390 | 289.97K/s | -1 | ok |
| atomic-fetchadd |  | java | AtomicLong | 1000000 | 3.909 | 3.913 | 255.82M/s | -1 | ok |
| task-spawn-await |  | java | virtual-threads | 20000 | 35.245 | 36.560 | 567.45K/s | -1 | ok |
