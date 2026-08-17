# gpuprobe

Standalone GPU capability probes. Not built by cmake — each is compiled by the
CI workflow that drives it, against whatever toolkit the runner has.

## `probe_cupti_caps.cu`

NVIDIA profiling capability audit, driven by `.github/workflows/gpu-profiling-audit.yml`
on the self-hosted runners PHOENIX (native Windows) and phoenix-wsl.

Settles the one load-bearing inference in the GPU kernel-timing research: whether
CUDA events and CUPTI Activity kernel timestamps are exempt from NVIDIA's
profiling permission gate. That decides whether `cajeta-profiler`'s baseline CUDA
timing tier works for unprivileged users — see `specs/cajeta-profiler-spec.md`
§5.4.4 and §12.4, and `agents/cajeta-profiler-plan.md` Unit 1.

Findings print as greppable `RESULT key=value` lines; the answer is
`RESULT verdict=`. **`EXEMPTION_PROVEN` is the only pass.**
`INCONCLUSIVE_PROCESS_PRIVILEGED` means the Profiling API was *allowed*, so the
process is privileged or the gate is off — the timing tests passing then prove
nothing about ordinary users.

Run it from the Actions tab (`workflow_dispatch`); it reads state and changes
nothing.
