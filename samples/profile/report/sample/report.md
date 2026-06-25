# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566296

## Languages

| Language | Version |
|---|---|
| cajeta ★ | 0.7.1 (6a7a05a6) |
| rust | 1.91.1 |
| cpp | g++ 15.2.0 |
| go | 1.26.0 |
| python | 3.12.9 |
| java | 25.0.1 |

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 0.620 | 0.626 | 1.02G/s | 468 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 1.515 | 1.525 | 1.14G/s | 2180 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 2.512 | 2.521 | 896.16M/s | 3772 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.407 | 0.410 | 1.55G/s | 2872 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.842 | 0.848 | 2.05G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 1.673 | 1.680 | 1.35G/s | 0 | ok |
| json-dom | twitter | cajeta | stdlib | 631514 | 2.378 | 2.710 | 265.57M/s | 174212 | ok |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 7.161 | 7.237 | 241.19M/s | 584180 | ok |
| json-dom | canada | cajeta | stdlib | 2251051 | 17.260 | 17.435 | 130.42M/s | 1668716 | ok |
| json-serialize | twitter | cajeta | stdlib | 631514 | 1.212 | 1.242 | 521.02M/s | 235844 | ok |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 2.685 | 3.087 | 643.21M/s | 679720 | ok |
| json-serialize | canada | cajeta | stdlib | 2251051 | 10.311 | 10.776 | 218.31M/s | 1806124 | ok |
| json-roundtrip | twitter | cajeta | stdlib | 631514 | 6.052 | 6.169 | 104.35M/s | 410188 | ok |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 16.074 | 16.246 | 107.45M/s | 1263956 | ok |
| json-roundtrip | canada | cajeta | stdlib | 2251051 | 44.746 | 45.444 | 50.31M/s | 3408884 | ok |
| json-conformance |  | cajeta | stdlib | 14 | 0.014 | 0.017 | 967.72K/s | 892 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 1.276 | 1.280 | 821.87M/s | 24 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 1.170 | 1.184 | 896.58M/s | 102416 | ok |
| json-dom |  | rust | serde_json | 631514 | 1.072 | 1.110 | 589.26M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.890 | 0.897 | 709.80M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 1.611 | 2.213 | 1.07G/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.647 | 1.654 | 1.05G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 3.703 | 3.711 | 607.96M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 3.782 | 3.805 | 595.16M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.109 | 0.110 | 5.80G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.141 | 0.143 | 4.46G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.286 | 0.288 | 6.03G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.388 | 0.391 | 4.45G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.508 | 1.533 | 1.49G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.328 | 1.338 | 1.69G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.648 | 4.134 | 173.11M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.367 | 2.778 | 266.76M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 8.668 | 10.611 | 199.26M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 3.429 | 4.914 | 503.67M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 14.878 | 17.320 | 151.30M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 13.471 | 14.808 | 167.11M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.752 | 1.771 | 360.55M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.668 | 0.678 | 945.71M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.891 | 0.910 | 708.97M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.125 | 1.154 | 561.17M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 3.922 | 3.984 | 440.37M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.105 | 2.113 | 820.43M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.702 | 2.755 | 639.24M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 2.979 | 3.049 | 579.83M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 31.008 | 31.360 | 72.59M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.633 | 5.794 | 399.65M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 6.115 | 6.158 | 368.14M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.919 | 8.965 | 252.40M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 631514 | 1.392 | 1.485 | 453.76M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 1727204 | 1.726 | 2.160 | 1.00G/s | -1 | ok |
| json-dom |  | java | jackson-databind | 2251051 | 15.954 | 16.141 | 141.09M/s | -1 | ok |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 0.179 | 0.182 | 559.48M/s | 45356 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 0.531 | 0.546 | 94.17M/s | 38420 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 2.187 | 2.273 | 13.72M/s | 104892 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 1.253 | 1.277 | 79.79M/s | 76820 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 0.631 | 0.646 | 63.35M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 7.968 | 7.982 | 6.28M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 2.328 | 2.364 | 8.59M/s | 25012 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 1.003 | 1.013 | 49.85M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.307 | 0.315 | 162.93M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 4.034 | 4.044 | 7.44M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 3.667 | 3.677 | 8.18M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.468 | 1.476 | 68.12M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.034 | 0.035 | 2.94G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.804 | 0.809 | 62.19M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.192 | 0.192 | 260.35M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.736 | 2.747 | 10.97M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.529 | 1.555 | 19.62M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.973 | 0.974 | 102.79M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.034 | 0.035 | 2.96G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.780 | 0.960 | 64.09M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.501 | 3.394 | 12.00M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.757 | 1.863 | 56.93M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.336 | 0.579 | 298.02M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.485 | 3.572 | 14.35M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.351 | 6.372 | 4.72M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.890 | 3.903 | 25.70M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.795 | 1.868 | 55.71M/s | -1 | ok |
| hashmap-int |  | java | HashMap | 50000 | 0.339 | 1.294 | 147.39M/s | -1 | ok |
| hashmap-string |  | java | HashMap | 30000 | 1.880 | 2.601 | 15.96M/s | -1 | ok |
| hashset-dedup |  | java | HashSet | 100000 | 1.002 | 1.336 | 99.84M/s | -1 | ok |
| arraylist-append |  | java | ArrayList | 100000 | 0.571 | 0.882 | 174.99M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 0.483 | 0.490 | 103.52M/s | 11736 | ok |
| sort-int64 | ascending | cajeta | stdlib | 50000 | 0.000 | 0.000 |  | 11736 | ok |
| sort-int64 | descending | cajeta | stdlib | 50000 | 0.000 | 0.000 |  | 11736 | ok |
| sort-int64 | dups | cajeta | stdlib | 50000 | 0.054 | 0.057 | 919.93M/s | 11736 | ok |
| sort-f64 |  | cajeta | stdlib | 50000 | 0.396 | 0.401 | 126.29M/s | 11736 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 1.490 | 1.496 | 33.56M/s | 23456 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 3.849 | 3.860 | 12.99M/s | 16 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.405 | 0.411 | 123.38M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.010 | 0.011 | 4.77G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.013 | 0.013 | 3.88G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.104 | 0.110 | 483.07M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.582 | 0.589 | 85.96M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.506 | 0.516 | 98.88M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.854 | 1.920 | 26.97M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.732 | 0.748 | 68.33M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.224 | 0.224 | 223.04M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.55G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.144 | 0.144 | 347.65M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.041 | 1.22G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.495 | 0.501 | 101.03M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.077 | 0.078 | 647.70M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.021 | 2.029 | 24.74M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.219 | 2.254 | 22.53M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.265 | 2.467 | 22.08M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.014 | 0.015 | 3.55G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.023 | 0.024 | 2.15G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.082 | 0.087 | 611.44M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.082 | 6.922 | 8.22M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.757 | 2.767 | 18.13M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.377 | 7.475 | 6.78M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.106 | 0.108 | 471.70M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.111 | 0.112 | 451.06M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.824 | 0.828 | 60.69M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.674 | 7.726 | 6.52M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.675 | 4.737 | 10.70M/s | -1 | ok |
| sort-int64 | random | java | Arrays.sort | 50000 | 2.201 | 2.262 | 22.71M/s | -1 | ok |
| sort-int64 | ascending | java | Arrays.sort | 50000 | 0.038 | 0.058 | 1.33G/s | -1 | ok |
| sort-int64 | descending | java | Arrays.sort | 50000 | 0.090 | 0.090 | 555.37M/s | -1 | ok |
| sort-int64 | dups | java | Arrays.sort | 50000 | 0.507 | 0.527 | 98.63M/s | -1 | ok |
| sort-stable-int64 |  | java | Arrays.sort(Timsort) | 50000 | 6.175 | 11.649 | 8.10M/s | -1 | ok |
| sort-f64 |  | java | Arrays.sort | 50000 | 0.314 | 0.359 | 159.31M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 0.002 | 0.002 | 1.93G/s | 380 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.004 | 0.004 | 81.40G/s | 16 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 0.145 | 0.146 | 2.49G/s | 18820 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.048 | 0.050 | 7.47G/s | 17620 | ok |
| string-search |  | rust | std-find | 360448 | 0.023 | 0.023 | 15.40G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.005 | 0.005 | 73.56G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.105 | 0.105 | 3.44G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.061 | 0.061 | 5.90G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 12.90G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.030 | 0.030 | 12.09G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.095 | 0.096 | 3.81G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.354 | 0.357 | 1.02G/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.50G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.046 | 0.046 | 7.82G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.136 | 0.182 | 2.65G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.396 | 0.497 | 910.35M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.002 | 3.22G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.049 | 0.049 | 7.33G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.373 | 0.376 | 967.25M/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.078 | 0.078 | 4.61G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.011 | 0.012 | 348.37M/s | -1 | ok |
| string-search |  | java | String.indexOf | 360448 | 0.113 | 0.568 | 3.18G/s | -1 | ok |
| string-replace |  | java | String.replace | 360448 | 0.447 | 0.908 | 807.07M/s | -1 | ok |
| string-uppercase |  | java | String.toUpperCase | 360448 | 0.287 | 0.293 | 1.26G/s | -1 | ok |
| string-build-concat |  | java | StringBuilder | 4000 | 0.017 | 0.019 | 234.44M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.007 | 0.007 | 152.12G/s | 36 | ok |
| xxhash3-256k |  | cajeta | stdlib | 262144 | 0.002 | 0.002 | 161.52G/s | 56 | ok |
| xxhash3_128 |  | cajeta | stdlib | 1048576 | 0.007 | 0.007 | 149.73G/s | 36 | ok |
| xxhash3_128-256k |  | cajeta | stdlib | 262144 | 0.002 | 0.002 | 160.53G/s | 56 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.246 | 4.26G/s | 16 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 0.415 | 0.415 | 2.52G/s | 20 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.257 | 1.258 | 834.41M/s | 20 | ok |
| blake3 |  | cajeta | stdlib | 1048576 | 0.186 | 0.187 | 5.63G/s | 16 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.020 | 0.020 | 52.67G/s | -1 | ok |
| xxhash3_128 |  | rust | xxhash-rust | 1048576 | 0.013 | 0.014 | 77.87G/s | -1 | ok |
| xxhash3-256k |  | rust | xxhash-rust | 262144 | 0.003 | 0.003 | 79.53G/s | -1 | ok |
| xxhash3_128-256k |  | rust | xxhash-rust | 262144 | 0.003 | 0.003 | 79.53G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.470 | 0.475 | 2.23G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.226 | 1.229 | 855.39M/s | -1 | ok |
| blake3 |  | rust | blake3 | 1048576 | 0.095 | 0.096 | 11.07G/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.007 | 0.007 | 150.16G/s | -1 | ok |
| xxhash3_128 |  | cpp | xxHash | 1048576 | 0.007 | 0.007 | 146.98G/s | -1 | ok |
| xxhash3-256k |  | cpp | xxHash | 262144 | 0.002 | 0.002 | 159.55G/s | -1 | ok |
| xxhash3_128-256k |  | cpp | xxHash | 262144 | 0.002 | 0.002 | 159.55G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.416 | 0.417 | 2.52G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.923 | 0.925 | 1.14G/s | -1 | ok |
| blake3 |  | cpp | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.016 | 0.016 | 66.28G/s | -1 | ok |
| xxhash3_128 |  | go | zeebo/xxh3 | 1048576 | 0.016 | 0.016 | 66.37G/s | -1 | ok |
| xxhash3-256k |  | go | zeebo/xxh3 | 262144 | 0.004 | 0.004 | 66.92G/s | -1 | ok |
| xxhash3_128-256k |  | go | zeebo/xxh3 | 262144 | 0.004 | 0.004 | 66.92G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.416 | 0.416 | 2.52G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.924 | 0.929 | 1.13G/s | -1 | ok |
| blake3 |  | go | lukechampine/blake3 | 1048576 | 0.076 | 0.111 | 13.85G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 44.82G/s | -1 | ok |
| xxhash3_128 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 44.96G/s | -1 | ok |
| xxhash3-256k |  | python | xxhash | 262144 | 0.006 | 0.006 | 44.27G/s | -1 | ok |
| xxhash3_128-256k |  | python | xxhash | 262144 | 0.006 | 0.006 | 43.61G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.416 | 0.417 | 2.52G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.924 | 0.927 | 1.13G/s | -1 | ok |
| blake3 |  | python | blake3 | 1048576 |  |  |  | -1 | skipped |
| xxhash3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3_128 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3-256k |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3_128-256k |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| sha256 |  | java | java.security.MessageDigest | 1048576 | 0.477 | 0.482 | 2.20G/s | -1 | ok |
| md5 |  | java | java.security.MessageDigest | 1048576 | 0.934 | 0.937 | 1.12G/s | -1 | ok |
| blake3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | java | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 24.129 | 24.245 | 41.44M/s | 20 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 9.846 | 10.480 | 101.56M/s | 40 | ok |
| stream-filter-map-reduce |  | rust | Iterator | 1000000 | 0.586 | 0.589 | 1.71G/s | -1 | ok |
| stream-parallel-reduce |  | rust | rayon | 1000000 | 0.082 | 0.101 | 12.13G/s | -1 | ok |
| stream-filter-map-reduce |  | cpp | views::filter|transform | 1000000 | 0.250 | 0.253 | 3.99G/s | -1 | ok |
| stream-parallel-reduce |  | cpp | std::reduce(par) | 1000000 | 0.102 | 0.102 | 9.84G/s | -1 | ok |
| stream-filter-map-reduce |  | go | hand-loop | 1000000 | 0.588 | 0.593 | 1.70G/s | -1 | ok |
| stream-parallel-reduce |  | go | goroutines | 1000000 | 0.052 | 0.072 | 19.16G/s | -1 | ok |
| stream-filter-map-reduce |  | python | genexpr | 1000000 | 20.028 | 20.212 | 49.93M/s | -1 | ok |
| stream-parallel-reduce |  | python | sum (GIL) | 1000000 | 3.740 | 3.753 | 267.39M/s | -1 | ok |
| stream-filter-map-reduce |  | java | java.util.stream | 1000000 | 0.153 | 0.156 | 6.54G/s | -1 | ok |
| stream-parallel-reduce |  | java | parallel-stream | 1000000 | 0.102 | 0.116 | 9.83G/s | -1 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.216 | 0.223 | 4.63G/s | 16 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.102 | 0.103 | 9.82G/s | 16 | ok |
| matmul |  | cajeta | stdlib | 40000 | 0.052 | 0.053 | 767.78M/s | 16 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.569 | 0.575 | 1.76G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.392 | 0.395 | 2.55G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.263 | 2.270 | 17.67M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.640 | 0.647 | 1.56G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.642 | 0.645 | 1.56G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.457 | 2.462 | 16.28M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.412 | 0.420 | 2.43G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.391 | 0.396 | 2.56G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 2.988 | 2.996 | 13.39M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.441 | 0.508 | 2.27G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 0.011 | 0.011 | 94.16G/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.074 | 0.076 | 537.92M/s | -1 | ok |
| saxpy |  | java | scalar | 1000000 | 0.404 | 1.069 | 2.47G/s | -1 | ok |
| dot-product |  | java | scalar | 1000000 | 0.400 | 0.847 | 2.50G/s | -1 | ok |
| matmul |  | java | scalar | 40000 | 2.282 | 2.404 | 17.53M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.739 | 28.758 | 22.27M/s | 16 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 341.192 | 345.328 | 29/s | 16 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 0.103 | 0.103 | 973.59K/s | 16 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 25.557 | 25.596 | 25.04M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 159.422 | 160.607 | 63/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.206 | 0.206 | 485.72K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.561 | 23.628 | 27.16M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 157.337 | 158.670 | 64/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.137 | 0.137 | 732.46K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 23.746 | 23.768 | 26.95M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 146.555 | 150.711 | 68/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.313 | 0.324 | 319.48K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 113.915 | 117.766 | 5.62M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4255.510 | 4293.561 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 36.950 | 37.031 | 2.71K/s | -1 | ok |
| clbg-mandelbrot |  | java | stdlib | 640000 | 26.125 | 26.714 | 24.50M/s | -1 | ok |
| clbg-fannkuch-redux |  | java | stdlib | 10 | 164.047 | 164.876 | 61/s | -1 | ok |
| clbg-spectral-norm |  | java | stdlib | 100 | 0.569 | 1.373 | 175.72K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 20.710 | 20.729 | 48.28M/s | 16 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 2.071 | 2.073 | 48.29M/s | 16 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 6.730 | 6.974 | 148.58M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.534 | 0.543 | 187.40M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.111 | 0.111 | 9.02G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.011 | 0.011 | 9.00G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 2.934 | 2.946 | 340.79M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 1.513 | 1.515 | 66.08M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 351.111 | 352.690 | 2.85M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 17.773 | 17.831 | 5.63M/s | -1 | ok |
| time-instant-arith |  | java | java.time | 1000000 | 0.207 | 0.209 | 4.83G/s | -1 | ok |
| time-localdate-arith |  | java | java.time | 100000 | 0.742 | 1.166 | 134.69M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 5.272 | 5.279 | 189.68M/s | 20 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 85.225 | 86.240 | 234.67K/s | 16 | ok |
| atomic-fetchadd |  | rust | AtomicI64 | 1000000 | 3.911 | 3.914 | 255.71M/s | -1 | ok |
| task-spawn-await |  | rust | std::thread | 20000 | 414.456 | 422.839 | 48.26K/s | -1 | ok |
| atomic-fetchadd |  | cpp | std::atomic | 1000000 | 3.907 | 3.941 | 255.96M/s | -1 | ok |
| task-spawn-await |  | cpp | std::thread | 20000 | 326.393 | 334.428 | 61.28K/s | -1 | ok |
| atomic-fetchadd |  | go | atomic.Int64 | 1000000 | 3.902 | 3.907 | 256.27M/s | -1 | ok |
| task-spawn-await |  | go | goroutine | 20000 | 5.237 | 5.600 | 3.82M/s | -1 | ok |
| atomic-fetchadd |  | python | atomic | 1000000 |  |  |  | -1 | skipped |
| task-spawn-await |  | python | asyncio | 20000 | 68.340 | 68.932 | 292.65K/s | -1 | ok |
| atomic-fetchadd |  | java | AtomicLong | 1000000 | 3.904 | 3.909 | 256.17M/s | -1 | ok |
| task-spawn-await |  | java | virtual-threads | 20000 | 33.745 | 39.591 | 592.69K/s | -1 | ok |
