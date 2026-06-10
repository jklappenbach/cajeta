<#
  cajeta_tests.ps1 -- dynamic work-queue parallel runner for cajeta_test.exe.

  Replaces the static round-robin sharding in cajeta_tests.cmd's :parallel path.
  Instead of slicing all tests into N fixed buckets up front (which leaves lanes
  idle once their bucket drains while other lanes still grind through slow JIT
  tests), this keeps every test in one queue and hands the next CHUNK to whichever
  worker just freed up. Lanes stay busy until the queue is empty.

  Chunk size is GUIDED: large early (amortizes per-process startup), shrinking to
  1 at the tail for a tight finish. Override with -Chunk N for a fixed size.

  Failed/crashed tests are re-run INDIVIDUALLY and SERIALLY afterwards: 32-way
  LLJIT memory pressure makes a test occasionally crash/misvalue, and the serial
  retry recovers that flakiness while genuine failures stay red. The final exit
  code is based on the retry.

  Reuse-ready: set $env:CAJETA_STDLIB_REUSE=1 before running. A worker then
  amortizes the stdlib prime across its whole chunk (first test ~17s, rest ~4s) --
  at which point a big -Chunk is the win. (Today reuse still has the ZoneId/Clock
  layer, so leave it off for whole-suite runs.)

  Usage:
    .\cajeta_tests.ps1                          # whole suite, CPU-count workers
    .\cajeta_tests.ps1 -Filter BinaryOpTests    # one suite
    .\cajeta_tests.ps1 -Filter Fp*,Json*        # wildcard / multiple
    .\cajeta_tests.ps1 -Workers 16 -Chunk 8
    .\cajeta_tests.ps1 -NoTui                    # plain periodic snapshots
    .\cajeta_tests.ps1 -GtestFlags --gtest_repeat=3
#>
[CmdletBinding()]
param(
    [string]   $TestBin    = 'build\test\cajeta_test.exe',
    [string[]] $Filter     = @(),
    [int]      $Workers    = 0,    # 0 => CPU count (capped at 32)
    [int]      $Chunk      = 0,    # 0 => guided self-scheduling
    [switch]   $NoRetry,
    [switch]   $NoTui,
    [string[]] $GtestFlags = @()
)

$Root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
Set-Location $Root
if (-not [System.IO.Path]::IsPathRooted($TestBin)) { $TestBin = Join-Path $Root $TestBin }

# --- runtime env: mingw + cajeta-llvm dll dir on PATH, fixtures root ----------
# Mirrors cajeta_tests.cmd so the script also works when launched directly
# (Windows resolves DLLs via PATH / the exe's dir, so the fork's libLLVM-*.dll
# directory must be reachable or the binary fails to even launch, 0xC0000135).
$msys = $env:MSYS2_ROOT; if (-not $msys) { $msys = 'C:\msys64' }
if (Test-Path "$msys\mingw64\bin") { $env:PATH = "$msys\mingw64\bin;$env:PATH" }
$llvmBin = $null
if ($env:CAJETA_LLVM_ROOT -and (Test-Path (Join-Path $env:CAJETA_LLVM_ROOT 'bin\libLLVM*.dll'))) {
    $llvmBin = Join-Path $env:CAJETA_LLVM_ROOT 'bin'
}
if (-not $llvmBin -and (Test-Path 'build\CMakeCache.txt')) {
    $row = Select-String -Path 'build\CMakeCache.txt' -Pattern '^LLVM_DIR(:[^=]*)?=' | Select-Object -First 1
    if ($row) {
        $d = ($row.Line -split '=', 2)[1] -replace '/', '\'
        $cand = ($d -replace '\\lib\\cmake\\llvm$', '') + '\bin'
        if (Test-Path (Join-Path $cand 'libLLVM*.dll')) { $llvmBin = $cand }
    }
}
if ($llvmBin) { $env:PATH = "$llvmBin;$env:PATH" }
if (-not $env:CAJETA_SOURCE_ROOT) { $env:CAJETA_SOURCE_ROOT = $Root }

if (-not (Test-Path $TestBin)) { Write-Error "test binary not found: $TestBin"; exit 1 }

if ($Workers -le 0) {
    $Workers = [int]$env:NUMBER_OF_PROCESSORS
    if ($Workers -le 0) { $Workers = 4 }
}
if ($Workers -gt 32) { $Workers = 32 }   # LLJIT memory pressure beyond ~32-way

