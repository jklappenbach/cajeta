# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 2.898 | 3.149 | 217.91M/s | 620 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 7.714 | 8.197 | 223.92M/s | 2072 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 18.148 | 19.032 | 124.04M/s | 3768 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.273 | 0.293 | 2.32G/s | 2852 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.661 | 0.715 | 2.61G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 1.376 | 1.430 | 1.64G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 20.457 | 20.734 | 84.43M/s | 479368 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 29.913 | 31.371 | 57.74M/s | 595212 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 63.995 | 70.719 | 26.99M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.047 | 0.050 | 298.78K/s | 864 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 19.514 | 19.972 | 53.73M/s | 136556 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 13.639 | 22.024 | 76.88M/s | 102420 | ok |
| json-dom |  | rust | serde_json | 631514 | 1.784 | 1.810 | 353.90M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 1.373 | 1.389 | 460.07M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 3.807 | 4.624 | 453.66M/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 3.156 | 3.396 | 547.27M/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 6.580 | 6.941 | 342.10M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 8.775 | 8.972 | 256.52M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.160 | 0.162 | 3.96G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.263 | 0.267 | 2.40G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.445 | 0.451 | 3.88G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.617 | 0.683 | 2.80G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 2.185 | 2.195 | 1.03G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 2.470 | 2.503 | 911.51M/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 6.966 | 7.857 | 90.66M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 3.056 | 6.036 | 206.62M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 15.156 | 20.362 | 113.96M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 7.425 | 9.152 | 232.63M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 30.216 | 36.094 | 74.50M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 25.845 | 31.380 | 87.10M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 3.919 | 3.976 | 161.12M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 1.425 | 1.438 | 443.02M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 1.875 | 1.967 | 336.87M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 2.366 | 2.449 | 266.89M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 10.643 | 12.661 | 162.29M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 4.486 | 4.784 | 385.05M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 6.358 | 7.372 | 271.67M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 8.013 | 9.048 | 215.55M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 53.600 | 54.819 | 42.00M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 11.709 | 12.349 | 192.24M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 13.522 | 14.806 | 166.47M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 20.188 | 21.571 | 111.50M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 631514 | 1.873 | 1.981 | 337.10M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 1727204 | 3.144 | 3.206 | 549.32M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 2251051 | 28.866 | 31.848 | 77.98M/s | -1 | ok |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 1.225 | 1.236 | 81.63M/s | 45368 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 2.914 | 2.974 | 17.16M/s | 69128 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 17.018 | 18.088 | 1.76M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 3.829 | 3.891 | 26.12M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 1.491 | 1.511 | 26.83M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 5.113 | 5.309 | 9.78M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 3.854 | 3.938 | 5.19M/s | 25016 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 1.489 | 1.586 | 33.57M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.493 | 0.519 | 101.42M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 7.994 | 10.362 | 3.75M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 7.801 | 9.325 | 3.85M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.917 | 2.248 | 52.16M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.067 | 0.068 | 1.50G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 1.450 | 1.508 | 34.49M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.284 | 0.319 | 175.86M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 3.962 | 3.984 | 7.57M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.983 | 2.058 | 15.13M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 1.656 | 1.766 | 60.37M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.062 | 0.062 | 1.61G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 1.451 | 2.658 | 34.46M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 5.447 | 8.146 | 5.51M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 3.089 | 4.291 | 32.38M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.267 | 1.171 | 373.86M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 4.772 | 7.517 | 10.48M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 12.334 | 12.803 | 2.43M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 6.993 | 7.197 | 14.30M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 3.183 | 3.193 | 31.42M/s | -1 | ok |
| hashmap-int |  | java | HashMap | 50000 | 0.707 | 2.117 | 70.73M/s | -1 | ok |
| hashmap-string |  | java | HashMap | 30000 | 3.029 | 4.782 | 9.90M/s | -1 | ok |
| hashset-dedup |  | java | HashSet | 100000 | 1.693 | 1.838 | 59.08M/s | -1 | ok |
| arraylist-append |  | java | ArrayList | 100000 | 1.012 | 1.183 | 98.83M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 3.593 | 4.216 | 13.91M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | stdlib | 50000 | 4.396 | 4.611 | 11.37M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 4.506 | 4.615 | 11.10M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 7.477 | 7.537 | 6.69M/s | 11736 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.563 | 0.580 | 88.74M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.013 | 0.013 | 3.86G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.023 | 0.023 | 2.22G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.152 | 0.159 | 329.39M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.854 | 0.904 | 58.57M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 1.119 | 1.137 | 44.70M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 2.330 | 2.352 | 21.46M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 1.041 | 1.061 | 48.03M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.382 | 0.391 | 130.89M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.031 | 0.033 | 1.59G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.253 | 0.258 | 197.87M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.065 | 0.071 | 774.57M/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.745 | 0.761 | 67.14M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.130 | 0.136 | 383.45M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.627 | 2.637 | 19.03M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.791 | 2.803 | 17.91M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.934 | 2.980 | 17.04M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.031 | 0.038 | 1.62G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.043 | 0.048 | 1.16G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.182 | 0.185 | 274.15M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 9.667 | 10.308 | 5.17M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 3.454 | 3.619 | 14.47M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 13.167 | 13.834 | 3.80M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.198 | 0.208 | 252.41M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.207 | 0.216 | 241.86M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 1.583 | 1.629 | 31.58M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 14.654 | 15.003 | 3.41M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 6.675 | 6.831 | 7.49M/s | -1 | ok |
| sort-int64 | random | java | Arrays.sort | 50000 | 2.591 | 3.343 | 19.30M/s | -1 | ok |
| sort-int64 | ascending | java | Arrays.sort | 50000 | 0.101 | 0.112 | 496.67M/s | -1 | ok |
| sort-int64 | descending | java | Arrays.sort | 50000 | 0.164 | 0.195 | 304.47M/s | -1 | ok |
| sort-int64 | dups | java | Arrays.sort | 50000 | 0.580 | 0.775 | 86.16M/s | -1 | ok |
| sort-stable-int64 |  | java | Arrays.sort(Timsort) | 50000 | 7.810 | 12.518 | 6.40M/s | -1 | ok |
| sort-f64 |  | java | Arrays.sort | 50000 | 0.507 | 0.562 | 98.64M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 15.268 | 19.396 | 261.98K/s | 1891392 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.281 | 0.294 | 1.28G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 1.485 | 1.516 | 242.69M/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.492 | 0.499 | 733.06M/s | 17604 | ok |
| string-search |  | rust | std-find | 360448 | 0.035 | 0.035 | 10.37G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.008 | 0.008 | 46.85G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.202 | 0.209 | 1.79G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.015 | 0.015 | 23.95G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.001 | 0.001 | 6.88G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.038 | 0.040 | 9.51G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.157 | 0.159 | 2.29G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.727 | 0.736 | 495.89M/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.003 | 0.003 | 1.51G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.048 | 0.048 | 7.58G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.241 | 0.317 | 1.50G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.859 | 0.946 | 419.77M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.002 | 0.003 | 1.74G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.052 | 0.052 | 6.91G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.456 | 0.459 | 790.96M/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.131 | 0.133 | 2.76G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.019 | 0.021 | 205.27M/s | -1 | ok |
| string-search |  | java | String.indexOf | 360448 | 0.181 | 0.188 | 1.99G/s | -1 | ok |
| string-replace |  | java | String.replace | 360448 | 1.155 | 1.594 | 312.11M/s | -1 | ok |
| string-uppercase |  | java | String.toUpperCase | 360448 | 0.337 | 0.380 | 1.07G/s | -1 | ok |
| string-build-concat |  | java | StringBuilder | 4000 | 0.030 | 0.041 | 134.07M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.031 | 0.032 | 34.21G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.323 | 0.329 | 3.25G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 3.262 | 3.462 | 321.47M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 2.150 | 2.179 | 487.73M/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.030 | 0.034 | 35.20G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.484 | 0.487 | 2.17G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.510 | 1.538 | 694.47M/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.010 | 0.011 | 102.81G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.484 | 0.488 | 2.17G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 1.103 | 1.110 | 950.99M/s | -1 | ok |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.019 | 0.019 | 56.27G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.484 | 0.490 | 2.17G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 1.107 | 1.109 | 946.95M/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.032 | 0.032 | 33.08G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.488 | 0.491 | 2.15G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 1.108 | 1.110 | 945.96M/s | -1 | ok |
| xxhash3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| sha256 |  | java | java.security.MessageDigest | 1048576 | 0.575 | 0.609 | 1.82G/s | -1 | ok |
| md5 |  | java | java.security.MessageDigest | 1048576 | 1.167 | 1.171 | 898.81M/s | -1 | ok |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | java | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 96.296 | 103.168 | 10.38M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 40.613 | 44.494 | 24.62M/s | 0 | ok |
| stream-filter-map-reduce |  | rust | Iterator | 1000000 | 0.657 | 0.662 | 1.52G/s | -1 | ok |
| stream-parallel-reduce |  | rust | rayon | 1000000 | 0.141 | 0.568 | 7.10G/s | -1 | ok |
| stream-filter-map-reduce |  | cpp | hand-loop | 1000000 | 0.080 | 0.117 | 12.52G/s | -1 | ok |
| stream-parallel-reduce |  | cpp | OpenMP | 1000000 | 0.309 | 0.500 | 3.24G/s | -1 | ok |
| stream-filter-map-reduce |  | go | hand-loop | 1000000 | 0.655 | 0.675 | 1.53G/s | -1 | ok |
| stream-parallel-reduce |  | go | goroutines | 1000000 | 0.224 | 0.606 | 4.46G/s | -1 | ok |
| stream-filter-map-reduce |  | python | genexpr | 1000000 | 41.669 | 43.773 | 24.00M/s | -1 | ok |
| stream-parallel-reduce |  | python | sum (GIL) | 1000000 | 5.752 | 5.980 | 173.85M/s | -1 | ok |
| stream-filter-map-reduce |  | java | java.util.stream | 1000000 | 0.392 | 0.424 | 2.55G/s | -1 | ok |
| stream-parallel-reduce |  | java | parallel-stream | 1000000 | 0.899 | 1.307 | 1.11G/s | -1 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.807 | 0.903 | 1.24G/s | 0 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.432 | 0.437 | 2.32G/s | 0 | ok |
| matmul |  | cajeta | stdlib | 40000 | 4.000 | 4.060 | 10.00M/s | 0 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.923 | 1.054 | 1.08G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.460 | 0.466 | 2.18G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 3.165 | 3.195 | 12.64M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.742 | 0.757 | 1.35G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.699 | 0.703 | 1.43G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.918 | 2.935 | 13.71M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.596 | 0.719 | 1.68G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.535 | 0.582 | 1.87G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 6.026 | 6.243 | 6.64M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.708 | 0.786 | 1.41G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 5.988 | 14.984 | 167.01M/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 7.011 | 12.030 | 5.71M/s | -1 | ok |
| saxpy |  | java | scalar | 1000000 | 0.525 | 2.136 | 1.90G/s | -1 | ok |
| dot-product |  | java | scalar | 1000000 | 0.439 | 1.617 | 2.28G/s | -1 | ok |
| matmul |  | java | scalar | 40000 | 2.552 | 3.221 | 15.67M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 34.310 | 34.578 | 18.65M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 187.026 | 194.321 | 53/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 7.013 | 7.364 | 14.26K/s | 0 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 31.619 | 31.830 | 20.24M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 187.741 | 188.987 | 53/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.382 | 0.382 | 261.93K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 27.774 | 27.835 | 23.04M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 199.018 | 201.391 | 50/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.160 | 0.160 | 625.35K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 33.893 | 33.985 | 18.88M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 195.466 | 196.461 | 51/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.498 | 0.505 | 200.99K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 276.659 | 286.618 | 2.31M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 8674.069 | 9446.706 | 1/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 80.317 | 82.352 | 1.25K/s | -1 | ok |
| clbg-mandelbrot |  | java | stdlib | 640000 | 28.690 | 29.356 | 22.31M/s | -1 | ok |
| clbg-fannkuch-redux |  | java | stdlib | 10 | 162.317 | 175.712 | 62/s | -1 | ok |
| clbg-spectral-norm |  | java | stdlib | 100 | 0.361 | 1.772 | 277.10K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 89.623 | 98.022 | 11.16M/s | 0 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 8.808 | 9.634 | 11.35M/s | 0 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 12.618 | 12.844 | 79.25M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.968 | 0.979 | 103.26M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.170 | 0.173 | 5.90G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.025 | 0.026 | 3.95G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 6.013 | 6.175 | 166.31M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 2.994 | 3.015 | 33.40M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 801.595 | 818.399 | 1.25M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 37.880 | 38.336 | 2.64M/s | -1 | ok |
| time-instant-arith |  | java | java.time | 1000000 | 0.253 | 0.259 | 3.95G/s | -1 | ok |
| time-localdate-arith |  | java | java.time | 100000 | 1.261 | 1.950 | 79.28M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 17.247 | 17.970 | 57.98M/s | 0 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 435.976 | 457.308 | 45.87K/s | 0 | ok |
| atomic-fetchadd |  | rust | AtomicI64 | 1000000 | 4.911 | 5.020 | 203.64M/s | -1 | ok |
| task-spawn-await |  | rust | std::thread | 20000 | 480.305 | 623.537 | 41.64K/s | -1 | ok |
| atomic-fetchadd |  | cpp | std::atomic | 1000000 | 4.593 | 4.629 | 217.72M/s | -1 | ok |
| task-spawn-await |  | cpp | std::thread | 20000 | 398.346 | 467.531 | 50.21K/s | -1 | ok |
| atomic-fetchadd |  | go | atomic.Int64 | 1000000 | 4.738 | 4.778 | 211.06M/s | -1 | ok |
| task-spawn-await |  | go | goroutine | 20000 | 8.591 | 9.083 | 2.33M/s | -1 | ok |
| atomic-fetchadd |  | python | atomic | 1000000 |  |  |  | -1 | skipped |
| task-spawn-await |  | python | asyncio | 20000 | 130.995 | 132.065 | 152.68K/s | -1 | ok |
| atomic-fetchadd |  | java | AtomicLong | 1000000 | 4.311 | 4.346 | 231.95M/s | -1 | ok |
| task-spawn-await |  | java | virtual-threads | 20000 | 54.098 | 55.042 | 369.70K/s | -1 | ok |
