#!/usr/bin/env bash
# Untimed preparation for the toolchain/ benchmarks (bench/src/main/cajeta/bench/
# toolchain/*Bench.cajeta call this from prepare(); bench/toolchain/*.md are the
# pages). Everything it writes is under the work directory, never in the repo's
# own trees: the tour is copied out of samples/tour without its build/ and
# .cajeta/ (so a cold row starts from an empty compile cache and the repo's
# cache is never touched).
#
#   prep.sh tour-copies N     N fresh tour copies: <work>/tour-cold/0..N-1
#   prep.sh tour-warm         one tour copy, built once: <work>/tour-warm
#   prep.sh synth-copies N    N fresh copies of the compile-10k program
#                             (datasets/gen_synth.py): <work>/synth-cold/0..N-1,
#                             its expected output in <work>/synth-expected
#   prep.sh synth-src         the compile-10k sources for direct compiles:
#                             <work>/lto/src (and <work>/synth-expected)
#   prep.sh hello             hello-world: <work>/hello/src, built --release to
#                             <work>/hello/build/hello and stripped to hello.stripped
#   prep.sh hello-project     hello-world as a cajeta.json project, built once:
#                             <work>/hello-project (the build-noop row)
#   prep.sh jit-warm          one jit-run of hello into <work>/jit-warm (the cache)
#   prep.sh tour-size         the tour built (debug flavor: optimized builds of
#                             the tour do not link on v0.46.3), stripped to
#                             <work>/tour-size/tour.stripped, and --emit=cja to
#                             <work>/tour-size/tour.cja
#   prep.sh dap               a tour copy at <work>/dap/tour, the breakpoint line
#                             in <work>/dap/line, and one DAP session to fill
#                             the JIT cache <work>/dap/cache
#   prep.sh kernel-probe      prints `kernel: nozmq` when this compiler was built
#                             without libzmq (no `cajeta kernel`), else `kernel: serves`
#   prep.sh clean <subdir>    remove <work>/<subdir>
#
# Env: CAJETA (compiler; default <repo>/build/src/cajeta), CAJETA_BENCH_REPO
# (default: the directory above bench/), CAJETA_BENCH_TOOLCHAIN_WORK (default
# bench/build/toolchain), CAJETA_BENCH_PYTHON (default /usr/bin/python3). Prints `ok` last; any failure exits non-zero with the
# reason on stderr, which the benchmark turns into its row's note.
set -euo pipefail
HERE="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BENCH="$( cd -- "$HERE/../.." &> /dev/null && pwd )"
REPO="${CAJETA_BENCH_REPO:-$( cd -- "$BENCH/.." &> /dev/null && pwd )}"
CJ="${CAJETA:-$REPO/build/src/cajeta}"
WK="${CAJETA_BENCH_TOOLCHAIN_WORK:-$BENCH/build/toolchain}"
TOUR="$REPO/samples/tour"
PYTHON="${CAJETA_BENCH_PYTHON:-/usr/bin/python3}"
mkdir -p "$WK"
die() { echo "prep.sh: $*" >&2; exit 1; }
[[ -x "$CJ" ]] || die "no compiler at $CJ (set CAJETA)"

tour_copy() {   # dst
    rm -rf "$1"; mkdir -p "$1"
    rsync -a --exclude build --exclude .cajeta "$TOUR/" "$1/"
}
synth_root() { "$PYTHON" "$BENCH/datasets/gen_synth.py" "$BENCH/datasets/cache"; }

hello_src() {   # dir: writes <dir>/hello/Hello.cajeta
    mkdir -p "$1/hello"
    cat > "$1/hello/Hello.cajeta" <<'EOF'
package hello;

// toolchain/startup-* and binary-size-hello: hello-world (bench/toolchain/Startup.md).
public final class Hello {
    public static int32 main() {
        System.stdout.println("hello, world");
        return 0;
    }
}
EOF
}

