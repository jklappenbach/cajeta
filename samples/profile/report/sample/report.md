# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 0.678 | 0.696 | 931.66M/s | 624 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 1.419 | 1.491 | 1.22G/s | 1936 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 2.302 | 2.387 | 977.95M/s | 3516 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.169 | 0.170 | 3.74G/s | 2616 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.433 | 0.436 | 3.99G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.794 | 0.810 | 2.84G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 6.449 | 6.881 | 267.82M/s | 479400 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 2.525 | 3.006 | 684.17M/s | 595216 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 15.013 | 16.348 | 115.05M/s | 1074744 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.009 | 0.010 | 1.59M/s | 176 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 0.937 | 0.970 | 1.12G/s | 136944 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 1.079 | 1.094 | 971.80M/s | 16 | ok |
| json-dom |  | rust | serde_json | 631514 | 1.129 | 1.152 | 559.40M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.725 | 0.738 | 871.30M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 1.621 | 1.665 | 1.07G/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.667 | 1.685 | 1.04G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 3.544 | 3.629 | 635.15M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 3.869 | 3.907 | 581.83M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.103 | 0.107 | 6.15G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.144 | 0.145 | 4.37G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.275 | 0.280 | 6.27G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.309 | 0.374 | 5.60G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.235 | 1.241 | 1.82G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.327 | 1.351 | 1.70G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.508 | 4.174 | 180.04M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.198 | 2.598 | 287.31M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 7.980 | 9.633 | 216.45M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 4.365 | 5.781 | 395.72M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 14.953 | 15.960 | 150.54M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 14.756 | 16.906 | 152.56M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.762 | 1.775 | 358.34M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.669 | 0.680 | 943.70M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.898 | 0.908 | 703.61M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.138 | 1.151 | 555.08M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 4.102 | 4.138 | 421.10M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.103 | 2.123 | 821.45M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.716 | 2.766 | 635.96M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 3.012 | 3.057 | 573.36M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 30.892 | 31.378 | 72.87M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.739 | 5.832 | 392.22M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 6.122 | 6.167 | 367.70M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.951 | 9.049 | 251.48M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 631514 | 1.118 | 1.204 | 564.67M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 1727204 | 1.785 | 2.322 | 967.43M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 2251051 | 15.805 | 16.067 | 142.43M/s | -1 | ok |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 0.132 | 0.136 | 758.05M/s | 25644 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 1.585 | 1.600 | 31.54M/s | 34640 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 8.759 | 9.182 | 3.43M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 1.622 | 1.642 | 61.64M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 0.690 | 0.698 | 58.01M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 3.566 | 3.597 | 14.02M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 1.954 | 1.994 | 10.24M/s | 25016 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 1.024 | 1.032 | 48.84M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.310 | 0.333 | 161.18M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.728 | 3.760 | 8.05M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 3.655 | 3.678 | 8.21M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.464 | 1.474 | 68.28M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.034 | 0.034 | 2.94G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.900 | 0.923 | 55.53M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.199 | 0.205 | 251.15M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.719 | 2.736 | 11.03M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.493 | 1.541 | 20.10M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.991 | 0.996 | 100.91M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.034 | 0.035 | 2.92G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.825 | 0.985 | 60.63M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.626 | 3.814 | 11.42M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.914 | 2.178 | 52.23M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.324 | 0.620 | 308.33M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.425 | 3.527 | 14.60M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.053 | 6.404 | 4.96M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.576 | 3.667 | 27.96M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.820 | 1.830 | 54.96M/s | -1 | ok |
| hashmap-int |  | java | HashMap | 50000 | 0.346 | 1.292 | 144.43M/s | -1 | ok |
| hashmap-string |  | java | HashMap | 30000 | 1.418 | 2.710 | 21.15M/s | -1 | ok |
| hashset-dedup |  | java | HashSet | 100000 | 0.996 | 1.357 | 100.37M/s | -1 | ok |
| arraylist-append |  | java | ArrayList | 100000 | 0.587 | 0.671 | 170.27M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 1.771 | 1.781 | 28.24M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | 50000 | 0.195 | 0.204 | 256.42M/s | 11752 | ok |
| sort-int64 | descending | cajeta | stdlib | 50000 | 0.207 | 0.211 | 241.02M/s | 11752 | ok |
| sort-int64 | dups | cajeta | stdlib | 50000 | 0.781 | 0.788 | 63.99M/s | 11752 | ok |
| sort-f64 |  | cajeta | stdlib | 50000 | 2.350 | 2.363 | 21.27M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 1.663 | 1.680 | 30.07M/s | 23456 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 3.601 | 3.613 | 13.89M/s | 11736 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.414 | 0.427 | 120.79M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.011 | 0.011 | 4.71G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.013 | 0.013 | 3.80G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.112 | 0.113 | 447.91M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.646 | 0.656 | 77.44M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.550 | 0.558 | 90.92M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.817 | 1.876 | 27.51M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.743 | 0.760 | 67.25M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.228 | 0.229 | 219.55M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.54G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.153 | 0.157 | 327.19M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.041 | 1.22G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.489 | 0.498 | 102.31M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.077 | 0.078 | 645.61M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.017 | 2.024 | 24.79M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.255 | 2.278 | 22.17M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.500 | 2.543 | 20.00M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.020 | 0.025 | 2.54G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.021 | 0.022 | 2.33G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.086 | 0.092 | 580.91M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.155 | 6.828 | 8.12M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.810 | 3.037 | 17.79M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.446 | 7.515 | 6.72M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.105 | 0.108 | 475.11M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.101 | 0.104 | 494.41M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.774 | 0.793 | 64.58M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.555 | 7.734 | 6.62M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.497 | 4.632 | 11.12M/s | -1 | ok |
| sort-int64 | random | java | Arrays.sort | 50000 | 2.163 | 2.410 | 23.12M/s | -1 | ok |
| sort-int64 | ascending | java | Arrays.sort | 50000 | 0.041 | 0.052 | 1.22G/s | -1 | ok |
| sort-int64 | descending | java | Arrays.sort | 50000 | 0.097 | 0.098 | 515.51M/s | -1 | ok |
| sort-int64 | dups | java | Arrays.sort | 50000 | 0.498 | 0.514 | 100.34M/s | -1 | ok |
| sort-stable-int64 |  | java | Arrays.sort(Timsort) | 50000 | 5.010 | 14.274 | 9.98M/s | -1 | ok |
| sort-f64 |  | java | Arrays.sort | 50000 | 0.313 | 0.380 | 159.68M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 0.031 | 0.034 | 127.51M/s | 2880 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.021 | 0.024 | 17.31G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 0.221 | 0.224 | 1.63G/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.290 | 0.294 | 1.24G/s | 17600 | ok |
| string-search |  | rust | std-find | 360448 | 0.021 | 0.021 | 17.08G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.004 | 0.004 | 81.59G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.095 | 0.097 | 3.80G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.059 | 0.059 | 6.10G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 13.79G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.032 | 0.032 | 11.27G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.094 | 0.095 | 3.84G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.358 | 0.383 | 1.01G/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.51G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.041 | 0.041 | 8.71G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.123 | 0.171 | 2.94G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.382 | 0.427 | 943.48M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.001 | 5.26G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.044 | 0.044 | 8.17G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.336 | 0.336 | 1.07G/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.071 | 0.071 | 5.10G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.010 | 0.010 | 403.67M/s | -1 | ok |
| string-search |  | java | String.indexOf | 360448 | 0.066 | 0.588 | 5.45G/s | -1 | ok |
| string-replace |  | java | String.replace | 360448 | 0.427 | 0.693 | 843.97M/s | -1 | ok |
| string-uppercase |  | java | String.toUpperCase | 360448 | 0.289 | 0.295 | 1.25G/s | -1 | ok |
| string-build-concat |  | java | StringBuilder | 4000 | 0.017 | 0.019 | 233.20M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.007 | 0.007 | 149.93G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.246 | 4.26G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 2.266 | 2.273 | 462.77M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.720 | 1.727 | 609.56M/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.013 | 0.013 | 78.22G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.467 | 0.475 | 2.25G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.202 | 1.219 | 872.09M/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.012 | 0.013 | 87.43G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.460 | 0.463 | 2.28G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 1.016 | 1.020 | 1.03G/s | -1 | ok |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.016 | 0.016 | 66.92G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.416 | 0.418 | 2.52G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.926 | 0.929 | 1.13G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 45.39G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.417 | 0.418 | 2.51G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.924 | 0.926 | 1.13G/s | -1 | ok |
| xxhash3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| sha256 |  | java | java.security.MessageDigest | 1048576 | 0.475 | 0.482 | 2.21G/s | -1 | ok |
| md5 |  | java | java.security.MessageDigest | 1048576 | 0.937 | 0.940 | 1.12G/s | -1 | ok |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | java | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 23.085 | 23.380 | 43.32M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 9.770 | 10.920 | 102.35M/s | 16 | ok |
| stream-filter-map-reduce |  | rust | Iterator | 1000000 | 0.658 | 0.661 | 1.52G/s | -1 | ok |
| stream-parallel-reduce |  | rust | rayon | 1000000 | 0.084 | 0.104 | 11.86G/s | -1 | ok |
| stream-filter-map-reduce |  | cpp | hand-loop | 1000000 | 0.059 | 0.059 | 17.05G/s | -1 | ok |
| stream-parallel-reduce |  | cpp | OpenMP | 1000000 | 0.041 | 0.042 | 24.38G/s | -1 | ok |
| stream-filter-map-reduce |  | go | hand-loop | 1000000 | 0.586 | 0.588 | 1.71G/s | -1 | ok |
| stream-parallel-reduce |  | go | goroutines | 1000000 | 0.044 | 0.077 | 22.73G/s | -1 | ok |
| stream-filter-map-reduce |  | python | genexpr | 1000000 | 18.765 | 19.415 | 53.29M/s | -1 | ok |
| stream-parallel-reduce |  | python | sum (GIL) | 1000000 | 3.687 | 3.721 | 271.25M/s | -1 | ok |
| stream-filter-map-reduce |  | java | java.util.stream | 1000000 | 0.278 | 0.283 | 3.60G/s | -1 | ok |
| stream-parallel-reduce |  | java | parallel-stream | 1000000 | 0.114 | 0.134 | 8.75G/s | -1 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.588 | 0.599 | 1.70G/s | 16 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.391 | 0.395 | 2.56G/s | 16 | ok |
| matmul |  | cajeta | stdlib | 40000 | 2.826 | 2.834 | 14.15M/s | 16 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.604 | 0.617 | 1.66G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.399 | 0.403 | 2.51G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.261 | 2.270 | 17.69M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.639 | 0.648 | 1.57G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.642 | 0.647 | 1.56G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.460 | 2.471 | 16.26M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.423 | 0.429 | 2.37G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.415 | 0.428 | 2.41G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 3.000 | 3.173 | 13.33M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.423 | 0.480 | 2.36G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 0.010 | 0.011 | 95.79G/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.075 | 0.079 | 532.55M/s | -1 | ok |
| saxpy |  | java | scalar | 1000000 | 0.438 | 1.121 | 2.28G/s | -1 | ok |
| dot-product |  | java | scalar | 1000000 | 0.789 | 0.875 | 1.27G/s | -1 | ok |
| matmul |  | java | scalar | 40000 | 2.290 | 2.394 | 17.46M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.764 | 28.811 | 22.25M/s | 16 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 144.650 | 145.443 | 69/s | 16 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 1.626 | 1.630 | 61.50K/s | 88 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 25.661 | 25.780 | 24.94M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 157.950 | 160.003 | 63/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.206 | 0.207 | 484.92K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.758 | 26.340 | 26.94M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 157.037 | 158.412 | 64/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.136 | 0.137 | 732.99K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 23.792 | 23.831 | 26.90M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 150.668 | 155.546 | 66/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.314 | 0.330 | 318.75K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 115.871 | 142.056 | 5.52M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4347.030 | 4419.175 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 38.247 | 38.341 | 2.61K/s | -1 | ok |
| clbg-mandelbrot |  | java | stdlib | 640000 | 26.076 | 26.167 | 24.54M/s | -1 | ok |
| clbg-fannkuch-redux |  | java | stdlib | 10 | 157.086 | 157.607 | 64/s | -1 | ok |
| clbg-spectral-norm |  | java | stdlib | 100 | 0.320 | 1.213 | 312.30K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 22.296 | 22.332 | 44.85M/s | 16 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 2.170 | 2.174 | 46.08M/s | 8 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 6.638 | 6.709 | 150.65M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.533 | 0.543 | 187.70M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.098 | 0.098 | 10.24G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.010 | 0.010 | 10.23G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 3.436 | 3.449 | 291.04M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 1.512 | 1.516 | 66.12M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 348.181 | 351.094 | 2.87M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 17.191 | 17.684 | 5.82M/s | -1 | ok |
| time-instant-arith |  | java | java.time | 1000000 | 0.207 | 0.207 | 4.83G/s | -1 | ok |
| time-localdate-arith |  | java | java.time | 100000 | 0.751 | 1.045 | 133.15M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 5.275 | 5.285 | 189.59M/s | 20 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 80.084 | 82.544 | 249.74K/s | 16 | ok |
| atomic-fetchadd |  | rust | AtomicI64 | 1000000 | 3.908 | 3.915 | 255.91M/s | -1 | ok |
| task-spawn-await |  | rust | std::thread | 20000 | 420.911 | 429.083 | 47.52K/s | -1 | ok |
| atomic-fetchadd |  | cpp | std::atomic | 1000000 | 3.917 | 3.930 | 255.29M/s | -1 | ok |
| task-spawn-await |  | cpp | std::thread | 20000 | 328.279 | 339.523 | 60.92K/s | -1 | ok |
| atomic-fetchadd |  | go | atomic.Int64 | 1000000 | 3.906 | 3.910 | 256.00M/s | -1 | ok |
| task-spawn-await |  | go | goroutine | 20000 | 4.873 | 5.311 | 4.10M/s | -1 | ok |
| atomic-fetchadd |  | python | atomic | 1000000 |  |  |  | -1 | skipped |
| task-spawn-await |  | python | asyncio | 20000 | 69.021 | 70.913 | 289.77K/s | -1 | ok |
| atomic-fetchadd |  | java | AtomicLong | 1000000 | 3.903 | 3.912 | 256.25M/s | -1 | ok |
| task-spawn-await |  | java | virtual-threads | 20000 | 38.022 | 43.259 | 526.01K/s | -1 | ok |
