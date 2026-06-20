# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 1.562 | 1.587 | 404.29M/s | 620 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 3.695 | 3.843 | 467.45M/s | 2064 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 8.974 | 9.104 | 250.85M/s | 3768 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.158 | 0.165 | 4.00G/s | 2868 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.355 | 0.373 | 4.86G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.728 | 0.734 | 3.09G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 10.853 | 11.470 | 159.15M/s | 479368 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 12.256 | 14.330 | 140.93M/s | 595212 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 34.314 | 35.845 | 50.34M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.024 | 0.026 | 571.52K/s | 864 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 10.678 | 11.567 | 98.20M/s | 136556 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 11.658 | 11.786 | 89.94M/s | 102420 | ok |
| json-dom |  | rust | serde_json | 631514 | 1.031 | 1.166 | 612.32M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.720 | 0.728 | 877.50M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 2.255 | 2.265 | 766.04M/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.569 | 1.580 | 1.10G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 4.157 | 4.623 | 541.45M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 4.072 | 4.095 | 552.85M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.099 | 0.104 | 6.40G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.144 | 0.144 | 4.39G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.273 | 0.275 | 6.33G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.376 | 0.381 | 4.59G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.241 | 1.507 | 1.81G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.325 | 1.342 | 1.70G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.489 | 4.206 | 181.03M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.264 | 2.717 | 278.95M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 8.326 | 10.075 | 207.45M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 4.742 | 5.890 | 364.26M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 15.454 | 16.721 | 145.66M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 13.556 | 15.449 | 166.06M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.746 | 1.762 | 361.60M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.693 | 0.713 | 910.81M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.925 | 0.931 | 683.01M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.144 | 1.168 | 551.83M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 3.944 | 4.508 | 437.97M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.050 | 2.064 | 842.52M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.664 | 2.687 | 648.47M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 2.981 | 2.996 | 579.46M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 32.386 | 33.039 | 69.51M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.542 | 5.586 | 406.15M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 5.835 | 6.189 | 385.79M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.635 | 8.808 | 260.69M/s | -1 | ok |
| json-dom |  | java | jackson | 631514 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 1727204 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 2251051 |  |  |  | -1 | skipped |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 0.989 | 1.019 | 101.14M/s | 45368 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 2.586 | 2.633 | 19.33M/s | 69128 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 12.011 | 12.294 | 2.50M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 3.330 | 3.409 | 30.03M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 1.324 | 1.355 | 30.20M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 4.426 | 4.562 | 11.30M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 3.338 | 3.408 | 5.99M/s | 25016 | ok |
| hashmap-int |  | rust | std-HashMap | 50000 | 0.828 | 0.891 | 60.42M/s | -1 | ok |
| hashmap-int |  | rust | ahash | 50000 | 0.276 | 0.277 | 181.34M/s | -1 | ok |
| hashmap-string |  | rust | std-HashMap | 30000 | 3.136 | 4.201 | 9.57M/s | -1 | ok |
| hashmap-string |  | rust | ahash | 30000 | 2.856 | 2.877 | 10.51M/s | -1 | ok |
| hashset-dedup |  | rust | std-HashSet | 100000 | 1.226 | 1.235 | 81.54M/s | -1 | ok |
| arraylist-append |  | rust | Vec | 100000 | 0.025 | 0.025 | 4.08G/s | -1 | ok |
| hashmap-int |  | cpp | std::unordered_map | 50000 | 0.802 | 0.809 | 62.38M/s | -1 | ok |
| hashmap-int |  | cpp | ankerl::unordered_dense | 50000 | 0.209 | 0.210 | 239.22M/s | -1 | ok |
| hashmap-string |  | cpp | std::unordered_map | 30000 | 2.670 | 2.692 | 11.24M/s | -1 | ok |
| hashmap-string |  | cpp | ankerl::unordered_dense | 30000 | 1.477 | 1.524 | 20.30M/s | -1 | ok |
| hashset-dedup |  | cpp | std::unordered_set | 100000 | 0.941 | 0.964 | 106.26M/s | -1 | ok |
| arraylist-append |  | cpp | std::vector | 100000 | 0.035 | 0.035 | 2.87G/s | -1 | ok |
| hashmap-int |  | go | map | 50000 | 0.783 | 0.938 | 63.87M/s | -1 | ok |
| hashmap-string |  | go | map | 30000 | 2.879 | 3.387 | 10.42M/s | -1 | ok |
| hashset-dedup |  | go | map-set | 100000 | 1.829 | 2.021 | 54.66M/s | -1 | ok |
| arraylist-append |  | go | slice | 100000 | 0.361 | 0.552 | 276.64M/s | -1 | ok |
| hashmap-int |  | python | dict | 50000 | 3.593 | 3.634 | 13.92M/s | -1 | ok |
| hashmap-string |  | python | dict | 30000 | 6.113 | 6.178 | 4.91M/s | -1 | ok |
| hashset-dedup |  | python | set | 100000 | 3.820 | 3.851 | 26.18M/s | -1 | ok |
| arraylist-append |  | python | list | 100000 | 1.691 | 1.939 | 59.14M/s | -1 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 3.054 | 3.065 | 16.37M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | stdlib | 50000 | 3.276 | 3.326 | 15.26M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 3.458 | 3.502 | 14.46M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 5.287 | 5.414 | 9.46M/s | 11736 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.340 | 0.373 | 147.05M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.010 | 0.010 | 5.10G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.013 | 0.014 | 3.71G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.086 | 0.086 | 584.45M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.581 | 0.590 | 86.01M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.739 | 0.746 | 67.68M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.823 | 1.863 | 27.43M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.713 | 0.721 | 70.11M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.264 | 0.264 | 189.21M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.55G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.158 | 0.158 | 316.82M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.041 | 1.22G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.489 | 0.501 | 102.29M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.077 | 0.077 | 647.37M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.034 | 2.045 | 24.58M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.256 | 2.273 | 22.16M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.258 | 2.396 | 22.14M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.014 | 0.016 | 3.55G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.021 | 0.021 | 2.33G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.076 | 0.085 | 655.45M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.128 | 6.542 | 8.16M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.772 | 2.800 | 18.04M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.356 | 7.573 | 6.80M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.105 | 0.109 | 474.70M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.110 | 0.111 | 454.48M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.799 | 0.804 | 62.55M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.591 | 7.630 | 6.59M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.558 | 4.611 | 10.97M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 9.749 | 12.666 | 410.31K/s | 1891392 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.211 | 0.211 | 1.71G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 1.017 | 1.021 | 354.35M/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.258 | 0.262 | 1.40G/s | 17604 | ok |
| string-search |  | rust | std-find | 360448 | 0.022 | 0.029 | 16.65G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.008 | 0.008 | 47.46G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.104 | 0.104 | 3.46G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.015 | 0.015 | 23.89G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 11.43G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.034 | 0.034 | 10.68G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.110 | 0.110 | 3.28G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.402 | 0.407 | 896.75M/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.18G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.047 | 0.047 | 7.68G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.123 | 0.196 | 2.93G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.377 | 0.431 | 956.94M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.002 | 4.25G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.050 | 0.050 | 7.17G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.380 | 0.383 | 947.56M/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.080 | 0.080 | 4.50G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.011 | 0.011 | 363.27M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.023 | 0.023 | 44.73G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.246 | 4.26G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 2.246 | 2.263 | 466.94M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.642 | 1.647 | 638.51M/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.038 | 0.038 | 27.44G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.484 | 0.490 | 2.16G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.241 | 1.243 | 844.94M/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.007 | 0.008 | 140.11G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.417 | 0.420 | 2.52G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.925 | 0.927 | 1.13G/s | -1 | ok |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.018 | 0.018 | 57.73G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.478 | 0.494 | 2.19G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.924 | 0.928 | 1.14G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.024 | 0.026 | 44.22G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.417 | 0.420 | 2.52G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.926 | 0.929 | 1.13G/s | -1 | ok |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 74.054 | 77.788 | 13.50M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 33.459 | 35.395 | 29.89M/s | 0 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.596 | 0.608 | 1.68G/s | 0 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.391 | 0.394 | 2.56G/s | 0 | ok |
| matmul |  | cajeta | stdlib | 40000 | 2.871 | 2.933 | 13.93M/s | 0 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.617 | 0.629 | 1.62G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.392 | 0.392 | 2.55G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.291 | 2.298 | 17.46M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.448 | 0.454 | 2.23G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.642 | 0.645 | 1.56G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.456 | 2.463 | 16.28M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.401 | 0.408 | 2.49G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.391 | 0.394 | 2.56G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 3.385 | 3.477 | 11.82M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.455 | 0.466 | 2.20G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 4.000 | 5.998 | 250.00M/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.077 | 0.080 | 517.22M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.665 | 28.735 | 22.33M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 160.607 | 163.463 | 62/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 3.804 | 3.827 | 26.29K/s | 0 | ok |
| clbg-mandelbrot |  | rust | scalar | 640000 | 26.752 | 26.814 | 23.92M/s | -1 | ok |
| clbg-fannkuch-redux |  | rust | scalar | 10 | 158.872 | 160.719 | 63/s | -1 | ok |
| clbg-spectral-norm |  | rust | scalar | 100 | 0.312 | 0.323 | 320.28K/s | -1 | ok |
| clbg-mandelbrot |  | cpp | scalar | 640000 | 23.572 | 23.631 | 27.15M/s | -1 | ok |
| clbg-fannkuch-redux |  | cpp | scalar | 10 | 155.316 | 157.064 | 64/s | -1 | ok |
| clbg-spectral-norm |  | cpp | scalar | 100 | 0.136 | 0.137 | 733.10K/s | -1 | ok |
| clbg-mandelbrot |  | go | scalar | 640000 | 27.836 | 27.896 | 22.99M/s | -1 | ok |
| clbg-fannkuch-redux |  | go | scalar | 10 | 148.950 | 149.371 | 67/s | -1 | ok |
| clbg-spectral-norm |  | go | scalar | 100 | 0.326 | 0.330 | 307.04K/s | -1 | ok |
| clbg-mandelbrot |  | python | numpy | 640000 | 118.988 | 124.350 | 5.38M/s | -1 | ok |
| clbg-fannkuch-redux |  | python | cpython | 10 | 4278.845 | 4353.315 | 2/s | -1 | ok |
| clbg-spectral-norm |  | python | cpython | 100 | 36.468 | 37.119 | 2.74K/s | -1 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 49.674 | 50.839 | 20.13M/s | 0 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 5.010 | 5.075 | 19.96M/s | 0 | ok |
| time-instant-arith |  | rust | chrono | 1000000 | 6.846 | 6.861 | 146.07M/s | -1 | ok |
| time-localdate-arith |  | rust | chrono | 100000 | 0.561 | 0.570 | 178.18M/s | -1 | ok |
| time-instant-arith |  | cpp | std::chrono | 1000000 | 0.112 | 0.112 | 8.96G/s | -1 | ok |
| time-localdate-arith |  | cpp | std::chrono | 100000 | 0.011 | 0.011 | 8.94G/s | -1 | ok |
| time-instant-arith |  | go | time | 1000000 | 2.938 | 2.970 | 340.43M/s | -1 | ok |
| time-localdate-arith |  | go | time | 100000 | 1.513 | 1.517 | 66.09M/s | -1 | ok |
| time-instant-arith |  | python | datetime | 1000000 | 351.576 | 356.047 | 2.84M/s | -1 | ok |
| time-localdate-arith |  | python | datetime | 100000 | 17.267 | 17.445 | 5.79M/s | -1 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 9.644 | 9.662 | 103.69M/s | 0 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 172.030 | 182.180 | 116.26K/s | 0 | ok |
