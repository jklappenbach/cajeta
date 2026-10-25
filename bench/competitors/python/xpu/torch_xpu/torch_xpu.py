#!/usr/bin/env python3
"""xpu/ PyTorch competitor (benchmark-reference unit 14, spec §6.13): the
Python-user line, torch.cuda on Phoenix (the ROCm wheel's torch.cuda on Proton,
backend hip). The same sizes, inputs and checks as the Cajeta rows:

  device-saxpy          y.add_(x, alpha=2), 2^26 float32, exact against r launches
  wave-reduce           x.sum(), 2^26 float32, within 1e-5 of the float64 host sum
  scan                  torch.cumsum(x, 0, dtype=int32), 2^26 int32, every element
  gemm-f64/f32/f16/bf16 a @ b at 4096 (TF32 off), 512 entries exact
  host-device-transfer  h.to("cuda") then .cpu(), 2^28 float32 pageable, checksum
  kernelbuffer-alloc-free  1e4 x torch.empty(16M float32) + del (caching allocator)

One synchronized iteration per trial (torch.cuda.synchronize), timed with
time.perf_counter_ns as the Cajeta harness times ours. Prints v1 rows.

CAJETA_BENCH_NO_GPU=1: every row is skipped and torch is never imported, so no
torch.cuda call is made. PROFILE_BUILD_ONLY is the driver's (py_compile only).
"""
import os
import sys
import time

IDS = ["device-saxpy", "wave-reduce", "scan", "gemm-f64", "gemm-f32", "gemm-f16", "gemm-bf16",
       "host-device-transfer", "kernelbuffer-alloc-free"]
VARIANT = {"gemm-f64": "n4096", "gemm-f32": "n4096", "gemm-f16": "n4096", "gemm-bf16": "n4096"}


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def row(bid, size, ver, warm, trials, st, tp, unit, status, ok, note, backend):
    mn, med, mean, p95 = st
    return (f"1,{env('PROFILE_RUN_ID', 'local')},{env('PROFILE_RUN_TS', '')},{bid},xpu,,-1,,{size},python,"
            f"{ver},torch,{ver},,{warm},{trials},{mn},{med},{mean},{p95},{tp},{unit},-1,-1,-1,-1,-1,"
            f"{status},{ok},{note.replace(',', ';')},{VARIANT.get(bid, '')},{backend}")


def skip(bid, reason, ver="", backend="cuda"):
    return row(bid, -1, ver, 0, 0, (-1, -1, -1, -1), "", "", "skipped", "", reason, backend)