# Live dashboard needs the per-test [ RUN ]/[ OK ] lines that --gtest_brief=1
# suppresses; brief mode is used for the non-TUI (redirected / -NoTui) path.
$liveTui = (-not $NoTui) -and (-not [Console]::IsOutputRedirected)
$hasBrief = ($GtestFlags | Where-Object { $_ -like '--gtest_brief*' }).Count -gt 0
$baseArgs = @() + $GtestFlags
if (-not $hasBrief) { if ($liveTui) { $baseArgs += '--gtest_brief=0' } else { $baseArgs += '--gtest_brief=1' } }

# --- discovery ---------------------------------------------------------------
function Build-Filter([string[]] $pats) {
    if (-not $pats -or $pats.Count -eq 0) { return $null }
    $parts = foreach ($p in $pats) { if ($p -notmatch '[.*]') { "$p.*" } else { $p } }
    return ($parts -join ':')
}
$discFilter = Build-Filter $Filter
$listArgs = @('--gtest_list_tests')
if ($discFilter) { $listArgs += "--gtest_filter=$discFilter" }
Write-Host '>> Discovering tests...'
$listRaw = & $TestBin @listArgs 2>$null
# gtest format: a suite header at column 0 ends with '.', tests are indented
# (first token), and a standalone '#'-comment line is type/value-param metadata.
$tests = New-Object System.Collections.Generic.List[string]
$cur = $null
foreach ($ln in $listRaw) {
    $tok = ($ln -split '\s+') | Where-Object { $_ -ne '' } | Select-Object -First 1
    if (-not $tok) { continue }
    if ($tok.StartsWith('#')) { continue }
    if ($tok.EndsWith('.')) { $cur = $tok }
    elseif ($cur) { $tests.Add($cur + $tok) }
}
$total = $tests.Count
if ($total -eq 0) { Write-Error 'no tests discovered via --gtest_list_tests'; exit 1 }

$tmp = Join-Path $env:TEMP ('cajeta_ps_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null

# --- scheduler state ---------------------------------------------------------
$queue = New-Object System.Collections.Generic.Queue[string]
foreach ($t in $tests) { $queue.Enqueue($t) }
$running    = New-Object System.Collections.ArrayList
$idx        = 0
$completed  = 0
$passTotal  = 0
$failNames  = New-Object System.Collections.Generic.List[string]
$crashTests = New-Object System.Collections.Generic.List[string]
$sw = [Diagnostics.Stopwatch]::StartNew()

function Get-ChunkSize {
    if ($Chunk -gt 0) { return $Chunk }
    # Guided: ~ remaining / (2 * workers). Big early for startup amortization,
    # 1 at the tail so the last lane can't run long alone.
    $n = [Math]::Ceiling($queue.Count / ($Workers * 2.0))
    if ($n -lt 1)  { $n = 1 }
    if ($n -gt 24) { $n = 24 }
    return [int]$n
}

function Start-One {
    $size = Get-ChunkSize
    $names = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $size -and $queue.Count -gt 0; $i++) { $names.Add($queue.Dequeue()) }
    if ($names.Count -eq 0) { return }
    $script:idx++
    $id  = $script:idx
    $out = Join-Path $tmp "c$id.out"
    $err = Join-Path $tmp "c$id.err"
    $argList = @("--gtest_filter=$(($names -join ':'))") + $baseArgs
    $p = Start-Process -FilePath $TestBin -ArgumentList $argList `
            -RedirectStandardOutput $out -RedirectStandardError $err `
            -NoNewWindow -PassThru
    # PS 5.1: a Start-Process -PassThru object reports a NULL .ExitCode after the
    # child exits unless its OS handle was retained. Touch .Handle now to force it.
    $null = $p.Handle
    [void]$running.Add([pscustomobject]@{
        Id = $id; Proc = $p; Names = $names; Out = $out; Err = $err
        Size = $names.Count; Started = $sw.Elapsed.TotalSeconds
    })
}

