# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566296

## Languages

| Language | Version |
|---|---|
| cajeta ★ | 0.7.1 (d6fdde37) |
| rust | 1.91.1 |
| cpp | g++ 15.2.0 |
| go | 1.26.0 |
| python | 3.12.9 |
| java | 25.0.1 |

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 0.604 | 0.613 | 1.05G/s | 404 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 1.509 | 1.517 | 1.14G/s | 2116 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 2.482 | 2.497 | 906.85M/s | 3708 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.173 | 0.174 | 3.66G/s | 2812 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.451 | 0.455 | 3.83G/s | 0 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.800 | 0.805 | 2.81G/s | 0 | ok |
| json-dom | twitter | cajeta | stdlib | 631514 | 2.330 | 2.662 | 271.02M/s | 174212 | ok |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 6.532 | 6.829 | 264.43M/s | 584184 | ok |
| json-dom | canada | cajeta | stdlib | 2251051 | 16.731 | 17.276 | 134.54M/s | 1668716 | ok |
| json-serialize | twitter | cajeta | stdlib | 631514 | 1.057 | 1.137 | 597.44M/s | 237388 | ok |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 2.507 | 2.985 | 688.86M/s | 693212 | ok |
| json-serialize | canada | cajeta | stdlib | 2251051 | 9.715 | 10.961 | 231.70M/s | 1806168 | ok |
| json-roundtrip | twitter | cajeta | stdlib | 631514 | 6.015 | 6.191 | 104.99M/s | 411732 | ok |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 15.147 | 15.951 | 114.03M/s | 1277448 | ok |
| json-roundtrip | canada | cajeta | stdlib | 2251051 | 42.472 | 44.751 | 53.00M/s | 3408924 | ok |
| json-conformance |  | cajeta | stdlib | 14 | 0.015 | 0.018 | 929.06K/s | 892 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 0.902 | 0.916 | 1.16G/s | 136560 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 1.163 | 1.175 | 901.56M/s | 102416 | ok |
| json-dom |  | rust | serde_json | 631514 | 0.982 | 1.001 | 643.17M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.728 | 0.758 | 867.02M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 1.609 | 1.639 | 1.07G/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.627 | 1.640 | 1.06G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 3.541 | 3.560 | 635.78M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 3.791 | 3.817 | 593.75M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.083 | 0.087 | 7.63G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.113 | 0.114 | 5.57G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.231 | 0.232 | 7.48G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.307 | 0.310 | 5.63G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.234 | 1.239 | 1.82G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.324 | 1.343 | 1.70G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.442 | 3.896 | 183.49M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.326 | 2.661 | 271.50M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 8.489 | 9.707 | 203.47M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 5.252 | 6.145 | 328.88M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 15.396 | 17.004 | 146.21M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 12.933 | 15.419 | 174.05M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.758 | 1.764 | 359.31M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.667 | 0.689 | 946.84M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.916 | 0.934 | 689.53M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.139 | 1.155 | 554.29M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 4.039 | 4.074 | 427.67M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.072 | 2.095 | 833.51M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.741 | 2.804 | 630.18M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 3.003 | 3.056 | 575.21M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 31.495 | 31.681 | 71.47M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.642 | 5.777 | 399.00M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 6.047 | 6.152 | 372.26M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.768 | 8.859 | 256.74M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 631514 | 1.168 | 1.205 | 540.61M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 1727204 | 1.676 | 2.081 | 1.03G/s | -1 | ok |
| json-dom |  | java | jackson-databind | 2251051 | 15.523 | 17.842 | 145.01M/s | -1 | ok |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 0.178 | 0.180 | 563.11M/s | 45356 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 0.465 | 0.481 | 107.46M/s | 38420 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 2.099 | 2.178 | 14.29M/s | 104892 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 1.238 | 1.302 | 80.76M/s | 76820 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 0.637 | 0.653 | 62.76M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 3.987 | 4.038 | 12.54M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 1.920 | 2.050 | 10.42M/s | 25012 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 1.060 | 1.074 | 47.17M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.349 | 0.353 | 143.47M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.778 | 3.806 | 7.94M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 3.632 | 3.693 | 8.26M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.470 | 1.480 | 68.04M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.034 | 0.034 | 2.94G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.910 | 0.927 | 54.94M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.209 | 0.268 | 239.75M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 3.067 | 3.079 | 9.78M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.690 | 1.749 | 17.75M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.974 | 1.129 | 102.68M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.034 | 0.035 | 2.95G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.767 | 0.975 | 65.22M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.477 | 3.357 | 12.11M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.716 | 2.237 | 58.26M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.360 | 0.555 | 277.55M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.736 | 3.778 | 13.38M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.272 | 6.285 | 4.78M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.875 | 3.893 | 25.81M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.780 | 1.841 | 56.19M/s | -1 | ok |
| hashmap-int |  | java | HashMap | 50000 | 0.336 | 1.415 | 148.88M/s | -1 | ok |
| hashmap-string |  | java | HashMap | 30000 | 1.376 | 2.498 | 21.81M/s | -1 | ok |
| hashset-dedup |  | java | HashSet | 100000 | 1.038 | 1.350 | 96.30M/s | -1 | ok |
| arraylist-append |  | java | ArrayList | 100000 | 0.570 | 0.883 | 175.31M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 0.559 | 0.569 | 89.41M/s | 11736 | ok |
| sort-int64 | ascending | cajeta | stdlib | 50000 | 0.063 | 0.065 | 797.77M/s | 11736 | ok |
| sort-int64 | descending | cajeta | stdlib | 50000 | 0.073 | 0.075 | 687.84M/s | 11736 | ok |
| sort-int64 | dups | cajeta | stdlib | 50000 | 0.131 | 0.134 | 382.58M/s | 11736 | ok |
| sort-f64 |  | cajeta | stdlib | 50000 | 0.468 | 0.479 | 106.89M/s | 11736 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 1.499 | 1.524 | 33.35M/s | 23456 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 3.476 | 3.513 | 14.39M/s | 16 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.410 | 0.419 | 122.03M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.011 | 0.011 | 4.74G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.013 | 0.013 | 3.84G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.111 | 0.112 | 448.67M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.623 | 0.633 | 80.29M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.550 | 0.554 | 90.91M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.940 | 1.954 | 25.77M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.731 | 0.763 | 68.35M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.238 | 0.239 | 209.82M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.55G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.152 | 0.153 | 328.13M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.041 | 1.22G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.486 | 0.490 | 102.89M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.077 | 0.077 | 648.88M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.020 | 2.036 | 24.75M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.202 | 2.232 | 22.70M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.266 | 2.274 | 22.07M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.015 | 0.024 | 3.39G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.021 | 0.024 | 2.33G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.076 | 0.089 | 661.53M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.100 | 6.702 | 8.20M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.755 | 2.759 | 18.15M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.432 | 7.497 | 6.73M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.101 | 0.107 | 493.19M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.108 | 0.111 | 463.64M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.798 | 0.803 | 62.68M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.608 | 7.639 | 6.57M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.617 | 4.669 | 10.83M/s | -1 | ok |
| sort-int64 | random | java | Arrays.sort | 50000 | 1.811 | 2.177 | 27.61M/s | -1 | ok |
| sort-int64 | ascending | java | Arrays.sort | 50000 | 0.036 | 0.058 | 1.37G/s | -1 | ok |
| sort-int64 | descending | java | Arrays.sort | 50000 | 0.090 | 0.090 | 555.31M/s | -1 | ok |
| sort-int64 | dups | java | Arrays.sort | 50000 | 0.455 | 0.478 | 109.78M/s | -1 | ok |
| sort-stable-int64 |  | java | Arrays.sort(Timsort) | 50000 | 4.878 | 15.954 | 10.25M/s | -1 | ok |
| sort-f64 |  | java | Arrays.sort | 50000 | 0.308 | 0.348 | 162.58M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 0.002 | 0.002 | 1.88G/s | 380 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.004 | 0.004 | 87.34G/s | 16 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 0.142 | 0.145 | 2.53G/s | 18820 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.048 | 0.050 | 7.49G/s | 17620 | ok |
| string-search |  | rust | std-find | 360448 | 0.026 | 0.026 | 14.00G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.005 | 0.005 | 67.25G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.114 | 0.115 | 3.16G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.071 | 0.071 | 5.08G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 11.76G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.030 | 0.030 | 12.08G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.096 | 0.096 | 3.76G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.355 | 0.360 | 1.02G/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.48G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.049 | 0.049 | 7.33G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.123 | 0.194 | 2.93G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.392 | 0.435 | 919.66M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.004 | 3.25G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.053 | 0.053 | 6.86G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.397 | 0.400 | 907.31M/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.084 | 0.084 | 4.31G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.012 | 0.012 | 334.11M/s | -1 | ok |
| string-search |  | java | String.indexOf | 360448 | 0.061 | 0.567 | 5.90G/s | -1 | ok |
| string-replace |  | java | String.replace | 360448 | 0.455 | 0.918 | 792.08M/s | -1 | ok |
| string-uppercase |  | java | String.toUpperCase | 360448 | 0.281 | 0.286 | 1.28G/s | -1 | ok |
| string-build-concat |  | java | StringBuilder | 4000 | 0.017 | 0.020 | 235.27M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.007 | 0.007 | 156.46G/s | 36 | ok |
| xxhash3-256k |  | cajeta | stdlib | 262144 | 0.002 | 0.002 | 161.52G/s | 56 | ok |
| xxhash3_128 |  | cajeta | stdlib | 1048576 | 0.007 | 0.007 | 158.56G/s | 36 | ok |
| xxhash3_128-256k |  | cajeta | stdlib | 262144 | 0.002 | 0.002 | 159.55G/s | 56 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.246 | 4.26G/s | 16 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 0.415 | 0.416 | 2.53G/s | 20 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.257 | 1.260 | 834.33M/s | 20 | ok |
| blake3 |  | cajeta | stdlib | 1048576 | 0.186 | 0.187 | 5.64G/s | 16 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.012 | 0.012 | 90.77G/s | -1 | ok |
| xxhash3_128 |  | rust | xxhash-rust | 1048576 | 0.012 | 0.012 | 89.00G/s | -1 | ok |
| xxhash3-256k |  | rust | xxhash-rust | 262144 | 0.003 | 0.003 | 90.24G/s | -1 | ok |
| xxhash3_128-256k |  | rust | xxhash-rust | 262144 | 0.003 | 0.003 | 90.52G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.415 | 0.418 | 2.53G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.086 | 1.087 | 965.83M/s | -1 | ok |
| blake3 |  | rust | blake3 | 1048576 | 0.085 | 0.085 | 12.38G/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.007 | 0.007 | 149.52G/s | -1 | ok |
| xxhash3_128 |  | cpp | xxHash | 1048576 | 0.007 | 0.007 | 147.83G/s | -1 | ok |
| xxhash3-256k |  | cpp | xxHash | 262144 | 0.002 | 0.002 | 159.55G/s | -1 | ok |
| xxhash3_128-256k |  | cpp | xxHash | 262144 | 0.002 | 0.002 | 158.59G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.416 | 0.416 | 2.52G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.923 | 0.927 | 1.14G/s | -1 | ok |
| blake3 |  | cpp | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.016 | 0.016 | 67.01G/s | -1 | ok |
| xxhash3_128 |  | go | zeebo/xxh3 | 1048576 | 0.016 | 0.016 | 67.09G/s | -1 | ok |
| xxhash3-256k |  | go | zeebo/xxh3 | 262144 | 0.004 | 0.004 | 66.92G/s | -1 | ok |
| xxhash3_128-256k |  | go | zeebo/xxh3 | 262144 | 0.004 | 0.004 | 66.92G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.416 | 0.417 | 2.52G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.924 | 0.927 | 1.13G/s | -1 | ok |
| blake3 |  | go | lukechampine/blake3 | 1048576 | 0.068 | 0.128 | 15.43G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 45.06G/s | -1 | ok |
| xxhash3_128 |  | python | xxhash | 1048576 | 0.024 | 0.024 | 44.38G/s | -1 | ok |
| xxhash3-256k |  | python | xxhash | 262144 | 0.006 | 0.006 | 43.83G/s | -1 | ok |
| xxhash3_128-256k |  | python | xxhash | 262144 | 0.006 | 0.006 | 43.25G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.416 | 0.416 | 2.52G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.924 | 0.927 | 1.13G/s | -1 | ok |
| blake3 |  | python | blake3 | 1048576 |  |  |  | -1 | skipped |
| xxhash3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3_128 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3-256k |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3_128-256k |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| sha256 |  | java | java.security.MessageDigest | 1048576 | 0.520 | 0.528 | 2.02G/s | -1 | ok |
| md5 |  | java | java.security.MessageDigest | 1048576 | 1.018 | 1.022 | 1.03G/s | -1 | ok |
| blake3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | java | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 23.423 | 23.502 | 42.69M/s | 20 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 9.887 | 10.734 | 101.14M/s | 32 | ok |
| stream-filter-map-reduce |  | rust | Iterator | 1000000 | 0.586 | 0.590 | 1.71G/s | -1 | ok |
| stream-parallel-reduce |  | rust | rayon | 1000000 | 0.071 | 0.103 | 14.08G/s | -1 | ok |
| stream-filter-map-reduce |  | cpp | views::filter|transform | 1000000 | 0.212 | 0.213 | 4.71G/s | -1 | ok |
| stream-parallel-reduce |  | cpp | std::reduce(par) | 1000000 | 0.084 | 0.085 | 11.85G/s | -1 | ok |
| stream-filter-map-reduce |  | go | hand-loop | 1000000 | 0.586 | 0.588 | 1.71G/s | -1 | ok |
| stream-parallel-reduce |  | go | goroutines | 1000000 | 0.045 | 0.066 | 22.31G/s | -1 | ok |
| stream-filter-map-reduce |  | python | genexpr | 1000000 | 19.087 | 19.160 | 52.39M/s | -1 | ok |
| stream-parallel-reduce |  | python | sum (GIL) | 1000000 | 3.690 | 3.711 | 270.97M/s | -1 | ok |
| stream-filter-map-reduce |  | java | java.util.stream | 1000000 | 0.153 | 0.154 | 6.53G/s | -1 | ok |
| stream-parallel-reduce |  | java | parallel-stream | 1000000 | 0.103 | 0.117 | 9.73G/s | -1 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.215 | 0.224 | 4.66G/s | 16 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.102 | 0.102 | 9.83G/s | 16 | ok |
| matmul |  | cajeta | stdlib | 40000 | 0.051 | 0.052 | 780.08M/s | 16 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.654 | 0.659 | 1.53G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.454 | 0.457 | 2.20G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.627 | 2.633 | 15.22M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.638 | 0.643 | 1.57G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.641 | 0.645 | 1.56G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.457 | 2.462 | 16.28M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.401 | 0.409 | 2.49G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.392 | 0.398 | 2.55G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 2.998 | 3.037 | 13.34M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.452 | 0.462 | 2.21G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 0.011 | 0.011 | 89.28G/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.078 | 0.080 | 511.59M/s | -1 | ok |
| saxpy |  | java | scalar | 1000000 | 0.407 | 1.052 | 2.46G/s | -1 | ok |
| dot-product |  | java | scalar | 1000000 | 0.395 | 0.822 | 2.53G/s | -1 | ok |
| matmul |  | java | scalar | 40000 | 2.287 | 2.406 | 17.49M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.728 | 28.773 | 22.28M/s | 20 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 151.404 | 152.966 | 66/s | 16 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 0.101 | 0.102 | 986.67K/s | 16 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 25.583 | 25.617 | 25.02M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 160.300 | 160.964 | 62/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.206 | 0.206 | 486.27K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.572 | 23.704 | 27.15M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 156.870 | 157.437 | 64/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.137 | 0.149 | 731.86K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 23.726 | 23.756 | 26.97M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 149.262 | 149.996 | 67/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.314 | 0.320 | 318.44K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 113.623 | 114.835 | 5.63M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4243.242 | 4277.117 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 37.528 | 37.567 | 2.66K/s | -1 | ok |
| clbg-mandelbrot |  | java | stdlib | 640000 | 26.077 | 26.665 | 24.54M/s | -1 | ok |
| clbg-fannkuch-redux |  | java | stdlib | 10 | 145.872 | 159.096 | 69/s | -1 | ok |
| clbg-spectral-norm |  | java | stdlib | 100 | 0.325 | 1.316 | 307.24K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 21.495 | 21.530 | 46.52M/s | 16 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 2.071 | 2.073 | 48.29M/s | 16 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 6.640 | 6.715 | 150.60M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.533 | 0.542 | 187.67M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.098 | 0.098 | 10.25G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.010 | 0.010 | 10.23G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 2.934 | 2.968 | 340.87M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 1.513 | 1.517 | 66.09M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 352.980 | 360.959 | 2.83M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 17.839 | 17.954 | 5.61M/s | -1 | ok |
| time-instant-arith |  | java | java.time | 1000000 | 0.207 | 0.208 | 4.83G/s | -1 | ok |
| time-localdate-arith |  | java | java.time | 100000 | 0.741 | 1.178 | 134.98M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 5.271 | 5.278 | 189.72M/s | 20 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 80.438 | 82.071 | 248.64K/s | 16 | ok |
| atomic-fetchadd |  | rust | AtomicI64 | 1000000 | 3.908 | 4.357 | 255.86M/s | -1 | ok |
| task-spawn-await |  | rust | std::thread | 20000 | 415.645 | 425.038 | 48.12K/s | -1 | ok |
| atomic-fetchadd |  | cpp | std::atomic | 1000000 | 3.908 | 3.911 | 255.86M/s | -1 | ok |
| task-spawn-await |  | cpp | std::thread | 20000 | 320.243 | 335.172 | 62.45K/s | -1 | ok |
| atomic-fetchadd |  | go | atomic.Int64 | 1000000 | 3.912 | 3.917 | 255.65M/s | -1 | ok |
| task-spawn-await |  | go | goroutine | 20000 | 4.932 | 5.394 | 4.05M/s | -1 | ok |
| atomic-fetchadd |  | python | atomic | 1000000 |  |  |  | -1 | skipped |
| task-spawn-await |  | python | asyncio | 20000 | 70.885 | 72.444 | 282.15K/s | -1 | ok |
| atomic-fetchadd |  | java | AtomicLong | 1000000 | 3.910 | 3.912 | 255.75M/s | -1 | ok |
| task-spawn-await |  | java | virtual-threads | 20000 | 36.587 | 39.823 | 546.64K/s | -1 | ok |
