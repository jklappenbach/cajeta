# ltm-pager-frame-lookup-linear — `LtmPager` scans its whole frame table on every page touch (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 6 on
> v0.46.3.

## 1. Definition
`LtmPager.findFrame` and `acquireFrame` scan the frame table linearly on
every page touch, so an `LtmBPlusTree` insert costs 44 µs at 4096 frames and
61 µs at 1024 frames (200k entries), and the catalog's 256 MB budget (65536
frames) would take hours for one trial. The benchmark runs at a tenth of its
page shape by default.

## 2. Use cases
- **2.1** As `LtmBPlusTree`, when a page is touched, then the frame is found
  in O(1) expected (a hash from page id to frame) and a free frame in O(1)
  (a free list or clock hand), independent of the budget.
- **2.2** As the test suite, then a test inserts 1e6 keys under a 256 MB
  budget within a bound that scales linearly in keys, not in frames.

## 3. Acceptance
`collection/ltm-insert` runs at its page shape (1e7 keys, 256 MB) in
minutes, and the default sizes in the benchmark return to the page's.
