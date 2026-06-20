# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 1.451 | 1.533 | 435.35M/s | 620 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 3.827 | 4.019 | 451.28M/s | 1920 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 9.142 | 9.303 | 246.22M/s | 3508 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.163 | 0.163 | 3.88G/s | 2612 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.402 | 0.407 | 4.30G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.737 | 0.765 | 3.05G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 10.376 | 11.170 | 166.47M/s | 479368 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 12.285 | 14.251 | 140.59M/s | 595212 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 32.738 | 34.927 | 52.76M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.025 | 0.027 | 553.40K/s | 864 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 10.474 | 10.596 | 100.12M/s | 136556 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 11.507 | 11.635 | 91.12M/s | 102420 | ok |
| json-dom |  | rust | serde_json | 631514 | 0.985 | 1.002 | 641.05M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.716 | 0.723 | 882.56M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 2.272 | 2.291 | 760.30M/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.584 | 1.605 | 1.09G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 4.115 | 4.152 | 547.07M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 4.120 | 4.149 | 546.37M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.083 | 0.086 | 7.59G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.118 | 0.119 | 5.34G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.228 | 0.229 | 7.57G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.311 | 0.314 | 5.55G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.240 | 1.245 | 1.82G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.335 | 1.343 | 1.69G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.427 | 3.722 | 184.28M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.266 | 2.533 | 278.69M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 8.278 | 9.360 | 208.64M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 4.408 | 5.283 | 391.86M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 14.553 | 15.792 | 154.67M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 12.689 | 14.710 | 177.40M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.744 | 1.753 | 362.17M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.690 | 0.705 | 915.65M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.915 | 0.931 | 690.37M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.124 | 1.153 | 562.02M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 3.910 | 4.015 | 441.74M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.094 | 2.109 | 824.95M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.703 | 2.724 | 639.03M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 2.966 | 3.021 | 582.28M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 31.740 | 32.557 | 70.92M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.754 | 5.885 | 391.23M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 6.098 | 6.212 | 369.12M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.813 | 8.889 | 255.42M/s | -1 | ok |
| json-dom |  | java | jackson | 631514 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 1727204 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 2251051 |  |  |  | -1 | skipped |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 0.999 | 1.005 | 100.12M/s | 45368 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 2.525 | 2.558 | 19.80M/s | 69128 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 11.566 | 11.919 | 2.59M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 3.204 | 3.272 | 31.21M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 1.276 | 1.295 | 31.36M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 4.595 | 4.640 | 10.88M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 3.308 | 3.371 | 6.05M/s | 25016 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 0.815 | 0.873 | 61.33M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.275 | 0.278 | 181.75M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.113 | 3.162 | 9.64M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 2.922 | 2.960 | 10.27M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.239 | 1.245 | 80.68M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.026 | 0.027 | 3.78G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.881 | 0.888 | 56.78M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.182 | 0.185 | 274.96M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.737 | 2.756 | 10.96M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.485 | 1.541 | 20.20M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.962 | 0.981 | 104.00M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.035 | 0.036 | 2.90G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.867 | 1.039 | 57.64M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.775 | 3.770 | 10.81M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.742 | 2.100 | 57.40M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.472 | 0.599 | 212.00M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.372 | 3.606 | 14.83M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.273 | 6.338 | 4.78M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.359 | 3.458 | 29.77M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.715 | 1.761 | 58.30M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 3.042 | 3.061 | 16.44M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | stdlib | 50000 | 3.311 | 3.330 | 15.10M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 3.445 | 3.549 | 14.52M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 5.314 | 5.339 | 9.41M/s | 11736 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.356 | 0.362 | 140.64M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.011 | 0.011 | 4.41G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.014 | 0.014 | 3.51G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.091 | 0.091 | 550.90M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.589 | 0.595 | 84.82M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.739 | 0.750 | 67.68M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.802 | 1.844 | 27.74M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.708 | 0.717 | 70.62M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.259 | 0.269 | 193.02M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.52G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.150 | 0.150 | 334.26M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.042 | 1.20G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.491 | 0.507 | 101.78M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.077 | 0.077 | 647.12M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.016 | 2.030 | 24.80M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.237 | 2.252 | 22.35M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.255 | 2.334 | 22.17M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.014 | 0.021 | 3.55G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.021 | 0.022 | 2.34G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.075 | 0.077 | 664.88M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.073 | 6.380 | 8.23M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.741 | 2.764 | 18.24M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.475 | 7.646 | 6.69M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.105 | 0.108 | 475.79M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.100 | 0.104 | 498.81M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.776 | 0.790 | 64.43M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.531 | 8.527 | 6.64M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.578 | 4.909 | 10.92M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 10.285 | 13.485 | 388.93K/s | 1891392 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.208 | 0.211 | 1.74G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 1.017 | 1.023 | 354.45M/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.256 | 0.263 | 1.41G/s | 17604 | ok |
| string-search |  | rust | std-find | 360448 | 0.020 | 0.020 | 17.76G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.007 | 0.007 | 50.67G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.098 | 0.098 | 3.70G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.014 | 0.014 | 25.50G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 12.12G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.030 | 0.030 | 12.10G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.096 | 0.097 | 3.75G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.355 | 0.357 | 1.02G/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.35G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.044 | 0.044 | 8.17G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.122 | 0.156 | 2.95G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.379 | 0.412 | 952.27M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.004 | 3.50G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.044 | 0.044 | 8.18G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.335 | 0.338 | 1.08G/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.071 | 0.071 | 5.10G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.010 | 0.010 | 404.12M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.024 | 0.024 | 44.37G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.247 | 4.27G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 2.255 | 2.282 | 465.10M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.720 | 1.726 | 609.70M/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.023 | 0.023 | 46.37G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.416 | 0.418 | 2.52G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.238 | 1.248 | 846.85M/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.007 | 0.007 | 142.20G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.416 | 0.418 | 2.52G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.923 | 0.925 | 1.14G/s | -1 | ok |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.017 | 0.017 | 63.55G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.415 | 0.417 | 2.53G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.927 | 0.944 | 1.13G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 45.07G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.417 | 0.420 | 2.51G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.924 | 0.927 | 1.13G/s | -1 | ok |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 74.094 | 76.842 | 13.50M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 32.618 | 34.151 | 30.66M/s | 0 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.593 | 0.606 | 1.69G/s | 0 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.390 | 0.393 | 2.56G/s | 0 | ok |
| matmul |  | cajeta | stdlib | 40000 | 2.921 | 2.929 | 13.69M/s | 0 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.593 | 0.637 | 1.69G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.391 | 0.394 | 2.55G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.290 | 2.294 | 17.46M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.403 | 0.411 | 2.48G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.642 | 0.646 | 1.56G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.460 | 2.466 | 16.26M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.408 | 0.417 | 2.45G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.390 | 0.395 | 2.57G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 3.367 | 3.385 | 11.88M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.415 | 0.466 | 2.41G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 0.010 | 0.010 | 100.42G/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.070 | 0.074 | 571.33M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.645 | 28.769 | 22.34M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 163.542 | 165.032 | 61/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 3.622 | 3.757 | 27.61K/s | 0 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 26.736 | 27.117 | 23.94M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 159.304 | 160.796 | 63/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.312 | 0.324 | 320.82K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.600 | 23.861 | 27.12M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 158.592 | 159.803 | 63/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.137 | 0.137 | 732.19K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 27.865 | 27.932 | 22.97M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 148.854 | 150.896 | 67/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.318 | 0.330 | 314.58K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 116.331 | 128.041 | 5.50M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4299.140 | 4411.660 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 36.085 | 37.406 | 2.77K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 49.907 | 51.121 | 20.04M/s | 0 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 4.943 | 5.101 | 20.23M/s | 0 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 9.480 | 9.689 | 105.49M/s | 0 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 194.314 | 198.637 | 102.93K/s | 0 | ok |
