# Engine regression on the PC: unit tests, pool fuzz, mechanisms, registry
# stress, swarm/scheduler (all modes), peer rotation, loopback perf with data
# verification -- 21 checks. See docs/engine-optimization-plan.md for what each
# test covers and the reference numbers.
#
#   powershell -ExecutionPolicy Bypass -File pctest\run_regress.ps1 [-Build] [-Quick]
#
#   -Build   run pctest\build.ps1 first (needs devkitPro's MSYS2 MinGW64 with
#            mbedtls + curl). Note: it rewrites the test .exe files tracked in
#            git; restore them with  git checkout -- pctest/*.exe
#   -Quick   skip the multi-GB loopback perf runs (~1 min instead of ~5)
#
# Exit code: number of failed checks (0 = all passed). Every check must exit 0
# and, where it compares data, report 0 mismatches / PASSED.
param([switch]$Build, [switch]$Quick)

$env:PATH = "C:\devkitPro\msys2\mingw64\bin;$env:PATH"
Push-Location (Join-Path $PSScriptRoot "..")   # tests expect the repo root as cwd
try {
    if ($Build) {
        powershell -ExecutionPolicy Bypass -File pctest\build.ps1 2>&1 |
            Select-String "OK -> |error:" | ForEach-Object { $_.Line }
        if ($LASTEXITCODE -ne 0) { Write-Host "build failed" -ForegroundColor Red; exit 100 }
    }
    $need = @("test_bencode", "test_magnet", "test_torrent_meta", "test_pool", "test_mechanisms",
              "test_registry", "test_swarm_sched", "test_rotation", "test_engine_perf")
    $missing = $need | Where-Object { -not (Test-Path ".\pctest\$_.exe") }
    if ($missing) {
        Write-Host ("missing test binaries: " + ($missing -join ", ") + " -- run with -Build") -ForegroundColor Red
        exit 101
    }

    $fail = 0
    foreach ($t in @("test_bencode", "test_magnet", "test_torrent_meta", "test_pool")) {
        $out = & ".\pctest\$t.exe" 2>&1 | Select-Object -Last 1
        if ($LASTEXITCODE -ne 0) { $fail++ }
        "{0,-28} code={1} {2}" -f $t, $LASTEXITCODE, $out
    }

    function RunT($exe, [string]$a, $label, $timeout, $pattern) {
        if ($a) {
            $p = Start-Process -FilePath $exe -ArgumentList $a -NoNewWindow -PassThru `
                 -RedirectStandardOutput "$env:TEMP\rg_out.txt" -RedirectStandardError "$env:TEMP\rg_err.txt"
        } else {
            $p = Start-Process -FilePath $exe -NoNewWindow -PassThru `
                 -RedirectStandardOutput "$env:TEMP\rg_out.txt" -RedirectStandardError "$env:TEMP\rg_err.txt"
        }
        $null = $p.Handle   # keeps ExitCode readable after the process ends
        $ok = $p.WaitForExit($timeout)
        if (-not $ok) { $p.Kill() }
        if (-not $ok -or $p.ExitCode -ne 0) { $script:fail++ }
        $r = (Select-String -Path "$env:TEMP\rg_out.txt" -Pattern $pattern | ForEach-Object { $_.Line.Trim() }) -join " | "
        "{0,-28} exited={1} code=0x{2:X} {3}" -f $label, $ok, $p.ExitCode, $r
    }

    RunT ".\pctest\test_mechanisms.exe" "" "mechanisms" 240000 "ALL OPTIMIZATION|FAILED"
    foreach ($i in 1..2) { RunT ".\pctest\test_registry.exe" "--secs 15" "registry#$i" 120000 "registry stress" }
    foreach ($m in @("--piece 2", "--piece 2 --appsched", "--piece 1 --appsched", "--piece 8 --appsched",
                     "--piece 2 --nozones", "--piece 2 --appsched --loose", "--piece 2 --appsched --sameip")) {
        RunT ".\pctest\test_swarm_sched.exe" $m "swarm $m" 300000 "swarm/scheduler|duplicate payload|^FAILED"
    }
    RunT ".\pctest\test_rotation.exe" "--norotate" "rotation off" 300000 "rotation test|fast seeders"
    RunT ".\pctest\test_rotation.exe" "" "rotation on" 300000 "rotation test|fast seeders"
    if (-not $Quick) {
        foreach ($i in 1..2) { RunT ".\pctest\test_engine_perf.exe" "--size 4096 --piece-size 4" "perf 4G#$i" 150000 "Effective|Data Verification" }
        RunT ".\pctest\test_engine_perf.exe" "--size 2048 --piece-size 16" "perf 2G 16MB" 150000 "Effective|Data Verification"
        RunT ".\pctest\test_engine_perf.exe" "--size 1024 --piece-size 1" "perf 1G 1MB" 150000 "Effective|Data Verification"
    }

    # Torrents and caches the tests leave behind (untracked).
    Remove-Item -ErrorAction SilentlyContinue pctest\swarm_sched_test.torrent, pctest\registry_test.torrent, pctest\rotation_test.torrent

    if ($fail -eq 0) { Write-Host "FAILURES: 0" -ForegroundColor Green }
    else { Write-Host "FAILURES: $fail" -ForegroundColor Red }
    exit $fail
} finally {
    Pop-Location
}
