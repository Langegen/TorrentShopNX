# log_stats.ps1 - регрессионная метрика полевых логов (engine.log + app log)
#
# Примеры:
#   powershell -File pctest\log_stats.ps1 -AppLog log1.txt -EngineLog engine1.log
#   powershell -File pctest\log_stats.ps1 -AppLog log1.txt            (только app-часть)
#
# Считает метрики, которые фигурируют в docs/improvement-plan-field-logs.md:
#   app : starvation events, wait_ms p50/p95/max, install/source speed avg/max,
#         scheduler boosts, stall mode entries, CNMT fallback, image 404
#   eng : per-tracker success/fail, starvation recovery rounds, live peers min/avg

param(
    [string]$AppLog = "",
    [string]$EngineLog = ""
)

function Pct([int64[]]$Values, [double]$P) {
    if ($Values.Count -eq 0) { return 0 }
    $sorted = $Values | Sort-Object
    $idx = [math]::Ceiling($sorted.Count * $P / 100.0) - 1
    if ($idx -lt 0) { $idx = 0 }
    return [int64]$sorted[$idx]
}

function Print-Header([string]$Title) {
    Write-Output ""
    Write-Output "=== $Title ==="
}

function Analyze-AppLog([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) {
        Write-Output "app log: not found ($Path)"
        return
    }
    $lines = Get-Content -LiteralPath $Path

    $starv_waits = [System.Collections.Generic.List[int64]]::new()
    $starv_count = 0
    $starv_max   = 0
    $install_speeds = [System.Collections.Generic.List[double]]::new()
    $source_speeds  = [System.Collections.Generic.List[double]]::new()
    $consume_speeds = [System.Collections.Generic.List[double]]::new()
    $rb_min = [int64]::MaxValue
    $boost_events = 0
    $boost_total  = 0
    $stall_entries = 0
    $stall_exits   = 0
    $cnmt_insufficient = 0
    $cnmt_fs_path  = 0
    $cnmt_inmem_fail = 0
    $img_ok  = 0
    $img_fail = 0
    $img_fail_by_code = @{}
    $prebuffer_target = 0
    $completed_starvation = -1
    $summ = $null
    $installs = 0
    $pace_sleeps = 0
    $pace_sleep_ms_total = 0

    foreach ($l in $lines) {
        if ($l -match 'installer: (?:ncz )?buffer wait wait_ms=(\d+)') {
            $ms = [int64]$matches[1]
            $starv_waits.Add($ms)
            $starv_count++
            if ($ms -gt $starv_max) { $starv_max = $ms }
        } elseif ($l -match 'installer: (?:installation completed successfully|summary) starvation_events=(\d+)') {
            $summ = [pscustomobject]@{
                starvation = [int]$matches[1]; p50 = 0; p95 = 0; max = 0; stall = 0
                inst_avg = 0; inst_max = 0; src_avg = 0; src_max = 0
                cons_avg = 0; cons_max = 0; live_min = -1
            }
            if ($l -match 'wait_p50_ms=(\d+) wait_p95_ms=(\d+) wait_max_ms=(\d+) stall_total_ms=(\d+)') {
                $summ.p50 = [int]$matches[1]; $summ.p95 = [int]$matches[2]
                $summ.max = [int]$matches[3]; $summ.stall = [int]$matches[4]
            }
            if ($l -match 'install_speed_avg_kbps=(\d+) install_speed_max_kbps=(\d+) source_speed_avg_kbps=(\d+) source_speed_max_kbps=(\d+) consume_speed_avg_kbps=(\d+) consume_speed_max_kbps=(\d+)') {
                $summ.inst_avg = [int]$matches[1]; $summ.inst_max = [int]$matches[2]
                $summ.src_avg = [int]$matches[3]; $summ.src_max = [int]$matches[4]
                $summ.cons_avg = [int]$matches[5]; $summ.cons_max = [int]$matches[6]
            }
            if ($l -match 'live_peers_min=(\d+)') { $summ.live_min = [int]$matches[1] }
            $completed_starvation = $summ.starvation
        } elseif ($l -match 'installer: pace sleep (\d+)ms') {
            $pace_sleeps++
            $pace_sleep_ms_total += [int]$matches[1]
        } elseif ($l -match 'installer: stats .*?install_speed=(\d+)KB/s .*?source_speed=(\d+)KB/s .*?consume_speed=(\d+)KB/s') {
            $install_speeds.Add([double]$matches[1])
            $source_speeds.Add([double]$matches[2])
            $consume_speeds.Add([double]$matches[3])
        } elseif ($l -match 'installer: stats .*?install_speed=(\d+)KB/s .*?source_speed=(\d+)KB/s') {
            $install_speeds.Add([double]$matches[1])
            $source_speeds.Add([double]$matches[2])
        } elseif ($l -match 'installer: stats .*?rb_avail=(\d+)') {
            $avail = [int64]$matches[1]
            if ($avail -lt $rb_min) { $rb_min = $avail }
        } elseif ($l -match 'scheduler: boosted (\d+) slow-peer claims to Critical') {
            $boost_events++
            $boost_total += [int]$matches[1]
        } elseif ($l -match 'scheduler: entered stall mode') {
            $stall_entries++
        } elseif ($l -match 'scheduler: stall recovered') {
            $stall_exits++
        } elseif ($l -match 'cnmt: insufficient data, need \d+ but have \d+') {
            $cnmt_insufficient++
        } elseif ($l -match 'hybrid: failed to extract CNMT from NCA in-memory') {
            $cnmt_inmem_fail++
        } elseif ($l -match 'hybrid: read CNMT from ContentMeta FS') {
            $cnmt_fs_path++
        } elseif ($l -match 'ImageDownloader: HTTP 200 OK') {
            $img_ok++
        } elseif ($l -match 'ImageDownloader: HTTP fetch failed, code=(\d+)') {
            $img_fail++
            $code = $matches[1]
            $img_fail_by_code[$code] = $img_fail_by_code[$code] + 1
        } elseif ($l -match 'installer: waiting local prebuffer target=(\d+)') {
            $prebuffer_target = [int64]$matches[1]
        } elseif ($l -match 'installer: installation completed successfully starvation_events=(\d+)') {
            $completed_starvation = [int]$matches[1]
        } elseif ($l -match 'hybrid: start install') {
            $installs++
        }
    }

    Print-Header "APP ($Path)"
    Write-Output ("installs                        = " + $installs)
    Write-Output ("starvation events (wait>=500ms) = " + $starv_count)
    Write-Output ("starvation wait p50             = " + (Pct $starv_waits.ToArray() 50) + " ms")
    Write-Output ("starvation wait p95             = " + (Pct $starv_waits.ToArray() 95) + " ms")
    Write-Output ("starvation wait max             = " + $starv_max + " ms")
    if ($install_speeds.Count -gt 0) {
        Write-Output ("install_speed avg/max           = " + [math]::Round(($install_speeds | Measure-Object -Average -Maximum).Average, 1) + " / " + ($install_speeds | Measure-Object -Maximum).Maximum + " KB/s")
        Write-Output ("source_speed avg/max           = " + [math]::Round(($source_speeds | Measure-Object -Average -Maximum).Average, 1) + " / " + ($source_speeds | Measure-Object -Maximum).Maximum + " KB/s")
        if ($consume_speeds.Count -gt 0) {
            Write-Output ("consume_speed avg/max          = " + [math]::Round(($consume_speeds | Measure-Object -Average -Maximum).Average, 1) + " / " + ($consume_speeds | Measure-Object -Maximum).Maximum + " KB/s")
        }
    }
    if ($rb_min -ne [int64]::MaxValue) {
        Write-Output ("rb_avail min (stats samples)   = " + $rb_min + " bytes")
    }
    Write-Output ("slow-peer boosts (events/total) = " + $boost_events + " / " + $boost_total)
    Write-Output ("stall mode entries/exits        = " + $stall_entries + " / " + $stall_exits)
    Write-Output ("pace sleeps (events/total ms)   = " + $pace_sleeps + " / " + $pace_sleep_ms_total)
    Write-Output ("cnmt insufficient data          = " + $cnmt_insufficient)
    Write-Output ("cnmt in-memory extract fails    = " + $cnmt_inmem_fail)
    Write-Output ("cnmt ContentMeta FS fallbacks   = " + $cnmt_fs_path)
    Write-Output ("images ok / failed              = " + $img_ok + " / " + $img_fail)
    foreach ($k in $img_fail_by_code.Keys | Sort-Object) {
        Write-Output ("  image 404-by-code $k             = " + $img_fail_by_code[$k])
    }
    if ($prebuffer_target -gt 0) {
        Write-Output ("prebuffer target                = " + $prebuffer_target + " bytes")
    }
    if ($completed_starvation -ge 0) {
        Write-Output ("completed starvation_events     = " + $completed_starvation)
    }
    if ($summ) {
        Write-Output ("session summary wait p50/p95/max = " + $summ.p50 + " / " + $summ.p95 + " / " + $summ.max + " ms")
        Write-Output ("session summary stall_total      = " + $summ.stall + " ms")
        Write-Output ("session summary install avg/max   = " + $summ.inst_avg + " / " + $summ.inst_max + " KB/s")
        Write-Output ("session summary source avg/max    = " + $summ.src_avg + " / " + $summ.src_max + " KB/s")
        if ($summ.cons_avg -gt 0) {
            Write-Output ("session summary consume avg/max  = " + $summ.cons_avg + " / " + $summ.cons_max + " KB/s")
        }
        if ($summ.live_min -ge 0) {
            Write-Output ("session summary live_peers_min   = " + $summ.live_min)
        }
    }
}

