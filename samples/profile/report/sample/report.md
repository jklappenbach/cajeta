# Cajeta profile — benchmark report

Reference machine: **cpu_model** AMD RYZEN AI MAX+ 395 w/ Radeon 8060S · **cpu_cores** 32 · **kernel** Linux 7.0.0-22-generic · **os** Ubuntu 26.04 LTS · **governor** performance · **mem_total_kb** 64566284

## codec

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| json-tokenize | twitter | cajeta | stdlib | 631514 | 1.504 | 1.520 | 419.83M/s | 620 | ok |
| json-tokenize | citm_catalog | cajeta | stdlib | 1727204 | 3.654 | 4.041 | 472.68M/s | 2120 | ok |
| json-tokenize | canada | cajeta | stdlib | 2251051 | 8.374 | 8.688 | 268.82M/s | 3708 | ok |
| json-bind-skip | twitter | cajeta | stdlib | 631514 | 0.153 | 0.155 | 4.12G/s | 2812 | ok |
| json-bind-skip | citm_catalog | cajeta | stdlib | 1727204 | 0.383 | 0.399 | 4.51G/s | 4 | ok |
| json-bind-skip | canada | cajeta | stdlib | 2251051 | 0.736 | 0.751 | 3.06G/s | 4 | ok |
| json-dom | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-dom | citm_catalog | cajeta | stdlib | 1727204 | 10.119 | 10.412 | 170.69M/s | 479368 | ok |
| json-dom | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-serialize | citm_catalog | cajeta | stdlib | 1727204 | 12.298 | 13.663 | 140.44M/s | 595212 | ok |
| json-serialize | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | twitter | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-roundtrip | citm_catalog | cajeta | stdlib | 1727204 | 32.329 | 34.300 | 53.43M/s | 1074740 | ok |
| json-roundtrip | canada | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| json-conformance |  | cajeta | stdlib | 14 | 0.025 | 0.026 | 565.73K/s | 864 | ok |
| base64-encode |  | cajeta | stdlib | 1048576 | 10.772 | 10.831 | 97.35M/s | 136556 | ok |
| base64-decode |  | cajeta | stdlib | 1048576 | 11.983 | 12.038 | 87.50M/s | 102420 | ok |
| json-dom |  | rust | serde_json | 631514 | 1.057 | 1.072 | 597.32M/s | -1 | ok |
| json-dom |  | rust | simd-json | 631514 | 0.713 | 0.722 | 886.28M/s | -1 | ok |
| json-dom |  | rust | serde_json | 1727204 | 2.270 | 2.293 | 760.87M/s | -1 | ok |
| json-dom |  | rust | simd-json | 1727204 | 1.578 | 1.587 | 1.09G/s | -1 | ok |
| json-dom |  | rust | serde_json | 2251051 | 4.135 | 4.147 | 544.37M/s | -1 | ok |
| json-dom |  | rust | simd-json | 2251051 | 4.140 | 4.158 | 543.80M/s | -1 | ok |
| json-dom |  | cpp | simdjson | 631514 | 0.099 | 0.100 | 6.38G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 631514 | 0.139 | 0.141 | 4.53G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 1727204 | 0.262 | 0.263 | 6.59G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 1727204 | 0.358 | 0.358 | 4.83G/s | -1 | ok |
| json-dom |  | cpp | simdjson | 2251051 | 1.238 | 1.419 | 1.82G/s | -1 | ok |
| json-dom |  | cpp | yyjson | 2251051 | 1.325 | 1.353 | 1.70G/s | -1 | ok |
| json-dom |  | go | encoding/json | 631514 | 3.657 | 4.153 | 172.69M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 631514 | 2.046 | 2.770 | 308.73M/s | -1 | ok |
| json-dom |  | go | encoding/json | 1727204 | 7.758 | 9.569 | 222.65M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 1727204 | 4.969 | 5.854 | 347.61M/s | -1 | ok |
| json-dom |  | go | encoding/json | 2251051 | 15.205 | 17.212 | 148.05M/s | -1 | ok |
| json-dom |  | go | goccy/go-json | 2251051 | 12.660 | 15.097 | 177.81M/s | -1 | ok |
| json-dom |  | python | json | 631514 | 2.040 | 2.058 | 309.56M/s | -1 | ok |
| json-dom |  | python | orjson | 631514 | 0.681 | 0.700 | 927.45M/s | -1 | ok |
| json-dom |  | python | msgspec | 631514 | 0.906 | 0.911 | 697.29M/s | -1 | ok |
| json-dom |  | python | ujson | 631514 | 1.153 | 1.171 | 547.52M/s | -1 | ok |
| json-dom |  | python | json | 1727204 | 4.087 | 4.162 | 422.62M/s | -1 | ok |
| json-dom |  | python | orjson | 1727204 | 2.089 | 2.104 | 826.65M/s | -1 | ok |
| json-dom |  | python | msgspec | 1727204 | 2.685 | 2.718 | 643.38M/s | -1 | ok |
| json-dom |  | python | ujson | 1727204 | 3.033 | 3.056 | 569.44M/s | -1 | ok |
| json-dom |  | python | json | 2251051 | 31.960 | 32.635 | 70.43M/s | -1 | ok |
| json-dom |  | python | orjson | 2251051 | 5.712 | 5.800 | 394.10M/s | -1 | ok |
| json-dom |  | python | msgspec | 2251051 | 5.653 | 6.051 | 398.22M/s | -1 | ok |
| json-dom |  | python | ujson | 2251051 | 8.790 | 8.832 | 256.09M/s | -1 | ok |
| json-dom |  | java | jackson | 631514 |  |  |  | -1 | skip |
| json-dom |  | java | jackson | 1727204 |  |  |  | -1 | skip |
| json-dom |  | java | jackson | 2251051 |  |  |  | -1 | skip |