function Parse-Chunk($e) {
    $txt = ''
    try { if (Test-Path $e.Out) { $txt = [IO.File]::ReadAllText($e.Out) } } catch { $txt = '' }
    $passed = 0
    $pm = [regex]::Match($txt, '\[\s+PASSED\s+\]\s+(\d+)\s+test')
    if ($pm.Success) { $passed = [int]$pm.Groups[1].Value }
    $fails = @()
    foreach ($m in [regex]::Matches($txt, '(?m)^\[\s+FAILED\s+\]\s+(\S+)\s+\(\d+\s*ms\)')) {
        $fails += $m.Groups[1].Value
    }
    $code = $null
    try { $code = $e.Proc.ExitCode } catch { $code = $null }
    # Crash = the process died mid-chunk. A gtest run that REACHES THE END always
    # prints a "[  PASSED  ] N tests." summary (even when some tests fail), so its
    # absence is the reliable crash signal. Do NOT key crash off the exit code:
    # Start-Process -PassThru yields a null .ExitCode (null -ne 0 is $true in PS),
    # which would mark every passing chunk crashed and dump the whole suite into
    # the serial retry pass -- the bug that made this runner "just as slow".
    $hasSummary = [regex]::IsMatch($txt, '\[\s+PASSED\s+\]\s+\d+\s+test')
    $crashed = (-not $hasSummary)
    return [pscustomobject]@{ Passed = $passed; Fails = $fails; Crashed = $crashed; Code = $code }
}

function Get-LaneInfo($e) {
    $cur = '(starting)'; $done = 0
    try {
        if (Test-Path $e.Out) {
            $lines = [IO.File]::ReadAllLines($e.Out)
            for ($i = $lines.Length - 1; $i -ge 0; $i--) {
                if ($lines[$i] -match '^\[\s*RUN') {
                    $cur = ($lines[$i] -replace '^\[\s*RUN\s*\]\s*', '').Trim(); break
                }
            }
            foreach ($l in $lines) {
                if ($l -match '^\[\s+OK\s+\]' -or $l -match '^\[\s+FAILED\s+\]\s+\S+\s+\(') { $done++ }
            }
        }
    } catch { }
    if ($cur.Length -gt 40) { $cur = $cur.Substring(0, 40) }
    return [pscustomobject]@{ Cur = $cur; Done = $done }
}

# --- live dashboard (anchored block, repainted in place) ---------------------
$anchorTop = $null
$reserve = $Workers + 1
if ($liveTui) {
    try {
        for ($i = 0; $i -lt $reserve; $i++) { Write-Host '' }
        $anchorTop = [Console]::CursorTop - $reserve
        if ($anchorTop -lt 0) { $anchorTop = 0 }
        [Console]::CursorVisible = $false
    } catch { $liveTui = $false }
}
$lastSnap = -100.0

