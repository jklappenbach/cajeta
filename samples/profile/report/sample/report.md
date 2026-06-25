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
| json-tokenize | twitter | cajeta | stdlib | 631514 | 0.613 | 0.626 | 1.03G/s | 404 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 1.477 | 1.499 | 1.17G/s | 2116 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 2.427 | 2.848 | 927.51M/s | 3708 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.174 | 0.175 | 3.63G/s | 2812 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.452 | 0.454 | 3.82G/s | 0 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.816 | 0.820 | 2.76G/s | 0 | ok |
| json-dom | twitter | cajeta | stdlib | 631514 | 2.338 | 2.676 | 270.14M/s | 174212 | ok |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 6.676 | 6.969 | 258.73M/s | 584188 | ok |
| json-dom | canada | cajeta | stdlib | 2251051 | 16.559 | 17.123 | 135.94M/s | 1668716 | ok |
| json-serialize | twitter | cajeta | stdlib | 631514 | 1.047 | 1.145 | 603.45M/s | 237388 | ok |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 2.743 | 3.069 | 629.64M/s | 693212 | ok |
| json-serialize | canada | cajeta | stdlib | 2251051 | 9.442 | 10.224 | 238.42M/s | 1806168 | ok |
| json-roundtrip | twitter | cajeta | stdlib | 631514 | 5.969 | 6.086 | 105.80M/s | 411732 | ok |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 15.435 | 15.933 | 111.90M/s | 1277448 | ok |
| json-roundtrip | canada | cajeta | stdlib | 2251051 | 43.773 | 44.685 | 51.43M/s | 3408908 | ok |
| json-conformance |  | cajeta | stdlib | 14 | 0.015 | 0.017 | 920.57K/s | 892 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 0.907 | 0.914 | 1.16G/s | 136560 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 1.171 | 1.180 | 895.23M/s | 102416 | ok |
| json-dom |  | rust | serde_json | 631514 | 0.879 | 0.906 | 718.74M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.726 | 0.738 | 869.33M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 1.615 | 1.622 | 1.07G/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.626 | 1.634 | 1.06G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 3.478 | 3.489 | 647.31M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 3.761 | 3.777 | 598.48M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.082 | 0.085 | 7.67G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.113 | 0.117 | 5.60G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.226 | 0.231 | 7.65G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.307 | 0.308 | 5.63G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.233 | 1.235 | 1.83G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.328 | 1.344 | 1.70G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.529 | 3.964 | 178.96M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.452 | 2.809 | 257.50M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 8.839 | 9.890 | 195.42M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 3.706 | 5.883 | 466.07M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 14.933 | 16.204 | 150.75M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 13.243 | 14.371 | 169.98M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.752 | 1.766 | 360.53M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.698 | 0.705 | 904.34M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.926 | 0.935 | 681.83M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.156 | 1.170 | 546.44M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 4.098 | 4.150 | 421.47M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.128 | 2.134 | 811.66M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.680 | 2.737 | 644.51M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 3.044 | 3.067 | 567.50M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 30.906 | 31.484 | 72.84M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.405 | 5.464 | 416.45M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 6.208 | 6.239 | 362.61M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.744 | 8.873 | 257.45M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 631514 | 1.107 | 1.164 | 570.49M/s | -1 | ok |
| json-dom |  | java | jackson-databind | 1727204 | 1.700 | 2.089 | 1.02G/s | -1 | ok |
| json-dom |  | java | jackson-databind | 2251051 | 18.535 | 20.091 | 121.45M/s | -1 | ok |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 0.177 | 0.181 | 565.15M/s | 45356 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 0.475 | 0.490 | 105.23M/s | 38420 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 2.128 | 2.182 | 14.10M/s | 104892 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 1.263 | 1.284 | 79.20M/s | 76820 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 0.635 | 0.653 | 63.03M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 3.968 | 3.981 | 12.60M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 1.847 | 1.892 | 10.83M/s | 25012 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 1.000 | 1.012 | 49.98M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.313 | 0.335 | 159.71M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.730 | 3.801 | 8.04M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 3.660 | 3.665 | 8.20M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.472 | 1.480 | 67.94M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.034 | 0.035 | 2.92G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.805 | 0.809 | 62.15M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.183 | 0.193 | 273.76M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.703 | 2.738 | 11.10M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.482 | 1.526 | 20.25M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.990 | 0.996 | 100.99M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.034 | 0.035 | 2.94G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.768 | 0.925 | 65.13M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.758 | 3.219 | 10.88M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.690 | 2.044 | 59.16M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.341 | 0.582 | 292.85M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.503 | 3.601 | 14.27M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.221 | 6.281 | 4.82M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.393 | 3.879 | 29.48M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.638 | 1.712 | 61.04M/s | -1 | ok |
| hashmap-int |  | java | HashMap | 50000 | 0.403 | 1.272 | 124.05M/s | -1 | ok |
| hashmap-string |  | java | HashMap | 30000 | 1.500 | 2.602 | 19.99M/s | -1 | ok |
| hashset-dedup |  | java | HashSet | 100000 | 1.128 | 1.272 | 88.66M/s | -1 | ok |
| arraylist-append |  | java | ArrayList | 100000 | 0.590 | 0.907 | 169.48M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 0.562 | 0.568 | 88.91M/s | 11736 | ok |
| sort-int64 | ascending | cajeta | stdlib | 50000 | 0.063 | 0.065 | 798.02M/s | 11736 | ok |
| sort-int64 | descending | cajeta | stdlib | 50000 | 0.074 | 0.075 | 676.01M/s | 11736 | ok |
| sort-int64 | dups | cajeta | stdlib | 50000 | 0.130 | 0.135 | 383.58M/s | 11736 | ok |
| sort-f64 |  | cajeta | stdlib | 50000 | 0.473 | 0.478 | 105.64M/s | 11736 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 1.502 | 1.514 | 33.30M/s | 23456 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 3.470 | 3.479 | 14.41M/s | 16 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.408 | 0.421 | 122.46M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.011 | 0.011 | 4.74G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.013 | 0.013 | 3.84G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.111 | 0.112 | 448.75M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.627 | 0.630 | 79.79M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.545 | 0.554 | 91.68M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.797 | 1.802 | 27.82M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.737 | 0.750 | 67.81M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.242 | 0.261 | 206.69M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.54G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.154 | 0.154 | 325.42M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.041 | 1.22G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.493 | 0.496 | 101.50M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.077 | 0.077 | 648.72M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.029 | 2.039 | 24.65M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.236 | 2.266 | 22.36M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.260 | 2.267 | 22.12M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.014 | 0.015 | 3.54G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.024 | 0.029 | 2.10G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.080 | 0.092 | 626.96M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.043 | 6.834 | 8.27M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.745 | 2.766 | 18.21M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.425 | 7.462 | 6.73M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.114 | 0.116 | 440.47M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.109 | 0.112 | 457.18M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.798 | 0.805 | 62.65M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.462 | 7.560 | 6.70M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.639 | 4.679 | 10.78M/s | -1 | ok |
| sort-int64 | random | java | Arrays.sort | 50000 | 2.334 | 2.773 | 21.42M/s | -1 | ok |
| sort-int64 | ascending | java | Arrays.sort | 50000 | 0.037 | 0.059 | 1.36G/s | -1 | ok |
| sort-int64 | descending | java | Arrays.sort | 50000 | 0.090 | 0.091 | 553.16M/s | -1 | ok |
| sort-int64 | dups | java | Arrays.sort | 50000 | 0.470 | 0.493 | 106.30M/s | -1 | ok |
| sort-stable-int64 |  | java | Arrays.sort(Timsort) | 50000 | 4.663 | 11.936 | 10.72M/s | -1 | ok |
| sort-f64 |  | java | Arrays.sort | 50000 | 0.315 | 0.371 | 158.90M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 0.002 | 0.002 | 1.91G/s | 380 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.004 | 0.004 | 85.45G/s | 16 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 0.143 | 0.144 | 2.52G/s | 18820 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.049 | 0.051 | 7.41G/s | 17620 | ok |
| string-search |  | rust | std-find | 360448 | 0.024 | 0.024 | 14.98G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.005 | 0.005 | 71.53G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.107 | 0.108 | 3.38G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.066 | 0.067 | 5.43G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 13.29G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.033 | 0.034 | 10.77G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.109 | 0.109 | 3.31G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.398 | 0.404 | 905.44M/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.35G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.046 | 0.046 | 7.81G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.122 | 0.189 | 2.94G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.425 | 0.458 | 847.53M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.002 | 5.78G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.044 | 0.044 | 8.18G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.335 | 0.336 | 1.07G/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.071 | 0.071 | 5.11G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.010 | 0.010 | 410.34M/s | -1 | ok |
| string-search |  | java | String.indexOf | 360448 | 0.061 | 0.560 | 5.88G/s | -1 | ok |
| string-replace |  | java | String.replace | 360448 | 0.612 | 0.631 | 589.06M/s | -1 | ok |
| string-uppercase |  | java | String.toUpperCase | 360448 | 0.279 | 0.290 | 1.29G/s | -1 | ok |
| string-build-concat |  | java | StringBuilder | 4000 | 0.017 | 0.019 | 238.49M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.007 | 0.007 | 156.43G/s | 36 | ok |
| xxhash3-256k |  | cajeta | stdlib | 262144 | 0.002 | 0.002 | 161.52G/s | 56 | ok |
| xxhash3_128 |  | cajeta | stdlib | 1048576 | 0.007 | 0.007 | 155.51G/s | 36 | ok |
| xxhash3_128-256k |  | cajeta | stdlib | 262144 | 0.002 | 0.002 | 160.53G/s | 56 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.246 | 4.26G/s | 16 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 0.415 | 0.416 | 2.53G/s | 20 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.257 | 1.259 | 834.46M/s | 20 | ok |
| blake3 |  | cajeta | stdlib | 1048576 | 0.186 | 0.187 | 5.63G/s | 16 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.012 | 0.012 | 88.92G/s | -1 | ok |
| xxhash3_128 |  | rust | xxhash-rust | 1048576 | 0.012 | 0.012 | 88.55G/s | -1 | ok |
| xxhash3-256k |  | rust | xxhash-rust | 262144 | 0.003 | 0.003 | 90.55G/s | -1 | ok |
| xxhash3_128-256k |  | rust | xxhash-rust | 262144 | 0.003 | 0.003 | 90.55G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.415 | 0.416 | 2.52G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.086 | 1.087 | 965.67M/s | -1 | ok |
| blake3 |  | rust | blake3 | 1048576 | 0.084 | 0.084 | 12.50G/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.008 | 0.008 | 128.09G/s | -1 | ok |
| xxhash3_128 |  | cpp | xxHash | 1048576 | 0.008 | 0.008 | 125.49G/s | -1 | ok |
| xxhash3-256k |  | cpp | xxHash | 262144 | 0.002 | 0.002 | 144.59G/s | -1 | ok |
| xxhash3_128-256k |  | cpp | xxHash | 262144 | 0.002 | 0.002 | 144.51G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.458 | 0.461 | 2.29G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 1.019 | 1.020 | 1.03G/s | -1 | ok |
| blake3 |  | cpp | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.017 | 0.017 | 60.64G/s | -1 | ok |
| xxhash3_128 |  | go | zeebo/xxh3 | 1048576 | 0.017 | 0.017 | 60.74G/s | -1 | ok |
| xxhash3-256k |  | go | zeebo/xxh3 | 262144 | 0.004 | 0.004 | 61.13G/s | -1 | ok |
| xxhash3_128-256k |  | go | zeebo/xxh3 | 262144 | 0.004 | 0.004 | 61.28G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.456 | 0.459 | 2.30G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.923 | 0.929 | 1.14G/s | -1 | ok |
| blake3 |  | go | lukechampine/blake3 | 1048576 | 0.082 | 0.104 | 12.86G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.024 | 0.024 | 44.50G/s | -1 | ok |
| xxhash3_128 |  | python | xxhash | 1048576 | 0.024 | 0.024 | 44.59G/s | -1 | ok |
| xxhash3-256k |  | python | xxhash | 262144 | 0.006 | 0.006 | 43.76G/s | -1 | ok |
| xxhash3_128-256k |  | python | xxhash | 262144 | 0.006 | 0.006 | 43.10G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.416 | 0.418 | 2.52G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.925 | 0.927 | 1.13G/s | -1 | ok |
| blake3 |  | python | blake3 | 1048576 |  |  |  | -1 | skipped |
| xxhash3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3_128 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3-256k |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| xxhash3_128-256k |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| sha256 |  | java | java.security.MessageDigest | 1048576 | 0.475 | 0.481 | 2.21G/s | -1 | ok |
| md5 |  | java | java.security.MessageDigest | 1048576 | 0.937 | 0.940 | 1.12G/s | -1 | ok |
| blake3 |  | java | stdlib | 1048576 |  |  |  | -1 | skipped |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | java | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 23.970 | 24.378 | 41.72M/s | 20 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 10.068 | 11.638 | 99.32M/s | 36 | ok |
| stream-filter-map-reduce |  | rust | Iterator | 1000000 | 0.685 | 0.689 | 1.46G/s | -1 | ok |
| stream-parallel-reduce |  | rust | rayon | 1000000 | 0.091 | 0.102 | 11.00G/s | -1 | ok |
| stream-filter-map-reduce |  | cpp | views::filter|transform | 1000000 | 0.209 | 0.210 | 4.80G/s | -1 | ok |
| stream-parallel-reduce |  | cpp | std::reduce(par) | 1000000 | 0.084 | 0.098 | 11.84G/s | -1 | ok |
| stream-filter-map-reduce |  | go | hand-loop | 1000000 | 0.590 | 0.594 | 1.69G/s | -1 | ok |
| stream-parallel-reduce |  | go | goroutines | 1000000 | 0.053 | 0.064 | 19.00G/s | -1 | ok |
| stream-filter-map-reduce |  | python | genexpr | 1000000 | 18.824 | 19.055 | 53.12M/s | -1 | ok |
| stream-parallel-reduce |  | python | sum (GIL) | 1000000 | 3.728 | 3.752 | 268.25M/s | -1 | ok |
| stream-filter-map-reduce |  | java | java.util.stream | 1000000 | 0.237 | 0.238 | 4.22G/s | -1 | ok |
| stream-parallel-reduce |  | java | parallel-stream | 1000000 | 0.109 | 0.129 | 9.17G/s | -1 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.215 | 0.221 | 4.65G/s | 16 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.102 | 0.103 | 9.82G/s | 16 | ok |
| matmul |  | cajeta | stdlib | 40000 | 0.049 | 0.049 | 821.66M/s | 16 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.569 | 0.581 | 1.76G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.392 | 0.392 | 2.55G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.260 | 2.264 | 17.70M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.695 | 0.701 | 1.44G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.699 | 0.702 | 1.43G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.457 | 2.696 | 16.28M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.436 | 0.437 | 2.30G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.428 | 0.431 | 2.34G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 3.041 | 3.214 | 13.15M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.398 | 0.403 | 2.51G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 0.011 | 0.011 | 93.98G/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.071 | 0.073 | 563.99M/s | -1 | ok |
| saxpy |  | java | scalar | 1000000 | 0.615 | 1.146 | 1.63G/s | -1 | ok |
| dot-product |  | java | scalar | 1000000 | 0.706 | 0.905 | 1.42G/s | -1 | ok |
| matmul |  | java | scalar | 40000 | 2.282 | 2.397 | 17.53M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.741 | 28.775 | 22.27M/s | 16 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 149.279 | 150.875 | 67/s | 16 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 0.102 | 0.102 | 984.15K/s | 16 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 25.595 | 25.621 | 25.01M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 158.388 | 160.496 | 63/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.205 | 0.206 | 486.67K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.592 | 23.638 | 27.13M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 156.920 | 157.258 | 64/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.136 | 0.137 | 732.88K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 23.715 | 23.760 | 26.99M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 148.309 | 149.610 | 67/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.312 | 0.316 | 320.32K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 112.774 | 115.553 | 5.68M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4248.623 | 4305.007 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 37.312 | 37.349 | 2.68K/s | -1 | ok |
| clbg-mandelbrot |  | java | stdlib | 640000 | 26.082 | 26.689 | 24.54M/s | -1 | ok |
| clbg-fannkuch-redux |  | java | stdlib | 10 | 155.497 | 156.516 | 64/s | -1 | ok |
| clbg-spectral-norm |  | java | stdlib | 100 | 0.954 | 1.231 | 104.86K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 20.715 | 20.731 | 48.28M/s | 16 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 2.030 | 2.034 | 49.26M/s | 16 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 6.680 | 6.743 | 149.71M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.533 | 0.543 | 187.72M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.098 | 0.098 | 10.25G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.010 | 0.010 | 10.23G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 2.942 | 3.062 | 339.91M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 1.515 | 1.529 | 66.00M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 355.641 | 357.891 | 2.81M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 17.973 | 18.040 | 5.56M/s | -1 | ok |
| time-instant-arith |  | java | java.time | 1000000 | 0.247 | 0.248 | 4.05G/s | -1 | ok |
| time-localdate-arith |  | java | java.time | 100000 | 0.877 | 1.366 | 114.03M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 5.271 | 5.276 | 189.72M/s | 20 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 82.239 | 83.172 | 243.19K/s | 12 | ok |
| atomic-fetchadd |  | rust | AtomicI64 | 1000000 | 3.903 | 3.913 | 256.21M/s | -1 | ok |
| task-spawn-await |  | rust | std::thread | 20000 | 420.716 | 429.507 | 47.54K/s | -1 | ok |
| atomic-fetchadd |  | cpp | std::atomic | 1000000 | 3.912 | 3.918 | 255.62M/s | -1 | ok |
| task-spawn-await |  | cpp | std::thread | 20000 | 325.958 | 337.810 | 61.36K/s | -1 | ok |
| atomic-fetchadd |  | go | atomic.Int64 | 1000000 | 3.909 | 4.060 | 255.85M/s | -1 | ok |
| task-spawn-await |  | go | goroutine | 20000 | 5.174 | 5.378 | 3.87M/s | -1 | ok |
| atomic-fetchadd |  | python | atomic | 1000000 |  |  |  | -1 | skipped |
| task-spawn-await |  | python | asyncio | 20000 | 69.182 | 69.400 | 289.09K/s | -1 | ok |
| atomic-fetchadd |  | java | AtomicLong | 1000000 | 3.907 | 3.910 | 255.95M/s | -1 | ok |
| task-spawn-await |  | java | virtual-threads | 20000 | 35.796 | 38.527 | 558.72K/s | -1 | ok |