case "${1:-}" in
    tour-copies)
        n="${2:?count}"; rm -rf "$WK/tour-cold"
        for ((i = 0; i < n; i++)); do tour_copy "$WK/tour-cold/$i"; done ;;
    tour-warm)
        tour_copy "$WK/tour-warm"
        ( cd "$WK/tour-warm" && "$CJ" build >"$WK/tour-warm.log" 2>&1 ) || die "tour build failed (see $WK/tour-warm.log)" ;;
    synth-copies)
        n="${2:?count}"; G="$(synth_root)"; rm -rf "$WK/synth-cold"
        for ((i = 0; i < n; i++)); do mkdir -p "$WK/synth-cold"; cp -r "$G/cajeta" "$WK/synth-cold/$i"; done
        cp "$G/EXPECTED" "$WK/synth-expected"; cp "$G/LINES" "$WK/synth-lines" ;;
    synth-src)
        G="$(synth_root)"; rm -rf "$WK/lto"; mkdir -p "$WK/lto"
        cp -r "$G/cajeta/src/main/cajeta" "$WK/lto/src"
        cp "$G/EXPECTED" "$WK/synth-expected"; cp "$G/LINES" "$WK/synth-lines" ;;
    hello)
        # Reused across the rows that need it while the compiler is the same one.
        stamp="$("$CJ" --version 2>&1 | head -1)"
        if [[ -x "$WK/hello/build/hello.stripped" && "$(cat "$WK/hello/.stamp" 2>/dev/null)" == "$stamp" ]]; then
            echo ok; exit 0
        fi
        rm -rf "$WK/hello"; hello_src "$WK/hello/src"
        "$CJ" --release --emit=exe -o "$WK/hello/build/hello" hello.Hello.main "$WK/hello/src" "$WK/hello/build/obj" \
            >"$WK/hello.log" 2>&1 || die "hello build failed (see $WK/hello.log)"
        strip -o "$WK/hello/build/hello.stripped" "$WK/hello/build/hello"
        printf '%s\n' "$stamp" > "$WK/hello/.stamp" ;;
    hello-project)
        P="$WK/hello-project"; rm -rf "$P"; hello_src "$P/src/main/cajeta"
        cat > "$P/cajeta.json" <<'EOF'
{
    "details": { "name": "hellonoop", "version": "0.1.0", "description": "toolchain/build-noop",
                 "license": "Apache-2.0", "authors": ["cajeta-bench"], "cajeta-lang-version": "1.0" },
    "settings": { "capabilities": [], "dependencies": {},
                  "build": { "source-root": "src/main/cajeta", "target": "host", "entry-method": "hello.Hello::main" } },
    "tasks": { "build": { "description": "Build hello-world",
        "actions": [ { "action": "build", "flavor": "debug", "output-path": "build/hello", "id": "art" } ],
        "outputs": { "path": "${art.path}", "sha256": "${art.sha256}" } } }
}
EOF
        ( cd "$P" && "$CJ" build >"$WK/hello-project.log" 2>&1 ) || die "hello project build failed (see $WK/hello-project.log)" ;;
    jit-warm)
        [[ -d "$WK/hello/src" ]] || hello_src "$WK/hello/src"
        rm -rf "$WK/jit-warm"
        "$CJ" jit-run --cache-dir="$WK/jit-warm" "$WK/hello/src" hello.Hello.main >"$WK/jit-warm.log" 2>&1 \
            || die "jit-run failed (see $WK/jit-warm.log)" ;;
    tour-size)
        D="$WK/tour-size"; rm -rf "$D"; mkdir -p "$D"; tour_copy "$D/tour"
        ( cd "$D/tour" && "$CJ" build >"$D/build.log" 2>&1 ) || die "tour build failed (see $D/build.log)"
        strip -o "$D/tour.stripped" "$D/tour/build/tour"
        "$CJ" --emit=cja -o "$D/tour.cja" tour.Tour.main "$D/tour/src/main/cajeta" "$D/obj" >"$D/cja.log" 2>&1 \
            || die "tour --emit=cja failed (see $D/cja.log)" ;;
    dap)
        D="$WK/dap"; rm -rf "$D"; mkdir -p "$D"; tour_copy "$D/tour"
        # The breakpoint: the first `demos.add(heap ...)` statement of Tour.main (not
        # the header comment that quotes one).
        grep -nE '^[[:space:]]+demos\.add\(heap ' "$D/tour/src/main/cajeta/tour/Tour.cajeta" | head -1 | cut -d: -f1 > "$D/line"
        [[ -s "$D/line" ]] || die "no demos.add line in Tour.cajeta"
        "$PYTHON" "$HERE/dap_breakpoint.py" "$CJ" "$D/tour/src/main/cajeta" tour.Tour.main Tour.cajeta "$(cat "$D/line")" "$D/cache" 1800 \
            >"$D/warm.log" 2>&1 || die "the DAP warm-up session failed (see $D/warm.log)" ;;
    kernel-probe)
        # Can this compiler serve a kernel? A build without libzmq reads the
        # connection file and then says so; one with libzmq starts serving
        # (cut off after 5 s). Prints `kernel: nozmq` or `kernel: serves`;
        # anything else is an error.
        f="$WK/kernel-probe.json"
        printf '{"transport":"tcp","ip":"127.0.0.1","shell_port":59701,"iopub_port":59702,"stdin_port":59703,"control_port":59704,"hb_port":59705,"key":"probe","signature_scheme":"hmac-sha256"}\n' > "$f"
        rc=0; msg="$(timeout 5 "$CJ" kernel -f "$f" 2>&1)" || rc=$?
        if [[ "$msg" == *"without libzmq"* ]]; then echo "kernel: nozmq"
        elif (( rc == 124 )); then echo "kernel: serves"
        else die "cajeta kernel exited $rc: $(head -1 <<<"$msg")"; fi ;;
    clean)
        rm -rf "$WK/${2:?subdir}" ;;
    *)
        sed -n '2,32p' "$0"; exit 2 ;;
esac
echo ok
