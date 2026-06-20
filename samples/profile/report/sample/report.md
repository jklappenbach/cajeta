# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 1.395 | 1.424 | 452.54M/s | 620 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 3.630 | 3.827 | 475.87M/s | 1748 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 8.476 | 9.545 | 265.59M/s | 3444 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.155 | 0.157 | 4.06G/s | 2544 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.378 | 0.398 | 4.57G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.686 | 0.789 | 3.28G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 10.001 | 10.425 | 172.71M/s | 479368 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 13.273 | 14.508 | 130.13M/s | 595212 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 33.959 | 34.797 | 50.86M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.025 | 0.026 | 560.74K/s | 864 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 10.541 | 10.687 | 99.48M/s | 136556 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 12.120 | 12.410 | 86.52M/s | 102420 | ok |
| json-dom |  | rust | serde_json | 631514 | 0.999 | 1.011 | 632.17M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.719 | 0.740 | 878.28M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 2.272 | 2.325 | 760.15M/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.607 | 1.645 | 1.07G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 4.182 | 4.272 | 538.26M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 4.298 | 4.469 | 523.70M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.091 | 0.094 | 6.91G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.126 | 0.129 | 5.01G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.249 | 0.250 | 6.94G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.309 | 0.332 | 5.60G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.237 | 1.243 | 1.82G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.337 | 1.342 | 1.68G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.685 | 3.988 | 171.39M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 1.985 | 2.454 | 318.18M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 8.786 | 9.564 | 196.58M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 4.464 | 5.587 | 386.89M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 15.568 | 16.238 | 144.59M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 14.148 | 15.604 | 159.10M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 1.759 | 1.779 | 358.92M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.689 | 0.709 | 916.45M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.896 | 0.904 | 704.97M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.126 | 1.133 | 560.94M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 4.138 | 4.235 | 417.39M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.060 | 2.097 | 838.57M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.656 | 2.807 | 650.30M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 3.035 | 3.060 | 569.18M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 32.101 | 32.495 | 70.12M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.698 | 5.823 | 395.09M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 5.874 | 6.016 | 383.23M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.581 | 8.834 | 262.32M/s | -1 | ok |
| json-dom |  | java | jackson | 631514 |  |  |  | -1 | skip |
| json-dom |  | java | jackson | 1727204 |  |  |  | -1 | skip |
| json-dom |  | java | jackson | 2251051 |  |  |  | -1 | skip |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 1.004 | 1.010 | 99.60M/s | 45368 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 2.555 | 2.610 | 19.57M/s | 69128 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 12.019 | 12.496 | 2.50M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 3.261 | 3.312 | 30.67M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 1.263 | 1.286 | 31.68M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 4.597 | 4.633 | 10.88M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 3.420 | 3.465 | 5.85M/s | 25016 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 3.110 | 3.122 | 16.08M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | stdlib | 50000 | 3.330 | 3.360 | 15.02M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 3.526 | 3.561 | 14.18M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 5.275 | 5.345 | 9.48M/s | 11736 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 9.842 | 12.524 | 406.42K/s | 1891392 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.211 | 0.211 | 1.71G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 1.015 | 1.019 | 355.15M/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.256 | 0.261 | 1.41G/s | 17604 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.023 | 0.024 | 44.71G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.246 | 4.27G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 2.258 | 2.270 | 464.36M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.645 | 1.649 | 637.48M/s | 0 | ok |
| xxhash3 |  | rust | xxhash-rust | 1048576 | 0.024 | 0.024 | 43.07G/s | -1 | ok |
| sha256 |  | rust | sha2 | 1048576 | 0.448 | 0.450 | 2.34G/s | -1 | ok |
| md5 |  | rust | md-5 | 1048576 | 1.330 | 1.336 | 788.28M/s | -1 | ok |
| xxhash3 |  | cpp | xxHash | 1048576 | 0.007 | 0.007 | 144.75G/s | -1 | ok |
| sha256 |  | cpp | openssl | 1048576 | 0.449 | 0.451 | 2.34G/s | -1 | ok |
| md5 |  | cpp | openssl | 1048576 | 0.995 | 1.000 | 1.05G/s | -1 | ok |
| xxhash3 |  | go | zeebo/xxh3 | 1048576 | 0.017 | 0.017 | 61.53G/s | -1 | ok |
| sha256 |  | go | crypto/sha256 | 1048576 | 0.420 | 0.425 | 2.49G/s | -1 | ok |
| md5 |  | go | crypto/md5 | 1048576 | 0.927 | 0.930 | 1.13G/s | -1 | ok |
| xxhash3 |  | python | xxhash | 1048576 | 0.023 | 0.023 | 45.70G/s | -1 | ok |
| sha256 |  | python | hashlib | 1048576 | 0.418 | 0.419 | 2.51G/s | -1 | ok |
| md5 |  | python | hashlib | 1048576 | 0.927 | 0.930 | 1.13G/s | -1 | ok |
| siphash |  | rust | siphash | 1048576 |  |  |  | -1 | skip |
| siphash |  | cpp | siphash | 1048576 |  |  |  | -1 | skip |
| siphash |  | go | siphash | 1048576 |  |  |  | -1 | skip |
| siphash |  | python | siphash | 1048576 |  |  |  | -1 | skip |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 73.856 | 78.153 | 13.54M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 32.638 | 33.890 | 30.64M/s | 0 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.595 | 0.610 | 1.68G/s | 0 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.391 | 0.395 | 2.56G/s | 0 | ok |
| matmul |  | cajeta | stdlib | 40000 | 2.889 | 2.933 | 13.85M/s | 0 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.634 | 28.827 | 22.35M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 161.960 | 163.806 | 62/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 3.734 | 3.741 | 26.78K/s | 0 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 49.395 | 50.454 | 20.24M/s | 0 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 4.907 | 5.041 | 20.38M/s | 0 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 9.409 | 9.870 | 106.28M/s | 0 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 185.637 | 192.360 | 107.74K/s | 0 | ok |