def main():
    backend = "hip" if env("CAJETA_XPU_BACKEND", "cuda") in ("hip", "amdgpu") else "cuda"
    if env("CAJETA_BENCH_NO_GPU", "") in ("1", "true"):
        for b in IDS:
            print(skip(b, "GPU held by another session (CAJETA_BENCH_NO_GPU=1)", backend=backend))
        return 0
    try:
        import torch
    except Exception as e:  # noqa: BLE001
        for b in IDS:
            print(skip(b, f"torch import failed: {type(e).__name__}", backend=backend))
        return 0
    ver = torch.__version__
    if not torch.cuda.is_available():
        for b in IDS:
            print(skip(b, "torch.cuda.is_available() is False", ver, backend))
        return 0
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    dev = "cuda"
    warm = int(env("PROFILE_WARMUP", "3"))
    trials = int(env("PROFILE_TRIALS", "20"))
    sync = torch.cuda.synchronize

    def measure(run):
        for _ in range(warm):
            run()
        s = []
        for _ in range(trials):
            t0 = time.perf_counter_ns()
            run()
            s.append(time.perf_counter_ns() - t0)
        s.sort()
        n = len(s)
        return s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]

    def emit(bid, size, st, unit, ok, note):
        tp = f"{size * 1e9 / st[1]:.3f}" if st[1] > 0 else ""
        print(row(bid, size, ver, warm, trials, st, tp, unit, "ok" if ok else "invalid",
                  "true" if ok else "false", note, backend), flush=True)

    only = sys.argv[1:] or IDS
    n26 = 1 << 26
    if "device-saxpy" in only:
        x = (torch.arange(n26, device=dev, dtype=torch.int64) % 16).to(torch.float32)
        y = torch.ones(n26, device=dev, dtype=torch.float32)
        runs = [0]

        def saxpy():
            y.add_(x, alpha=2.0)
            sync()
            runs[0] += 1
        st = measure(saxpy)
        ok = bool(torch.equal(y, 1.0 + (2.0 * runs[0]) * x))
        emit("device-saxpy", 12 * n26, st, "B/s", ok, "y.add_(x; alpha=2)")
        del x, y
    if "wave-reduce" in only:
        x = (torch.arange(n26, device=dev, dtype=torch.int64) % 8).to(torch.float32) * 0.125
        want = float(x.double().sum().item())
        out = [None]

        def red():
            out[0] = x.sum()
            sync()
        st = measure(red)
        ok = abs(float(out[0].item()) - want) <= 1e-5 * want
        emit("wave-reduce", 4 * n26, st, "B/s", ok, "torch.sum float32")
        del x
    if "scan" in only:
        x = (torch.arange(n26, device=dev, dtype=torch.int64) % 5 - 2).to(torch.int32)
        ref = torch.cumsum(x.cpu().to(torch.int64), 0).to(torch.int32)
        out = [None]

        def scan():
            out[0] = torch.cumsum(x, 0, dtype=torch.int32)
            sync()
        st = measure(scan)
        ok = bool(torch.equal(out[0].cpu(), ref))
        emit("scan", 8 * n26, st, "B/s", ok, "torch.cumsum int32")
        del x
    for bid, dt in (("gemm-f64", torch.float64), ("gemm-f32", torch.float32),
                    ("gemm-f16", torch.float16), ("gemm-bf16", torch.bfloat16)):
        if bid not in only:
            continue
        n = 4096
        i = torch.arange(n * n, dtype=torch.int64)
        if bid in ("gemm-f64", "gemm-f16"):
            a_h = torch.eye(n, dtype=torch.float64)
            b_h = (i % 100).to(torch.float64).reshape(n, n)
        else:
            a_h = ((i // 7) % 4).to(torch.float64).reshape(n, n)
            b_h = ((i // 3) % 5).to(torch.float64).reshape(n, n)
        a = a_h.to(dev, dt)
        b = b_h.to(dev, dt)
        out = [None]

        def mm():
            out[0] = a @ b
            sync()
        st = measure(mm)
        c = out[0].double().cpu()
        ok = True
        for s in range(512):
            r, cc = (s * 37 + 11) % n, (s * 101 + 5) % n
            if float(c[r, cc]) != float((a_h[r, :] * b_h[:, cc]).sum()):
                ok = False
                break
        emit(bid, 2 * n ** 3, st, "flop/s", ok, f"torch.matmul {dt} (TF32 off)")
        del a, b, out
    if "host-device-transfer" in only:
        n = 1 << 28
        h = (torch.arange(n, dtype=torch.int64) % 1000).to(torch.float32)
        want = float(h.double().sum())
        back = [None]

        def xfer():
            back[0] = h.to(dev).cpu()
        st = measure(xfer)
        ok = float(back[0].double().sum()) == want
        emit("host-device-transfer", 8 * n, st, "B/s", ok, ".to(cuda) + .cpu() pageable")
        del h
    if "kernelbuffer-alloc-free" in only:
        cnt = [0, 0]

        def alloc():
            for _ in range(10000):
                t = torch.empty(16777216, device=dev, dtype=torch.float32)
                if t.numel() == 16777216:
                    cnt[0] += 1
                del t
            cnt[1] += 1
        st = measure(alloc)
        ok = cnt[1] > 0 and cnt[0] == cnt[1] * 10000
        emit("kernelbuffer-alloc-free", 10000, st, "allocs/s", ok, "torch.empty + del (caching allocator)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
