#!/usr/bin/env python3
# Python "hello-metric" runner — emits one schema-conformant CSV row (columns.txt
# order). Real per-area Python runners (json/orjson/msgspec, numpy) extend this
# contract in Units 5+.
import os
import time

run_id = os.environ.get("PROFILE_RUN_ID", "local")
ts = os.environ.get("PROFILE_RUN_TS", "")
ver = os.environ.get("PROFILE_LANG_VERSION", "")

t0 = time.perf_counter_ns()
acc = 0
for i in range(1_000_000):
    acc += i
ns = time.perf_counter_ns() - t0
if acc < 0:
    print("", end="")

print(
    f"1,{run_id},{ts},hello,scaffold,,-1,,1000000,python,{ver},stdlib,,-OO,0,1,"
    f"{ns},{ns},{ns},{ns},,,-1,-1,-1,-1,-1,ok,true,,"
)
