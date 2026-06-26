#!/usr/bin/env python3
# GPU matmul competitor: dense N*N float64 C = A @ B on the GPU via PyTorch ROCm
# (gfx1151 / HIP). Mirrors the Cajeta gpu bench exactly: A = identity, B[idx] =
# idx % 100, so C == B is an exact cross-check at each swept size. Emits one CSV
# row per size (schema columns.txt + trailing `backend`). Throughput in GFLOP/s
# (2*N^3 / median_ns). Skips cleanly (a skipped row, exit 0) when torch has no
# usable GPU. Run via the pinned ml/venv-rocm-gfx1151 (see PROFILE_GPU_PY).
import os, sys, time

SIZES = [256, 512, 1024, 2048]


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def stats(samples):
    s = sorted(samples)
    n = len(s)
    return s[0], s[n // 2], sum(s) // n, s[min(n - 1, n * 95 // 100)]


def row(run_id, ts, n, ver, st, ok, backend):
    sz = n * n
    mn, med, mean, p95 = st
    flops = 2.0 * (n ** 3)
    gflops = (flops / med) if med > 0 else 0.0  # FLOP per ns == GFLOP/s
    status = "ok" if ok else "invalid"
    return (
        f"1,{run_id},{ts},matmul,gpu,,-1,,{sz},python,{ver},torch,{ver},--rocm,"
        f"10,30,{mn},{med},{mean},{p95},{gflops:.3f},GFLOP/s,-1,-1,-1,-1,-1,"
        f"{status},{str(ok).lower()},,n{n},{backend}"
    )


def skip_row(run_id, ts, n, reason, ver=""):
    sz = n * n
    return (
        f"1,{run_id},{ts},matmul,gpu,,-1,,{sz},python,{ver},torch,{ver},,0,0,"
        f"-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,{reason},n{n},hip"
    )


def main():
    run_id = env("PROFILE_RUN_ID", "gpu-run")
    ts = env("PROFILE_TS", "0")
    try:
        import torch
    except Exception as e:
        for n in SIZES:
            print(skip_row(run_id, ts, n, "torch import failed"))
        return 0
    ver = getattr(torch, "__version__", "")
    if not torch.cuda.is_available():
        for n in SIZES:
            print(skip_row(run_id, ts, n, "no GPU (torch.cuda.is_available=False)", ver))
        return 0
    dev = "cuda"
    for n in SIZES:
        try:
            a = torch.eye(n, dtype=torch.float64, device=dev)
            b = (torch.arange(n * n, dtype=torch.float64, device=dev) % 100).reshape(n, n)
            torch.cuda.synchronize()
            for _ in range(10):  # warmup
                c = a @ b
            torch.cuda.synchronize()
            samples = []
            for _ in range(30):
                t0 = time.perf_counter_ns()
                c = a @ b
                torch.cuda.synchronize()
                samples.append(time.perf_counter_ns() - t0)
            ok = bool(torch.allclose(c, b))   # A=identity => C==B
            print(row(run_id, ts, n, ver, stats(samples), ok, "hip"))
        except Exception as e:
            print(skip_row(run_id, ts, n, "torch matmul failed: " + repr(e)[:60], ver))
    return 0


if __name__ == "__main__":
    sys.exit(main())