function Analyze-EngineLog([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) {
        Write-Output "engine log: not found ($Path)"
        return
    }
    $lines = Get-Content -LiteralPath $Path

    $tracker_attempts = @{}
    $tracker_ok       = @{}
    $tracker_peers    = @{}
    $tracker_fail     = @{}
    $fail_reasons = @{}
    $recovery_total = 0
    $recovery_max_round = 0
    $recovered_total = 0
    $live_values = [System.Collections.Generic.List[int]]::new()
    $dht_bg_lines = 0

    foreach ($l in $lines) {
        if ($l -match '\[meta\] tracker [\d.]+s \((\d+)\) (.+)$') {
            $peers = [int]$matches[1]
            $url   = $matches[2].TrimEnd()
            $tracker_attempts[$url] = $tracker_attempts[$url] + 1
            $tracker_ok[$url]       = $tracker_ok[$url] + 1
            $tracker_peers[$url]    = $tracker_peers[$url] + $peers
        } elseif ($l -match '\[meta\] tracker [\d.]+s \(failed: ([^)]*)\) (.+)$') {
            $reason = $matches[1]
            $url    = $matches[2].TrimEnd()
            $tracker_attempts[$url] = $tracker_attempts[$url] + 1
            $tracker_fail[$url]     = $tracker_fail[$url] + 1
            $fail_reasons[$reason]  = $fail_reasons[$reason] + 1
        } elseif ($l -match 'starvation recovery \(round (\d+)\):.*?live=(\d+)') {
            $recovery_total++
            $round = [int]$matches[1]
            if ($round -gt $recovery_max_round) { $recovery_max_round = $round }
            $live_values.Add([int]$matches[2])
        } elseif ($l -match 'starvation recovered \(delivered block after \d+ rounds\)') {
            $recovered_total++
        } elseif ($l -match '\[dht\] background ') {
            $dht_bg_lines++
        }
    }

    Print-Header "ENGINE ($Path)"
    Write-Output ("tracker announce attempts      = " + ($tracker_attempts.Values | Measure-Object -Sum).Sum)
    Write-Output ("starvation recovery events     = " + $recovery_total + " (max round " + $recovery_max_round + ")")
    Write-Output ("starvation recovered events    = " + $recovered_total)
    if ($live_values.Count -gt 0) {
        $avgLive = [math]::Round(($live_values | Measure-Object -Average).Average, 1)
        Write-Output ("live peers at recovery min/avg = " + ($live_values | Measure-Object -Minimum).Minimum + " / " + $avgLive)
    }
    Write-Output ("[dht] background log lines     = " + $dht_bg_lines)
    Write-Output ("--- failures by reason:")
    foreach ($k in $fail_reasons.Keys | Sort-Object) {
        Write-Output ("  $k = " + $fail_reasons[$k])
    }
    Write-Output ("--- tracker success (ok/attempts, peers):")
    foreach ($url in $tracker_attempts.Keys | Sort-Object { $tracker_ok[$_] -eq $null }) {
        $ok = if ($tracker_ok[$url]) { $tracker_ok[$url] } else { 0 }
        $peers = if ($tracker_peers[$url]) { $tracker_peers[$url] } else { 0 }
        Write-Output ("  $url : $ok/$($tracker_attempts[$url]) peers=$peers")
    }
}

if (-not $AppLog -and -not $EngineLog) {
    Write-Output "Usage: log_stats.ps1 -AppLog <log1.txt> [-EngineLog <engine1.log>]"
    exit 1
}

Analyze-AppLog $AppLog
Analyze-EngineLog $EngineLog

Write-Output ""
Write-Output "Done."