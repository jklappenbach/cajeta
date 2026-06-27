# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566296

## Languages

| Language | Version |
|---|---|
| cajeta ★ | 0.7.1 (7f51f06c) |
| rust | 1.91.1 |
| cpp | g++ 15.2.0 |
| go | 1.26.0 |
| python | 3.12.9 |
| java | 25.0.1 |

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 0.580 | 0.585 | 1.09G/s | 468 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 1.406 | 1.469 | 1.23G/s | 2180 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 2.219 | 2.261 | 1.01G/s | 3772 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.166 | 0.167 | 3.81G/s | 2876 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.448 | 0.453 | 3.85G/s | 0 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.811 | 0.816 | 2.78G/s | 0 | ok |
| json-dom | twitter | cajeta | stdlib | 631514 | 2.288 | 2.630 | 275.99M/s | 174212 | ok |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 6.511 | 6.817 | 265.28M/s | 584184 | ok |
| json-dom | canada | cajeta | stdlib | 2251051 | 16.404 | 16.703 | 137.23M/s | 1668716 | ok |
| json-serialize | twitter | cajeta | stdlib | 631514 | 0.980 | 1.138 | 644.13M/s | 237388 | ok |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 2.598 | 3.138 | 664.88M/s | 693212 | ok |
| json-serialize | canada | cajeta | stdlib | 2251051 | 9.746 | 10.543 | 230.98M/s | 1806168 | ok |
| json-roundtrip | twitter | cajeta | stdlib | 631514 | 5.837 | 6.007 | 108.19M/s | 411732 | ok |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 15.253 | 15.689 | 113.23M/s | 1277448 | ok |
| json-roundtrip | canada | cajeta | stdlib | 2251051 | 42.163 | 43.072 | 53.39M/s | 3408924 | ok |
| json-conformance |  | cajeta | stdlib | 14 | 0.014 | 0.016 | 967.05K/s | 892 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 1.192 | 1.212 | 880.04M/s | 136560 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 1.160 | 1.167 | 904.23M/s | 102416 | ok |
| json-dom |  | rust | serde_json | 631514 | 0.975 | 0.985 | 647.60M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.806 | 0.808 | 783.39M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 1.603 | 1.720 | 1.08G/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.616 | 1.629 | 1.07G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 3.451 | 3.467 | 652.20M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 3.734 | 3.771 | 602.87M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.093 | 0.096 | 6.79G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.131 | 0.131 | 4.84G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.254 | 0.256 | 6.81G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.354 | 0.357 | 4.88G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.229 | 1.239 | 1.83G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.335 | 1.374 | 1.69G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.767 | 4.177 | 167.62M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.423 | 2.961 | 260.66M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 8.171 | 10.016 | 211.40M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 4.867 | 5.461 | 354.89M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 14.577 | 17.862 | 154.43M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 12.420 | 13.227 | 181.24M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.752 | 1.769 | 360.48M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.694 | 0.707 | 910.48M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.896 | 0.905 | 705.15M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.138 | 1.152 | 555.05M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 4.035 | 4.643 | 428.10M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.076 | 2.098 | 832.08M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.691 | 2.743 | 641.89M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 3.004 | 3.041 | 575.01M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 31.605 | 32.179 | 71.22M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.364 | 5.475 | 419.65M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 6.070 | 6.095 | 370.83M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.805 | 8.851 | 255.66M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 631514 | 1.121 | 1.181 | 563.53M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 1727204 | 1.984 | 2.335 | 870.71M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 2251051 | 15.357 | 15.872 | 146.58M/s | -1 | ok |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 0.181 | 0.184 | 552.42M/s | 45356 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 0.464 | 0.473 | 107.86M/s | 38420 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 2.105 | 2.169 | 14.25M/s | 104892 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 1.237 | 1.247 | 80.84M/s | 76820 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 0.606 | 0.613 | 65.98M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 3.411 | 3.425 | 14.66M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 1.911 | 1.986 | 10.47M/s | 25012 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 0.939 | 0.948 | 53.27M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.309 | 0.312 | 162.00M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.735 | 3.755 | 8.03M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 3.683 | 3.703 | 8.15M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.464 | 1.475 | 68.32M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.034 | 0.035 | 2.91G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.802 | 0.810 | 62.31M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.178 | 0.180 | 281.40M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.801 | 2.816 | 10.71M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.491 | 1.541 | 20.12M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.962 | 0.966 | 103.92M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.034 | 0.036 | 2.90G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.780 | 0.907 | 64.07M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.497 | 3.346 | 12.01M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.789 | 2.120 | 55.89M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.282 | 0.600 | 354.12M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.768 | 3.781 | 13.27M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.344 | 6.372 | 4.73M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.867 | 3.909 | 25.86M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.758 | 1.792 | 56.87M/s | -1 | ok |
| hashmap-int |  | java | HashMap | 50000 | 0.454 | 1.490 | 110.15M/s | -1 | ok |
| hashmap-string |  | java | HashMap | 30000 | 1.378 | 2.734 | 21.78M/s | -1 | ok |
| hashset-dedup |  | java | HashSet | 100000 | 1.002 | 1.309 | 99.81M/s | -1 | ok |
| arraylist-append |  | java | ArrayList | 100000 | 0.568 | 0.658 | 176.08M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 0.566 | 0.572 | 88.37M/s | 11736 | ok |
| sort-int64 | ascending | cajeta | stdlib | 50000 | 0.063 | 0.065 | 792.49M/s | 11736 | ok |
| sort-int64 | descending | cajeta | stdlib | 50000 | 0.073 | 0.074 | 680.99M/s | 11736 | ok |
| sort-int64 | dups | cajeta | stdlib | 50000 | 0.125 | 0.133 | 400.63M/s | 11736 | ok |
| sort-f64 |  | cajeta | stdlib | 50000 | 0.468 | 0.474 | 106.91M/s | 11736 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 1.501 | 1.509 | 33.30M/s | 23456 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 3.555 | 3.560 | 14.06M/s | 16 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.400 | 0.421 | 125.01M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.017 | 0.017 | 2.93G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.016 | 0.021 | 3.08G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.103 | 0.103 | 484.85M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.575 | 0.586 | 86.93M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.501 | 0.509 | 99.81M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.808 | 1.823 | 27.66M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.734 | 0.740 | 68.11M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.234 | 0.235 | 213.58M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.55G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.154 | 0.154 | 325.44M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.041 | 1.21G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.492 | 0.497 | 101.56M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.077 | 0.077 | 647.53M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.019 | 2.023 | 24.77M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.250 | 2.260 | 22.22M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.253 | 2.296 | 22.19M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.015 | 0.024 | 3.29G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.023 | 0.037 | 2.19G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.081 | 0.091 | 615.82M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.108 | 6.180 | 8.19M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.765 | 2.781 | 18.09M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.412 | 7.487 | 6.75M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.104 | 0.105 | 480.14M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.107 | 0.109 | 469.30M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.783 | 0.789 | 63.82M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.588 | 7.711 | 6.59M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.639 | 4.683 | 10.78M/s | -1 | ok |
| sort-int64 | random | java | Arrays.sort | 50000 | 2.150 | 2.359 | 23.25M/s | -1 | ok |
| sort-int64 | ascending | java | Arrays.sort | 50000 | 0.058 | 0.059 | 856.16M/s | -1 | ok |
| sort-int64 | descending | java | Arrays.sort | 50000 | 0.090 | 0.090 | 553.59M/s | -1 | ok |
| sort-int64 | dups | java | Arrays.sort | 50000 | 0.558 | 0.564 | 89.53M/s | -1 | ok |
| sort-stable-int64 |  | java | Arrays.sort(Timsort) | 50000 | 6.685 | 13.521 | 7.48M/s | -1 | ok |
| sort-f64 |  | java | Arrays.sort | 50000 | 0.313 | 0.365 | 159.70M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 0.002 | 0.002 | 1.82G/s | 380 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.004 | 0.004 | 81.40G/s | 16 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 0.145 | 0.146 | 2.49G/s | 18820 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.049 | 0.052 | 7.29G/s | 17620 | ok |
| string-search |  | rust | std-find | 360448 | 0.024 | 0.024 | 14.94G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.005 | 0.005 | 71.25G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.107 | 0.108 | 3.37G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.063 | 0.063 | 5.73G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 12.90G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.034 | 0.034 | 10.58G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.111 | 0.112 | 3.25G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.356 | 0.406 | 1.01G/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.51G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.047 | 0.047 | 7.64G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.139 | 0.194 | 2.59G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.385 | 0.487 | 936.02M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.004 | 3.41G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.044 | 0.044 | 8.18G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.336 | 0.336 | 1.07G/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.071 | 0.071 | 5.10G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.010 | 0.010 | 397.26M/s | -1 | ok |
| string-search |  | java | String.indexOf | 360448 | 0.061 | 0.547 | 5.89G/s | -1 | ok |
| string-replace |  | java | String.replace | 360448 | 0.626 | 0.641 | 576.25M/s | -1 | ok |
| string-uppercase |  | java | String.toUpperCase | 360448 | 0.286 | 0.293 | 1.26G/s | -1 | ok |
| string-build-concat |  | java | StringBuilder | 4000 | 0.017 | 0.020 | 232.26M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.007 | 0.007 | 157.16G/s | 36 | ok |
| xxhash3-256k |  | cajeta | stdlib | 262144 | 0.002 | 0.002 | 161.52G/s | 56 | ok |
| xxhash3_128 |  | cajeta | stdlib | 1048576 | 0.007 | 0.007 | 157.37G/s | 36 | ok |
| xxhash3_128-256k |  | cajeta | stdlib | 262144 | 0.002 | 0.002 | 160.53G/s | 56 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.246 | 4.26G/s | 16 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 0.415 | 0.416 | 2.53G/s | 20 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.257 | 1.259 | 834.37M/s | 20 | ok |
| blake3 |  | cajeta | stdlib | 1048576 | 0.183 | 0.184 | 5.73G/s | 16 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.020 | 0.020 | 52.31G/s | -1 | ok |
| xxhash3_128 |  | rust | xxhash-rust | 1048576 | 0.018 | 0.020 | 57.79G/s | -1 | ok |
| xxhash3-256k |  | rust | xxhash-rust | 262144 | 0.003 | 0.003 | 83.59G/s | -1 | ok |
| xxhash3_128-256k |  | rust | xxhash-rust | 262144 | 0.003 | 0.003 | 83.59G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.450 | 0.451 | 2.33G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.086 | 1.088 | 965.22M/s | -1 | ok |
| blake3 |  | rust | blake3 | 1048576 | 0.083 | 0.083 | 12.63G/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.008 | 0.008 | 128.09G/s | -1 | ok |
| xxhash3_128 |  | cpp | xxHash | 1048576 | 0.008 | 0.008 | 126.70G/s | -1 | ok |
| xxhash3-256k |  | cpp | xxHash | 262144 | 0.002 | 0.002 | 148.69G/s | -1 | ok |
| xxhash3_128-256k |  | cpp | xxHash | 262144 | 0.002 | 0.002 | 147.85G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.450 | 0.450 | 2.33G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.923 | 1.001 | 1.14G/s | -1 | ok |
| blake3 |  | cpp | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.016 | 0.016 | 66.49G/s | -1 | ok |
| xxhash3_128 |  | go | zeebo/xxh3 | 1048576 | 0.016 | 0.016 | 66.83G/s | -1 | ok |
| xxhash3-256k |  | go | zeebo/xxh3 | 262144 | 0.004 | 0.004 | 67.10G/s | -1 | ok |
| xxhash3_128-256k |  | go | zeebo/xxh3 | 262144 | 0.004 | 0.004 | 67.10G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.420 | 0.421 | 2.50G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.925 | 0.929 | 1.13G/s | -1 | ok |
| blake3 |  | go | lukechampine/blake3 | 1048576 | 0.063 | 0.093 | 16.52G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 45.23G/s | -1 | ok |
| xxhash3_128 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 45.25G/s | -1 | ok |
| xxhash3-256k |  | python | xxhash | 262144 | 0.006 | 0.006 | 43.98G/s | -1 | ok |
| xxhash3_128-256k |  | python | xxhash | 262144 | 0.006 | 0.006 | 43.18G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.417 | 0.419 | 2.51G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.924 | 0.927 | 1.14G/s | -1 | ok |
| blake3 |  | python | blake3 | 1048576 |  |  |  | -1 | skipped |
| xxhash3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3_128 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3-256k |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3_128-256k |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| sha256 |  | java | java.security.MessageDigest | 1048576 | 0.480 | 0.486 | 2.18G/s | -1 | ok |
| md5 |  | java | java.security.MessageDigest | 1048576 | 0.941 | 0.943 | 1.11G/s | -1 | ok |
| blake3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | java | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 21.318 | 21.924 | 46.91M/s | 20 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 9.005 | 9.677 | 111.05M/s | 36 | ok |
| stream-filter-map-reduce |  | rust | Iterator | 1000000 | 0.673 | 0.675 | 1.49G/s | -1 | ok |
| stream-parallel-reduce |  | rust | rayon | 1000000 | 0.087 | 0.098 | 11.53G/s | -1 | ok |
| stream-filter-map-reduce |  | cpp | views::filter|transform | 1000000 | 0.209 | 0.214 | 4.78G/s | -1 | ok |
| stream-parallel-reduce |  | cpp | std::reduce(par) | 1000000 | 0.084 | 0.084 | 11.85G/s | -1 | ok |
| stream-filter-map-reduce |  | go | hand-loop | 1000000 | 0.585 | 0.590 | 1.71G/s | -1 | ok |
| stream-parallel-reduce |  | go | goroutines | 1000000 | 0.041 | 0.076 | 24.63G/s | -1 | ok |
| stream-filter-map-reduce |  | python | genexpr | 1000000 | 18.723 | 18.876 | 53.41M/s | -1 | ok |
| stream-parallel-reduce |  | python | sum (GIL) | 1000000 | 3.705 | 3.728 | 269.91M/s | -1 | ok |
| stream-filter-map-reduce |  | java | java.util.stream | 1000000 | 0.236 | 0.239 | 4.24G/s | -1 | ok |
| stream-parallel-reduce |  | java | parallel-stream | 1000000 | 0.103 | 0.112 | 9.74G/s | -1 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.218 | 0.241 | 4.59G/s | 16 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.103 | 0.106 | 9.68G/s | 16 | ok |
| matmul |  | cajeta | stdlib | 40000 | 0.058 | 0.059 | 691.34M/s | 16 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.576 | 0.601 | 1.73G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.392 | 0.392 | 2.55G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.259 | 2.263 | 17.71M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.638 | 0.644 | 1.57G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.641 | 0.645 | 1.56G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.455 | 2.463 | 16.29M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.441 | 0.447 | 2.27G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.433 | 0.437 | 2.31G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 2.996 | 3.027 | 13.35M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.504 | 0.509 | 1.98G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 0.010 | 0.010 | 99.51G/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.077 | 0.078 | 520.80M/s | -1 | ok |
| saxpy |  | java | scalar | 1000000 | 0.414 | 1.062 | 2.41G/s | -1 | ok |
| dot-product |  | java | scalar | 1000000 | 0.534 | 0.835 | 1.87G/s | -1 | ok |
| matmul |  | java | scalar | 40000 | 2.283 | 2.400 | 17.52M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 4.257 | 4.262 | 150.34M/s | 16 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 151.173 | 151.279 | 66/s | 16 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 0.102 | 0.102 | 981.53K/s | 16 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 25.613 | 25.641 | 24.99M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 158.845 | 161.116 | 63/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.206 | 0.206 | 485.63K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.598 | 23.622 | 27.12M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 157.975 | 158.107 | 63/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.137 | 0.137 | 731.97K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 23.767 | 23.774 | 26.93M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 148.607 | 149.252 | 67/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.313 | 0.332 | 319.89K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 116.174 | 122.178 | 5.51M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4293.279 | 4346.666 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 37.404 | 37.436 | 2.67K/s | -1 | ok |
| clbg-mandelbrot |  | java | stdlib | 640000 | 26.068 | 26.784 | 24.55M/s | -1 | ok |
| clbg-fannkuch-redux |  | java | stdlib | 10 | 155.930 | 158.542 | 64/s | -1 | ok |
| clbg-spectral-norm |  | java | stdlib | 100 | 0.502 | 1.367 | 199.19K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 20.725 | 20.754 | 48.25M/s | 16 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 2.033 | 2.036 | 49.19M/s | 16 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 6.657 | 6.740 | 150.23M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.532 | 0.543 | 187.88M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.109 | 0.109 | 9.19G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.011 | 0.011 | 9.17G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 2.937 | 2.962 | 340.50M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 1.512 | 1.518 | 66.14M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 350.208 | 352.057 | 2.86M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 16.873 | 17.223 | 5.93M/s | -1 | ok |
| time-instant-arith |  | java | java.time | 1000000 | 0.207 | 0.241 | 4.82G/s | -1 | ok |
| time-localdate-arith |  | java | java.time | 100000 | 0.742 | 1.171 | 134.81M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 3.905 | 3.910 | 256.06M/s | 20 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 81.945 | 86.506 | 244.07K/s | 12 | ok |
| atomic-fetchadd |  | rust | AtomicI64 | 1000000 | 3.911 | 4.202 | 255.68M/s | -1 | ok |
| task-spawn-await |  | rust | std::thread | 20000 | 419.547 | 425.131 | 47.67K/s | -1 | ok |
| atomic-fetchadd |  | cpp | std::atomic | 1000000 | 3.910 | 3.913 | 255.72M/s | -1 | ok |
| task-spawn-await |  | cpp | std::thread | 20000 | 326.922 | 334.739 | 61.18K/s | -1 | ok |
| atomic-fetchadd |  | go | atomic.Int64 | 1000000 | 3.910 | 3.915 | 255.77M/s | -1 | ok |
| task-spawn-await |  | go | goroutine | 20000 | 5.119 | 5.392 | 3.91M/s | -1 | ok |
| atomic-fetchadd |  | python | atomic | 1000000 |  |  |  | -1 | skipped |
| task-spawn-await |  | python | asyncio | 20000 | 69.268 | 69.828 | 288.73K/s | -1 | ok |
| atomic-fetchadd |  | java | AtomicLong | 1000000 | 3.910 | 3.913 | 255.75M/s | -1 | ok |
| task-spawn-await |  | java | virtual-threads | 20000 | 36.094 | 39.256 | 554.11K/s | -1 | ok |