## collection

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| arraylist-append |  | cajeta | stdlib | 100000 | 1.023 | 1.046 | 97.80M/s | 45368 | ok |
| hashmap-int |  | cajeta | stdlib | 50000 | 2.562 | 2.584 | 19.52M/s | 69128 | ok |
| hashmap-string |  | cajeta | stdlib | 30000 | 11.868 | 12.202 | 2.53M/s | 338916 | ok |
| hashset-dedup |  | cajeta | stdlib | 100000 | 3.340 | 3.380 | 29.94M/s | 46104 | ok |
| linkedlist-insert-traverse |  | cajeta | stdlib | 40000 | 1.312 | 1.327 | 30.48M/s | 56268 | ok |
| heap-sort |  | cajeta | stdlib | 50000 | 4.598 | 4.624 | 10.87M/s | 13684 | ok |
| redblacktree-insert-lookup |  | cajeta | stdlib | 20000 | 3.327 | 3.413 | 6.01M/s | 25016 | ok |

## sort

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| sort-int64 | random | cajeta | stdlib | 50000 | 3.106 | 3.124 | 16.10M/s | 11752 | ok |
| sort-int64 | ascending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | descending | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-int64 | dups | cajeta | stdlib | -1 |  |  |  | -1 | skipped |
| sort-f64 |  | cajeta | stdlib | 50000 | 3.316 | 3.339 | 15.08M/s | 11752 | ok |
| sort-stable-int64 |  | cajeta | stdlib | 50000 | 3.507 | 3.520 | 14.26M/s | 23452 | ok |
| binary-search |  | cajeta | stdlib | 50000 | 5.651 | 5.667 | 8.85M/s | 11736 | ok |

## string

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| string-build-concat |  | cajeta | stdlib | 4000 | 9.541 | 12.363 | 419.25K/s | 1891392 | ok |
| string-search |  | cajeta | stdlib | 360448 | 0.211 | 0.212 | 1.71G/s | 0 | ok |
| string-replace |  | cajeta | stdlib | 360448 | 1.016 | 1.018 | 354.85M/s | 18800 | ok |
| string-uppercase |  | cajeta | stdlib | 360448 | 0.256 | 0.261 | 1.41G/s | 17604 | ok |

## hash

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| xxhash3 |  | cajeta | stdlib | 1048576 | 0.024 | 0.024 | 44.27G/s | 0 | ok |
| siphash |  | cajeta | stdlib | 1048576 | 0.246 | 0.247 | 4.25G/s | 0 | ok |
| sha256 |  | cajeta | stdlib | 1048576 | 2.260 | 2.274 | 463.93M/s | 0 | ok |
| md5 |  | cajeta | stdlib | 1048576 | 1.647 | 1.653 | 636.64M/s | 0 | ok |

## stream

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| stream-filter-map-reduce |  | cajeta | stdlib | 1000000 | 81.597 | 82.384 | 12.26M/s | 0 | ok |
| stream-parallel-reduce |  | cajeta | stdlib | 1000000 | 33.469 | 35.744 | 29.88M/s | 0 | ok |

## math

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| saxpy |  | cajeta | stdlib | 1000000 | 0.589 | 0.600 | 1.70G/s | 0 | ok |
| dot-product |  | cajeta | stdlib | 1000000 | 0.390 | 0.393 | 2.56G/s | 0 | ok |
| matmul |  | cajeta | stdlib | 40000 | 2.909 | 2.927 | 13.75M/s | 0 | ok |

## clbg

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| clbg-mandelbrot |  | cajeta | stdlib | 640000 | 28.635 | 29.054 | 22.35M/s | 0 | ok |
| clbg-fannkuch-redux |  | cajeta | stdlib | 10 | 162.194 | 163.339 | 62/s | 0 | ok |
| clbg-spectral-norm |  | cajeta | stdlib | 100 | 3.476 | 4.079 | 28.77K/s | 0 | ok |

## time

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| time-instant-arith |  | cajeta | stdlib | 1000000 | 50.087 | 53.217 | 19.97M/s | 0 | ok |
| time-localdate-arith |  | cajeta | stdlib | 100000 | 4.982 | 5.147 | 20.07M/s | 0 | ok |

## concurrent

| benchmark | variant | lang | library | input | min (ms) | median (ms) | rate | ws (KB) | status |
|---|---|---|---|---|---|---|---|---|---|
| atomic-fetchadd |  | cajeta | stdlib | 1000000 | 9.408 | 9.637 | 106.29M/s | 0 | ok |
| task-spawn-await |  | cajeta | stdlib | 20000 | 180.831 | 180.983 | 110.60K/s | 0 | ok |
