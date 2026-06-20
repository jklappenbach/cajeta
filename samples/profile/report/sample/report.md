# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 1.812 | 1.897 | 348.59M/s | 620 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 3.852 | 4.027 | 448.34M/s | 1944 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 9.129 | 9.172 | 246.58M/s | 3524 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.158 | 0.163 | 4.00G/s | 2624 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.380 | 0.397 | 4.54G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.731 | 0.740 | 3.08G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 10.452 | 10.635 | 165.24M/s | 479368 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 11.150 | 13.009 | 154.91M/s | 595212 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 33.103 | 34.781 | 52.18M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.025 | 0.027 | 550.57K/s | 864 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 10.871 | 10.941 | 96.46M/s | 136556 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 11.726 | 12.056 | 89.42M/s | 102420 | ok |
| json-dom |  | rust | serde_json | 631514 | 1.070 | 1.084 | 590.24M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.843 | 0.847 | 748.73M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 2.261 | 2.326 | 763.80M/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.552 | 1.558 | 1.11G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 4.083 | 4.099 | 551.36M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 4.063 | 4.101 | 554.05M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.086 | 0.087 | 7.33G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.116 | 0.116 | 5.45G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.231 | 0.233 | 7.47G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.311 | 0.313 | 5.55G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.238 | 1.241 | 1.82G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.326 | 1.333 | 1.70G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.784 | 4.032 | 166.88M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.047 | 2.661 | 308.46M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 8.150 | 9.495 | 211.93M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 5.226 | 5.822 | 330.48M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 15.770 | 17.136 | 142.74M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 14.345 | 16.757 | 156.92M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.740 | 1.757 | 362.99M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.676 | 0.691 | 933.99M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.896 | 0.899 | 704.94M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.094 | 1.108 | 577.12M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 3.968 | 3.986 | 435.31M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.047 | 2.067 | 843.57M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.642 | 2.686 | 653.79M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 2.951 | 2.986 | 585.23M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 31.724 | 32.537 | 70.96M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.530 | 5.698 | 407.03M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 5.861 | 5.930 | 384.05M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.622 | 8.800 | 261.09M/s | -1 | ok |
| json-dom |  | java | jackson | 631514 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 1727204 |  |  |  | -1 | skipped |
| json-dom |  | java | jackson | 2251051 |  |  |  | -1 | skipped |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 0.970 | 1.006 | 103.07M/s | 45368 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 2.460 | 2.522 | 20.32M/s | 69128 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 11.621 | 12.083 | 2.58M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 3.427 | 3.484 | 29.18M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 1.282 | 1.301 | 31.21M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 4.423 | 4.575 | 11.30M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 3.282 | 3.373 | 6.09M/s | 25016 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 3.094 | 3.123 | 16.16M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | stdlib | 50000 | 3.305 | 3.329 | 15.13M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 3.469 | 3.543 | 14.41M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 5.307 | 5.323 | 9.42M/s | 11736 | ok |
| sort-int64 | random | rust | std-unstable | 50000 | 0.371 | 0.382 | 134.70M/s | -1 | ok |
| sort-int64 | ascending | rust | std-unstable | 50000 | 0.012 | 0.012 | 4.19G/s | -1 | ok |
| sort-int64 | descending | rust | std-unstable | 50000 | 0.015 | 0.015 | 3.35G/s | -1 | ok |
| sort-int64 | dups | rust | std-unstable | 50000 | 0.095 | 0.095 | 526.71M/s | -1 | ok |
| sort-stable-int64 |  | rust | std-stable | 50000 | 0.651 | 0.659 | 76.84M/s | -1 | ok |
| sort-f64 |  | rust | std-unstable | 50000 | 0.828 | 0.837 | 60.41M/s | -1 | ok |
| sort-int64 | random | cpp | std::sort | 50000 | 1.821 | 1.850 | 27.46M/s | -1 | ok |
| sort-int64 | random | cpp | pdqsort | 50000 | 0.726 | 0.733 | 68.88M/s | -1 | ok |
| sort-int64 | ascending | cpp | std::sort | 50000 | 0.259 | 0.261 | 193.01M/s | -1 | ok |
| sort-int64 | ascending | cpp | pdqsort | 50000 | 0.020 | 0.020 | 2.55G/s | -1 | ok |
| sort-int64 | descending | cpp | std::sort | 50000 | 0.150 | 0.151 | 333.02M/s | -1 | ok |
| sort-int64 | descending | cpp | pdqsort | 50000 | 0.041 | 0.041 | 1.22G/s | -1 | ok |
| sort-int64 | dups | cpp | std::sort | 50000 | 0.489 | 0.494 | 102.15M/s | -1 | ok |
| sort-int64 | dups | cpp | pdqsort | 50000 | 0.078 | 0.078 | 644.36M/s | -1 | ok |
| sort-stable-int64 |  | cpp | std::stable_sort | 50000 | 2.021 | 2.027 | 24.75M/s | -1 | ok |
| sort-f64 |  | cpp | std::sort | 50000 | 2.228 | 2.264 | 22.44M/s | -1 | ok |
| sort-int64 | random | go | slices.Sort | 50000 | 2.258 | 2.312 | 22.14M/s | -1 | ok |
| sort-int64 | ascending | go | slices.Sort | 50000 | 0.014 | 0.016 | 3.56G/s | -1 | ok |
| sort-int64 | descending | go | slices.Sort | 50000 | 0.021 | 0.022 | 2.34G/s | -1 | ok |
| sort-int64 | dups | go | slices.Sort | 50000 | 0.106 | 0.125 | 473.85M/s | -1 | ok |
| sort-stable-int64 |  | go | slices.SortStableFunc | 50000 | 6.207 | 6.252 | 8.06M/s | -1 | ok |
| sort-f64 |  | go | slices.Sort | 50000 | 2.751 | 2.767 | 18.18M/s | -1 | ok |
| sort-int64 | random | python | Timsort | 50000 | 7.407 | 7.478 | 6.75M/s | -1 | ok |
| sort-int64 | ascending | python | Timsort | 50000 | 0.104 | 0.106 | 480.70M/s | -1 | ok |
| sort-int64 | descending | python | Timsort | 50000 | 0.110 | 0.111 | 456.09M/s | -1 | ok |
| sort-int64 | dups | python | Timsort | 50000 | 0.787 | 0.792 | 63.57M/s | -1 | ok |
| sort-stable-int64 |  | python | Timsort | 50000 | 7.435 | 7.573 | 6.72M/s | -1 | ok |
| sort-f64 |  | python | Timsort | 50000 | 4.598 | 4.652 | 10.88M/s | -1 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 10.049 | 13.023 | 398.07K/s | 1891392 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.139 | 0.211 | 2.60G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 1.016 | 1.025 | 354.73M/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.256 | 0.260 | 1.41G/s | 17604 | ok |
| string-search |  | rust | std-find | 360448 | 0.023 | 0.023 | 15.69G/s | -1 | ok |
| string-search |  | rust | memchr | 360448 | 0.008 | 0.008 | 44.75G/s | -1 | ok |
| string-replace |  | rust | std-replace | 360448 | 0.111 | 0.112 | 3.26G/s | -1 | ok |
| string-uppercase |  | rust | std-to_uppercase | 360448 | 0.016 | 0.016 | 22.57G/s | -1 | ok |
| string-build-concat |  | rust | String::push_str | 4000 | 0.000 | 0.000 | 10.81G/s | -1 | ok |
| string-search |  | cpp | std::string::find | 360448 | 0.030 | 0.030 | 12.10G/s | -1 | ok |
| string-replace |  | cpp | std find-loop | 360448 | 0.096 | 0.114 | 3.76G/s | -1 | ok |
| string-uppercase |  | cpp | ascii-toupper | 360448 | 0.355 | 0.357 | 1.02G/s | -1 | ok |
| string-build-concat |  | cpp | std::string += | 4000 | 0.002 | 0.002 | 2.35G/s | -1 | ok |
| string-search |  | go | strings.Index | 360448 | 0.050 | 0.050 | 7.26G/s | -1 | ok |
| string-replace |  | go | strings.ReplaceAll | 360448 | 0.123 | 0.172 | 2.92G/s | -1 | ok |
| string-uppercase |  | go | strings.ToUpper | 360448 | 0.377 | 0.387 | 957.19M/s | -1 | ok |
| string-build-concat |  | go | strings.Builder | 4000 | 0.001 | 0.004 | 2.94G/s | -1 | ok |
| string-search |  | python | str.find | 360448 | 0.044 | 0.044 | 8.17G/s | -1 | ok |
| string-replace |  | python | str.replace | 360448 | 0.336 | 0.338 | 1.07G/s | -1 | ok |
| string-uppercase |  | python | str.upper | 360448 | 0.071 | 0.071 | 5.09G/s | -1 | ok |
| string-build-concat |  | python | str.join | 4000 | 0.010 | 0.010 | 402.50M/s | -1 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.023 | 0.024 | 44.71G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.247 | 4.26G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 2.247 | 2.270 | 466.62M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.643 | 1.650 | 638.32M/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.025 | 0.025 | 41.27G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.472 | 0.475 | 2.22G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.402 | 1.404 | 748.04M/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.008 | 0.008 | 125.80G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.470 | 0.473 | 2.23G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 1.046 | 1.046 | 1.00G/s | -1 | ok |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.018 | 0.018 | 59.50G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.468 | 0.471 | 2.24G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 1.040 | 1.041 | 1.01G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 45.33G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.417 | 0.417 | 2.52G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.925 | 0.929 | 1.13G/s | -1 | ok |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skipped |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skipped |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 75.968 | 79.064 | 13.16M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 32.615 | 34.476 | 30.66M/s | 0 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.585 | 0.594 | 1.71G/s | 0 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.391 | 0.393 | 2.56G/s | 0 | ok |
| matmul |  | cajeta | stdlib | 40000 | 2.827 | 2.934 | 14.15M/s | 0 | ok |
| saxpy |  | rust | scalar | 1000000 | 0.588 | 0.613 | 1.70G/s | -1 | ok |
| dot-product |  | rust | scalar | 1000000 | 0.391 | 0.395 | 2.56G/s | -1 | ok |
| matmul |  | rust | scalar | 40000 | 2.300 | 2.311 | 17.40M/s | -1 | ok |
| saxpy |  | cpp | scalar | 1000000 | 0.407 | 0.416 | 2.46G/s | -1 | ok |
| dot-product |  | cpp | scalar | 1000000 | 0.643 | 0.649 | 1.56G/s | -1 | ok |
| matmul |  | cpp | scalar | 40000 | 2.461 | 2.465 | 16.26M/s | -1 | ok |
| saxpy |  | go | scalar | 1000000 | 0.463 | 0.467 | 2.16G/s | -1 | ok |
| dot-product |  | go | scalar | 1000000 | 0.457 | 0.461 | 2.19G/s | -1 | ok |
| matmul |  | go | scalar | 40000 | 3.367 | 3.386 | 11.88M/s | -1 | ok |
| saxpy |  | python | numpy | 1000000 | 0.412 | 0.437 | 2.43G/s | -1 | ok |
| dot-product |  | python | numpy | 1000000 | 0.010 | 0.011 | 98.14G/s | -1 | ok |
| matmul |  | python | numpy | 40000 | 0.075 | 0.079 | 533.40M/s | -1 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.672 | 28.723 | 22.32M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 160.856 | 161.830 | 62/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 3.603 | 3.619 | 27.75K/s | 0 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 50.023 | 50.449 | 19.99M/s | 0 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 5.044 | 5.052 | 19.83M/s | 0 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 9.477 | 9.497 | 105.52M/s | 0 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 177.467 | 181.260 | 112.70K/s | 0 | ok |
