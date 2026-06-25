# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566296

## Languages

| Language | Version |
|---|---|
| cajeta ★ | 0.7.1 (1717ef5c) |
| rust | 1.91.1 |
| cpp | g++ 15.2.0 |
| go | 1.26.0 |
| python | 3.12.9 |
| java | 25.0.1 |

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 0.600 | 0.614 | 1.05G/s | 468 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 1.486 | 1.549 | 1.16G/s | 2184 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 2.494 | 2.522 | 902.74M/s | 3768 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.407 | 0.409 | 1.55G/s | 2872 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.839 | 0.846 | 2.06G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 1.681 | 1.686 | 1.34G/s | 0 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 6.505 | 6.864 | 265.53M/s | 479320 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 2.626 | 3.113 | 657.82M/s | 575016 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 15.087 | 15.577 | 114.48M/s | 1054508 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.014 | 0.017 | 1.03M/s | 848 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 1.273 | 1.278 | 823.45M/s | 24 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 1.176 | 1.187 | 891.73M/s | 102416 | ok |
| json-dom |  | rust | serde_json | 631514 | 0.878 | 0.885 | 719.46M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.723 | 0.732 | 873.24M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 1.670 | 1.750 | 1.03G/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.625 | 1.957 | 1.06G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 3.496 | 3.624 | 643.87M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 3.618 | 3.658 | 622.13M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.096 | 0.099 | 6.60G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.132 | 0.133 | 4.77G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.226 | 0.226 | 7.66G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.306 | 0.307 | 5.64G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.232 | 1.237 | 1.83G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.325 | 1.330 | 1.70G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.657 | 3.965 | 172.68M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.512 | 2.863 | 251.42M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 8.407 | 9.345 | 205.44M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 4.182 | 5.102 | 413.00M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 14.553 | 16.096 | 154.68M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 13.088 | 15.606 | 172.00M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.739 | 1.789 | 363.23M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.690 | 0.720 | 914.67M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.877 | 0.894 | 719.72M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.095 | 1.117 | 576.90M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 4.027 | 4.064 | 428.94M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.036 | 2.064 | 848.32M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.625 | 2.685 | 657.94M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 2.914 | 2.954 | 592.81M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 31.186 | 31.969 | 72.18M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.570 | 5.666 | 404.17M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 6.124 | 6.155 | 367.58M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.564 | 8.961 | 262.84M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 631514 | 1.143 | 1.178 | 552.60M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 1727204 | 1.729 | 2.487 | 998.83M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 2251051 | 15.555 | 15.692 | 144.71M/s | -1 | ok |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 0.178 | 0.181 | 561.78M/s | 45364 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 0.529 | 0.541 | 94.45M/s | 38420 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 2.208 | 2.273 | 13.59M/s | 104892 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 1.261 | 1.273 | 79.32M/s | 76824 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 0.630 | 0.637 | 63.49M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 8.085 | 8.105 | 6.18M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 2.332 | 2.382 | 8.58M/s | 25016 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 0.944 | 0.996 | 52.98M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.309 | 0.315 | 161.98M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.726 | 3.780 | 8.05M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 3.699 | 3.718 | 8.11M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.581 | 1.629 | 63.24M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.034 | 0.034 | 2.94G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.811 | 0.819 | 61.64M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.183 | 0.184 | 272.89M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.768 | 2.775 | 10.84M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.530 | 1.541 | 19.61M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.966 | 0.970 | 103.50M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.034 | 0.035 | 2.96G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.783 | 0.951 | 63.87M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.440 | 3.073 | 12.29M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.964 | 2.386 | 50.91M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.442 | 0.575 | 226.23M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.416 | 3.610 | 14.64M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.271 | 6.301 | 4.78M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.373 | 3.602 | 29.65M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.796 | 1.821 | 55.67M/s | -1 | ok |
| hashmap-int |  | java | HashMap | 50000 | 0.332 | 1.118 | 150.71M/s | -1 | ok |
| hashmap-string |  | java | HashMap | 30000 | 1.596 | 2.430 | 18.80M/s | -1 | ok |
| hashset-dedup |  | java | HashSet | 100000 | 0.990 | 1.313 | 101.00M/s | -1 | ok |
| arraylist-append |  | java | ArrayList | 100000 | 0.574 | 0.893 | 174.30M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 0.486 | 0.494 | 102.85M/s | 11736 | ok |
| sort-int64 | ascending | cajeta | stdlib | 50000 | 0.000 | 0.000 |  | 11736 | ok |
| sort-int64 | descending | cajeta | stdlib | 50000 | 0.000 | 0.000 |  | 11736 | ok |
| sort-int64 | dups | cajeta | stdlib | 50000 | 0.053 | 0.055 | 935.03M/s | 11736 | ok |
| sort-f64 |  | cajeta | stdlib | 50000 | 0.394 | 0.400 | 126.94M/s | 11732 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 1.491 | 1.501 | 33.54M/s | 23456 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 3.830 | 3.853 | 13.05M/s | 16 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.377 | 0.386 | 132.49M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.010 | 0.010 | 5.11G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.011 | 0.011 | 4.53G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.103 | 0.122 | 485.37M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.577 | 0.582 | 86.67M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.500 | 0.510 | 99.91M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.817 | 1.994 | 27.52M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.730 | 0.752 | 68.51M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.240 | 0.241 | 208.51M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.55G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.153 | 0.153 | 326.76M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.041 | 1.22G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.487 | 0.492 | 102.72M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.077 | 0.077 | 648.29M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.017 | 2.033 | 24.79M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.243 | 2.271 | 22.29M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.292 | 2.493 | 21.81M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.016 | 0.025 | 3.22G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.024 | 0.024 | 2.11G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.088 | 0.094 | 570.15M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.038 | 6.145 | 8.28M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.746 | 2.768 | 18.21M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.441 | 7.519 | 6.72M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.099 | 0.105 | 502.98M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.106 | 0.111 | 472.82M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.795 | 0.801 | 62.89M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.636 | 7.674 | 6.55M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.655 | 4.728 | 10.74M/s | -1 | ok |
| sort-int64 | random | java | Arrays.sort | 50000 | 1.808 | 2.310 | 27.65M/s | -1 | ok |
| sort-int64 | ascending | java | Arrays.sort | 50000 | 0.055 | 0.062 | 916.39M/s | -1 | ok |
| sort-int64 | descending | java | Arrays.sort | 50000 | 0.090 | 0.091 | 553.46M/s | -1 | ok |
| sort-int64 | dups | java | Arrays.sort | 50000 | 0.523 | 0.610 | 95.56M/s | -1 | ok |
| sort-stable-int64 |  | java | Arrays.sort(Timsort) | 50000 | 6.976 | 9.870 | 7.17M/s | -1 | ok |
| sort-f64 |  | java | Arrays.sort | 50000 | 0.309 | 0.349 | 161.98M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 0.017 | 0.019 | 228.66M/s | 2880 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.004 | 0.004 | 81.40G/s | 16 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 0.145 | 0.146 | 2.48G/s | 18820 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.049 | 0.051 | 7.37G/s | 17620 | ok |
| string-search |  | rust | std-find | 360448 | 0.023 | 0.023 | 15.41G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.005 | 0.005 | 73.58G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.106 | 0.107 | 3.39G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.061 | 0.061 | 5.91G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 12.90G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.030 | 0.030 | 12.11G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.097 | 0.099 | 3.72G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.354 | 0.355 | 1.02G/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.51G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.046 | 0.046 | 7.86G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.123 | 0.183 | 2.92G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.369 | 0.454 | 975.56M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.002 | 3.30G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.050 | 0.050 | 7.28G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.373 | 0.375 | 966.44M/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.078 | 0.078 | 4.60G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.016 | 0.017 | 245.23M/s | -1 | ok |
| string-search |  | java | String.indexOf | 360448 | 0.061 | 0.063 | 5.88G/s | -1 | ok |
| string-replace |  | java | String.replace | 360448 | 0.429 | 0.903 | 840.19M/s | -1 | ok |
| string-uppercase |  | java | String.toUpperCase | 360448 | 0.275 | 0.278 | 1.31G/s | -1 | ok |
| string-build-concat |  | java | StringBuilder | 4000 | 0.017 | 0.019 | 235.82M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.008 | 0.008 | 134.54G/s | 120 | ok |
| xxhash3-256k |  | cajeta | stdlib | 262144 | 0.002 | 0.002 | 143.80G/s | 0 | ok |
| xxhash3_128 |  | cajeta | stdlib | 1048576 | 0.008 | 0.008 | 133.83G/s | 0 | ok |
| xxhash3_128-256k |  | cajeta | stdlib | 262144 | 0.002 | 0.002 | 144.59G/s | 4 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.252 | 0.255 | 4.16G/s | 4 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 0.426 | 0.427 | 2.46G/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.279 | 1.297 | 820.01M/s | 0 | ok |
| blake3 |  | cajeta | stdlib | 1048576 | 0.189 | 0.190 | 5.54G/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.013 | 0.013 | 79.17G/s | -1 | ok |
| xxhash3_128 |  | rust | xxhash-rust | 1048576 | 0.013 | 0.013 | 78.81G/s | -1 | ok |
| xxhash3-256k |  | rust | xxhash-rust | 262144 | 0.003 | 0.003 | 80.51G/s | -1 | ok |
| xxhash3_128-256k |  | rust | xxhash-rust | 262144 | 0.003 | 0.003 | 80.51G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.415 | 0.417 | 2.53G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.086 | 1.094 | 965.68M/s | -1 | ok |
| blake3 |  | rust | blake3 | 1048576 | 0.084 | 0.084 | 12.52G/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.008 | 0.008 | 136.62G/s | -1 | ok |
| xxhash3_128 |  | cpp | xxHash | 1048576 | 0.008 | 0.012 | 136.28G/s | -1 | ok |
| xxhash3-256k |  | cpp | xxHash | 262144 | 0.002 | 0.002 | 143.01G/s | -1 | ok |
| xxhash3_128-256k |  | cpp | xxHash | 262144 | 0.002 | 0.002 | 142.24G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.416 | 0.419 | 2.52G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.923 | 0.928 | 1.14G/s | -1 | ok |
| blake3 |  | cpp | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.016 | 0.016 | 66.79G/s | -1 | ok |
| xxhash3_128 |  | go | zeebo/xxh3 | 1048576 | 0.016 | 0.016 | 66.79G/s | -1 | ok |
| xxhash3-256k |  | go | zeebo/xxh3 | 262144 | 0.004 | 0.004 | 67.10G/s | -1 | ok |
| xxhash3_128-256k |  | go | zeebo/xxh3 | 262144 | 0.004 | 0.004 | 67.10G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.416 | 0.421 | 2.52G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.925 | 0.931 | 1.13G/s | -1 | ok |
| blake3 |  | go | lukechampine/blake3 | 1048576 | 0.053 | 0.092 | 19.78G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 46.02G/s | -1 | ok |
| xxhash3_128 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 45.66G/s | -1 | ok |
| xxhash3-256k |  | python | xxhash | 262144 | 0.006 | 0.006 | 44.73G/s | -1 | ok |
| xxhash3_128-256k |  | python | xxhash | 262144 | 0.006 | 0.006 | 44.05G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.415 | 0.417 | 2.53G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.923 | 0.929 | 1.14G/s | -1 | ok |
| blake3 |  | python | blake3 | 1048576 |  |  |  | -1 | skipped |
| xxhash3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3_128 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3-256k |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3_128-256k |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| sha256 |  | java | java.security.MessageDigest | 1048576 | 0.473 | 0.476 | 2.22G/s | -1 | ok |
| md5 |  | java | java.security.MessageDigest | 1048576 | 0.934 | 0.942 | 1.12G/s | -1 | ok |
| blake3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | java | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 24.379 | 24.400 | 41.02M/s | 20 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 9.896 | 11.818 | 101.05M/s | 40 | ok |
| stream-filter-map-reduce |  | rust | Iterator | 1000000 | 0.586 | 0.590 | 1.71G/s | -1 | ok |
| stream-parallel-reduce |  | rust | rayon | 1000000 | 0.089 | 0.109 | 11.29G/s | -1 | ok |
| stream-filter-map-reduce |  | cpp | hand-loop | 1000000 | 0.058 | 0.065 | 17.26G/s | -1 | ok |
| stream-parallel-reduce |  | cpp | OpenMP | 1000000 | 0.029 | 0.037 | 35.03G/s | -1 | ok |
| stream-filter-map-reduce |  | go | hand-loop | 1000000 | 0.652 | 0.658 | 1.53G/s | -1 | ok |
| stream-parallel-reduce |  | go | goroutines | 1000000 | 0.038 | 0.060 | 26.26G/s | -1 | ok |
| stream-filter-map-reduce |  | python | genexpr | 1000000 | 19.848 | 20.292 | 50.38M/s | -1 | ok |
| stream-parallel-reduce |  | python | sum (GIL) | 1000000 | 3.675 | 3.712 | 272.10M/s | -1 | ok |
| stream-filter-map-reduce |  | java | java.util.stream | 1000000 | 0.153 | 0.154 | 6.55G/s | -1 | ok |
| stream-parallel-reduce |  | java | parallel-stream | 1000000 | 0.109 | 0.116 | 9.20G/s | -1 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.587 | 0.600 | 1.70G/s | 16 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.392 | 0.395 | 2.55G/s | 16 | ok |
| matmul |  | cajeta | stdlib | 40000 | 0.446 | 0.448 | 89.65M/s | 16 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.582 | 0.634 | 1.72G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.392 | 0.393 | 2.55G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.265 | 2.270 | 17.66M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.691 | 0.695 | 1.45G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.694 | 0.696 | 1.44G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.450 | 2.457 | 16.33M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.399 | 0.408 | 2.50G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.391 | 0.396 | 2.56G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 2.994 | 3.066 | 13.36M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.376 | 0.433 | 2.66G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 5.989 | 5.999 | 166.98M/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.072 | 0.074 | 559.17M/s | -1 | ok |
| saxpy |  | java | scalar | 1000000 | 0.419 | 1.037 | 2.38G/s | -1 | ok |
| dot-product |  | java | scalar | 1000000 | 0.399 | 0.851 | 2.51G/s | -1 | ok |
| matmul |  | java | scalar | 40000 | 2.290 | 2.407 | 17.47M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.738 | 28.765 | 22.27M/s | 16 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 326.681 | 335.412 | 31/s | 16 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 1.637 | 1.641 | 61.08K/s | 16 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 25.579 | 25.655 | 25.02M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 159.206 | 162.164 | 63/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.206 | 0.206 | 486.29K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.550 | 23.573 | 27.18M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 157.722 | 159.032 | 63/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.137 | 0.137 | 732.29K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 23.741 | 23.784 | 26.96M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 148.271 | 148.825 | 67/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.312 | 0.327 | 320.09K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 113.596 | 118.312 | 5.63M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4242.482 | 4307.642 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 37.366 | 37.392 | 2.68K/s | -1 | ok |
| clbg-mandelbrot |  | java | stdlib | 640000 | 26.086 | 26.124 | 24.53M/s | -1 | ok |
| clbg-fannkuch-redux |  | java | stdlib | 10 | 156.363 | 159.988 | 64/s | -1 | ok |
| clbg-spectral-norm |  | java | stdlib | 100 | 0.322 | 1.016 | 310.51K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 21.100 | 21.122 | 47.39M/s | 16 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 2.051 | 2.053 | 48.76M/s | 16 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 6.684 | 6.729 | 149.61M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.531 | 0.542 | 188.22M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.098 | 0.098 | 10.25G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.010 | 0.010 | 10.23G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 2.939 | 2.957 | 340.22M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 1.512 | 1.516 | 66.12M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 356.050 | 365.672 | 2.81M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 17.506 | 17.687 | 5.71M/s | -1 | ok |
| time-instant-arith |  | java | java.time | 1000000 | 0.207 | 0.207 | 4.83G/s | -1 | ok |
| time-localdate-arith |  | java | java.time | 100000 | 0.801 | 1.044 | 124.82M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 5.273 | 5.279 | 189.66M/s | 20 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 85.274 | 85.326 | 234.54K/s | 16 | ok |
| atomic-fetchadd |  | rust | AtomicI64 | 1000000 | 3.909 | 3.914 | 255.81M/s | -1 | ok |
| task-spawn-await |  | rust | std::thread | 20000 | 414.025 | 429.261 | 48.31K/s | -1 | ok |
| atomic-fetchadd |  | cpp | std::atomic | 1000000 | 3.909 | 3.915 | 255.79M/s | -1 | ok |
| task-spawn-await |  | cpp | std::thread | 20000 | 316.969 | 330.509 | 63.10K/s | -1 | ok |
| atomic-fetchadd |  | go | atomic.Int64 | 1000000 | 4.075 | 4.077 | 245.41M/s | -1 | ok |
| task-spawn-await |  | go | goroutine | 20000 | 5.247 | 5.561 | 3.81M/s | -1 | ok |
| atomic-fetchadd |  | python | atomic | 1000000 |  |  |  | -1 | skipped |
| task-spawn-await |  | python | asyncio | 20000 | 68.735 | 69.267 | 290.97K/s | -1 | ok |
| atomic-fetchadd |  | java | AtomicLong | 1000000 | 3.904 | 3.911 | 256.14M/s | -1 | ok |
| task-spawn-await |  | java | virtual-threads | 20000 | 36.650 | 38.888 | 545.71K/s | -1 | ok |