function Render {
    if ($liveTui) {
        try {
            $w = [Console]::WindowWidth - 1
            if ($w -lt 20) { $w = 20 }
            [Console]::SetCursorPosition(0, $anchorTop)
            $rel = [int]$sw.Elapsed.TotalSeconds
            $fill = if ($total -gt 0) { [int]($completed * 30 / $total) } else { 0 }
            if ($fill -gt 30) { $fill = 30 }
            $bar = ('#' * $fill) + ('-' * (30 - $fill))
            $nbad = $failNames.Count + $crashTests.Count
            $hdr = "cajeta [$bar] $completed/$total  ${Workers}w  q=$($queue.Count)  ${nbad}f  ${rel}s"
            Write-Host ($hdr.PadRight($w).Substring(0, $w))
            $shown = 0
            foreach ($e in $running) {
                $info = Get-LaneInfo $e
                $age = [int]($sw.Elapsed.TotalSeconds - $e.Started)
                $line = "  #$($e.Id) $($info.Cur) ($($info.Done)/$($e.Size)) ${age}s"
                if ($line.Length -gt $w) { $line = $line.Substring(0, $w) }
                Write-Host $line.PadRight($w)
                $shown++; if ($shown -ge $Workers) { break }
            }
            for ($k = $shown; $k -lt $Workers; $k++) { Write-Host (' ' * $w) }
        } catch { $script:liveTui = $false }
    }
    else {
        # Plain mode: a progress snapshot at most every 2s.
        $now = $sw.Elapsed.TotalSeconds
        if (($now - $script:lastSnap) -ge 2.0) {
            $script:lastSnap = $now
            $nbad = $failNames.Count + $crashTests.Count
            Write-Host ("  {0}/{1} done  {2} running  q={3}  {4}f  {5}s" -f `
                $completed, $total, $running.Count, $queue.Count, $nbad, [int]$now)
        }
    }
}

Write-Host ">> Running $total tests across $Workers workers (guided chunks)..."
$t0 = $sw.Elapsed.TotalSeconds

try {
    while ($queue.Count -gt 0 -or $running.Count -gt 0) {
        while ($running.Count -lt $Workers -and $queue.Count -gt 0) { Start-One }
        $fin = @($running | Where-Object { $_.Proc.HasExited })
        foreach ($e in $fin) {
            try { $e.Proc.WaitForExit() } catch { }
            $r = Parse-Chunk $e
            $passTotal += $r.Passed
            foreach ($f in $r.Fails) { $failNames.Add($f) }
            if ($r.Crashed) { foreach ($n in $e.Names) { $crashTests.Add($n) } }
            $completed += $e.Size
            $running.Remove($e)
        }
        Render
        if ($fin.Count -eq 0) { Start-Sleep -Milliseconds 200 }
    }
}
finally {
    # Always release surviving children + cursor on Ctrl-C / error.
    foreach ($e in @($running)) { try { if (-not $e.Proc.HasExited) { $e.Proc.Kill() } } catch { } }
    if ($null -ne $anchorTop) {
        try { [Console]::SetCursorPosition(0, $anchorTop + $reserve); [Console]::CursorVisible = $true } catch { }
    }
}
$elapsed = [int]($sw.Elapsed.TotalSeconds - $t0)

# --- first-pass summary ------------------------------------------------------
$firstFails  = $failNames  | Select-Object -Unique
$firstCrash  = $crashTests | Select-Object -Unique
Write-Host ''
Write-Host '=== First pass ==='
Write-Host ("Discovered: {0}   Workers: {1}   Elapsed: {2}s" -f $total, $Workers, $elapsed)
Write-Host ("Passed: {0}   Failed: {1}   Crashed: {2}" -f $passTotal, $firstFails.Count, $firstCrash.Count)

$retry = New-Object System.Collections.Generic.List[string]
foreach ($n in $firstFails) { if (-not $retry.Contains($n)) { $retry.Add($n) } }
foreach ($n in $firstCrash) { if (-not $retry.Contains($n)) { $retry.Add($n) } }

if ($retry.Count -eq 0) {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host ''
    Write-Host "ALL $passTotal TESTS PASSED."
    exit 0
}

if ($NoRetry) {
    Write-Host ''
    Write-Host 'Failures (first pass, no retry):'
    foreach ($n in $firstFails) { Write-Host "  FAIL  $n" }
    foreach ($n in $firstCrash) { Write-Host "  CRASH $n" }
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

# --- retry pass: each suspect test alone, serially ---------------------------
Write-Host ''
Write-Host "=== Retry pass: re-running $($retry.Count) suspect test(s) individually, serially ==="
Write-Host '(32-way memory-pressure flakiness recovers here; real failures persist)'
$realFail  = New-Object System.Collections.Generic.List[string]
$realCrash = New-Object System.Collections.Generic.List[string]
foreach ($n in $retry) {
    $o = Join-Path $tmp 'retry.out'; $er = Join-Path $tmp 'retry.err'
    $p = Start-Process -FilePath $TestBin -ArgumentList @("--gtest_filter=$n", '--gtest_brief=1') `
            -RedirectStandardOutput $o -RedirectStandardError $er -NoNewWindow -PassThru
    $null = $p.Handle    # retain handle so .ExitCode populates after exit (PS 5.1)
    $p.WaitForExit()
    $txt = ''
    try { $txt = [IO.File]::ReadAllText($o) } catch { }
    $isFail = [regex]::IsMatch($txt, '(?m)^\[\s+FAILED\s+\]\s+\S+\s+\(\d+\s*ms\)')
    # Same output-based crash signal as Parse-Chunk: a completed run prints the
    # "[  PASSED  ]" summary; its absence (and no FAILED line) means a real crash.
    $hasSummary = [regex]::IsMatch($txt, '\[\s+PASSED\s+\]\s+\d+\s+test')
    $code = $null; try { $code = $p.ExitCode } catch { $code = $null }
    if ($isFail) { $realFail.Add($n); Write-Host "  FAIL  $n" }
    elseif (-not $hasSummary) { $realCrash.Add($n); Write-Host "  CRASH $n (exit $code)" }
    else { Write-Host "  ok    $n (was flaky)" }
}

Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ''
Write-Host '=== Retry summary ==='
Write-Host ("Still failing: {0}   Still crashing: {1}" -f $realFail.Count, $realCrash.Count)
if ($realFail.Count -eq 0 -and $realCrash.Count -eq 0) {
    Write-Host 'All first-pass problems recovered on serial retry -- memory-pressure flakiness, not real. PASS.'
    exit 0
}
foreach ($n in $realFail)  { Write-Host "  REAL FAIL  $n" }
foreach ($n in $realCrash) { Write-Host "  REAL CRASH $n" }
exit 1
