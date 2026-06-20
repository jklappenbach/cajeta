#!/usr/bin/env bash
# Unit 16 TDD smoke test (Python report layer + Cajeta-themed minisite). Reads a
# fixture results.csv/env.csv and asserts report.md + a self-contained
# site/index.html are produced (no external URLs, Cajeta-branded, every area
# present). The reporter NEVER runs a benchmark.
set -euo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
PROJ="$( cd -- "${SCRIPT_DIR}/.." &> /dev/null && pwd )"

fail() { echo "REPORT FAIL: $1" >&2; exit 1; }
command -v python3 >/dev/null || fail "python3 not found"

D="/tmp/profile-report-test"
rm -rf "$D"; mkdir -p "$D"

# Minimal fixture CSV (two areas, an ok + a skipped row).
cat > "$D/results.csv" <<'CSV'
schema_version,run_id,timestamp_unix,benchmark,area,dataset_name,dataset_bytes,dataset_sha256,input_size,language,language_version,library,library_version,flags,warmup,trials,min_ns,median_ns,mean_ns,p95_ns,throughput,throughput_unit,peak_rss_kb,cgroup_peak_kb,alloc_bytes,alloc_count,working_set_kb,status,check_ok,notes,variant
1,t,1750444800,json-tokenize,codec,,-1,,631514,cajeta,0.7,stdlib,,--release,10,50,1429708,1466247,1450000,1500000,,,5000,-1,-1,-1,620,ok,true,,twitter
1,t,1750444800,json-dom,codec,,-1,,2251051,cajeta,0.7,stdlib,,--release,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,false,floats,canada
1,t,1750444800,hashmap-int,collection,,-1,,50000,cajeta,0.7,stdlib,,--release,5,30,2419869,2497504,2450000,2600000,,,34592,-1,-1,-1,34592,ok,true,,
CSV
printf 'key,value\ncpu_model,Test CPU\ncpu_cores,32\nkernel,Linux test\n' > "$D/env.csv"

python3 "$PROJ/report/report.py" "$D" || fail "report.py exited non-zero"

[[ -f "$D/report.md" ]]        || fail "report.md not written"
[[ -f "$D/site/index.html" ]]  || fail "site/index.html not written"
grep -q "Cajeta profile" "$D/site/index.html" || fail "site not Cajeta-branded"
grep -q "5C3310"          "$D/site/index.html" || fail "site missing the cajeta caramel palette"
grep -q "json-tokenize"   "$D/site/index.html" || fail "site missing a benchmark (sidebar/panel)"
grep -q "Test CPU"        "$D/site/index.html" || fail "site missing env block"
grep -q "status-skipped"  "$D/site/index.html" || fail "site missing skipped styling"
grep -q "class='sidebar'" "$D/site/index.html" || fail "site missing the benchmark sidebar index"
grep -q "showTab"         "$D/site/index.html" || fail "site missing the Time/Throughput/Memory tabs"
# self-contained: no external http(s) resource references
if grep -qE "https?://[^ '\"]+\.(css|js)" "$D/site/index.html"; then fail "site references external CDN"; fi
grep -q "## codec" "$D/report.md" || fail "report.md missing codec area"

echo "REPORT OK"
