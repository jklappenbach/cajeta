#!/usr/bin/env bash
# Competitor rows -> schema v2 with catalog ids (benchmark-reference; the
# columns are competitors/columns.txt, the same 37 dev.cajeta.bench writes).
#
# Every package driver (competitors/<package>.sh) pipes its programs' stdout
# through `rows_v2 <package>`. A program may print either shape:
#   * v1: 31 or 32 columns (…,variant[,backend]) with the profile-era area and
#     benchmark names (sort, string, stream, clbg, math, gpu, …);
#   * v2: 37 columns, schema_version 2, area = package, benchmark = slug.
# The filter writes v2 only:
#   - area/benchmark mapped to the catalog id <package>/<slug>:
#       sort -> collection, string|stream|clbg -> lang,
#       math/saxpy|math/dot-product -> lang (the SIMD rows of Math.md),
#       math/matmul stays math, gpu/matmul -> xpu/gemm-f32,
#       gpu/matmul-f16 -> xpu/gemm-f16; time, concurrent, hash, codec as is;
#       scaffold/hello (the toolchain probe) -> selftest/hello;
#   - backend defaults to cpu;
#   - machine = CAJETA_BENCH_MACHINE, else the hostname lowercased;
#   - os = "NAME VERSION_ID" from os-release, " (WSL2)" under Microsoft's
#     kernel (the dev.cajeta.bench format);
#   - driver = empty on cpu rows; read from /proc/driver/nvidia/version or
#     /opt/rocm/.info/version for cuda/hip rows (files only: nothing touches
#     the device);
#   - model = empty; platform = the language column;
#   - go rows: a literal GOAMD64=vN in flags becomes the level the driver
#     built with ($GOAMD64), so the row records what actually ran.
# With a package argument, rows whose catalog package differs are dropped
# (a program that spans two packages, e.g. math/math's saxpy + matmul, is run
# by both drivers and each keeps its own rows). A line that is neither v1 nor
# v2 is reported on stderr and dropped, so a malformed program fails its smoke.

# Go: AVX-512 (GOAMD64=v4) only where the CPU has it (Proton's Zen 5 does,
# Phoenix's Zen 2 does not, and a v4 binary refuses to start there). Sourcing
# this file exports the level; the drivers build with it and rows_v2 records
# it in the go rows' flags.
if [[ -z "${GOAMD64:-}" ]]; then
    if grep -qw avx512f /proc/cpuinfo 2>/dev/null; then GOAMD64=v4; else GOAMD64=v3; fi
fi
export GOAMD64

_rows_os() {
    local name="" ver="" pretty="" base="Linux" k
    if [[ -r /etc/os-release ]]; then
        name="$(. /etc/os-release 2>/dev/null; printf '%s' "${NAME:-}")"
        ver="$(. /etc/os-release 2>/dev/null; printf '%s' "${VERSION_ID:-}")"
        pretty="$(. /etc/os-release 2>/dev/null; printf '%s' "${PRETTY_NAME:-}")"
    fi
    if [[ -n "$name" ]]; then base="$name${ver:+ $ver}"; elif [[ -n "$pretty" ]]; then base="$pretty"; fi
    k="$(tr 'A-Z' 'a-z' < /proc/version 2>/dev/null || true)"
    if [[ "$k" == *wsl2* || "$k" == *microsoft-standard* ]]; then base="$base (WSL2)"
    elif [[ "$k" == *microsoft* ]]; then base="$base (WSL1)"; fi
    printf '%s' "${base//,/ }"
}

_rows_machine() {
    local m="${CAJETA_BENCH_MACHINE:-}"
    m="$(printf '%s' "$m" | tr -d '[:space:]')"
    if [[ -z "$m" ]]; then
        m="$(cat /proc/sys/kernel/hostname 2>/dev/null || hostname 2>/dev/null || echo unknown)"
        m="$(printf '%s' "$m" | tr -d '[:space:]' | tr 'A-Z' 'a-z')"
    fi
    printf '%s' "${m:-unknown}"
}

_rows_driver_nv() {
    sed -nE 's/.*Kernel Module( for [^ ]+)?[[:space:]]+([0-9][0-9.]*).*/NVIDIA \2/p' /proc/driver/nvidia/version 2>/dev/null | head -1
}
_rows_driver_rocm() {
    local v; v="$(head -1 /opt/rocm/.info/version 2>/dev/null || true)"
    [[ -n "$v" ]] && printf 'ROCm %s' "$v"
}

rows_v2() {
    local want="${1:-}"
    awk -F, -v OFS=, -v want="$want" \
        -v machine="$(_rows_machine)" -v os="$(_rows_os)" \
        -v drv_nv="$(_rows_driver_nv)" -v drv_rocm="$(_rows_driver_rocm)" \
        -v goamd64="${GOAMD64:-}" '
    function map(area, bench,    key) {
        key = area "/" bench
        if (area == "sort") return "collection" SUBSEP bench
        if (area == "string" || area == "stream" || area == "clbg") return "lang" SUBSEP bench
        if (key == "math/saxpy" || key == "math/dot-product") return "lang" SUBSEP bench
        if (key == "gpu/matmul") return "xpu" SUBSEP "gemm-f32"
        if (key == "gpu/matmul-f16") return "xpu" SUBSEP "gemm-f16"
        if (area == "gpu") return "xpu" SUBSEP bench
        if (area == "scaffold") return "selftest" SUBSEP bench
        return area SUBSEP bench
    }
    /^[[:space:]]*$/ { next }
    $1 == "schema_version" { next }          # a stray header line
    {
        if (NF == 37 && $1 == "2") {
            pkg = $5
        } else if ((NF == 31 || NF == 32) && $1 == "1") {
            if (NF == 31) $32 = ""
            split(map($5, $4), m, SUBSEP)
            $5 = m[1]; $4 = m[2]
            $1 = "2"
            if ($32 == "") $32 = "cpu"
            d = ""
            if ($32 == "cuda" || $32 == "nvptx") d = drv_nv
            else if ($32 == "hip" || $32 == "amdgpu" || $32 == "rocm") d = drv_rocm
            $33 = machine; $34 = os; $35 = d; $36 = ""; $37 = $10
            pkg = $5
        } else {
            print "rows_v2: dropped a line with " NF " columns: " substr($0, 1, 120) > "/dev/stderr"
            next
        }
        if (goamd64 != "" && $10 == "go") gsub(/GOAMD64=v[0-9]/, "GOAMD64=" goamd64, $14)
        if (want != "" && pkg != want) next
        print
    }'
}
