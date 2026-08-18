# pctest\run_verification.ps1
#
# Запускает PC-тест движка против magnet-ссылки и собирает итог по сетевому
# поведению: UPnP-проброс, входящие подключения, живые пиры, скорость, ошибки
# соединений. Это «проверка на вашем PC»: сеть/роутер/NAT у вас дома те же,
# что у Switch, поэтому результат близок к тому, что увидит консоль.
#
# Использование (PowerShell, из корня репозитория):
#   powershell -ExecutionPolicy Bypass -File pctest\run_verification.ps1 "magnet:?..." [file_index] [duration_sec] [apptest|diagtest]
#
# Пример:
#   powershell -ExecutionPolicy Bypass -File pctest\run_verification.ps1 "magnet:?xt=urn:btih:9B71...&tr=http%3A..." 0 120

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Magnet,

    [Parameter(Position = 1)]
    [int]$FileIndex = 0,

    [Parameter(Position = 2)]
    [int]$Duration = 120,

    [ValidateSet("apptest", "diagtest")]
    [string]$Exe = "apptest"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exePath = Join-Path $root ("pctest\{0}.exe" -f $Exe)
if (-not (Test-Path $exePath)) {
    Write-Host ("NOT FOUND: {0}" -f $exePath)
    Write-Host "Соберите сначала тесты:"
    Write-Host "  powershell -ExecutionPolicy Bypass -File pctest\build.ps1"
    exit 1
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$outFile = Join-Path $env:TEMP ("tsnx_verify_{0}_{1}_out.txt" -f $Exe, $stamp)
$errFile = Join-Path $env:TEMP ("tsnx_verify_{0}_{1}_err.txt" -f $Exe, $stamp)
$wdFile = Join-Path $root "watchdog.log"
Remove-Item $outFile, $errFile, $wdFile -ErrorAction SilentlyContinue

Write-Host "== TorrentShopNX PC verification =="
Write-Host ("exe      : {0}" -f $exePath)
Write-Host ("duration : {0}s   file_index: {1}   test: {2}" -f $Duration, $FileIndex, $Exe)
Write-Host ("magnet   : {0}" -f $Magnet)
Write-Host ""

$argList = @('"' + $Magnet + '"', "$FileIndex", "$Duration")
$p = Start-Process -FilePath $exePath -ArgumentList $argList `
        -WorkingDirectory $root `
        -RedirectStandardOutput $outFile `
        -RedirectStandardError $errFile `
        -PassThru -WindowStyle Hidden

$timedOut = $false
$waitSec = $Duration + 90
if (-not $p.WaitForExit(($waitSec * 1000))) {
    Write-Host ("WARNING: процесс не завершился за {0}s -> убит." -f $waitSec)
    Write-Host "  Либо сворм не отдал ни байта (тест заблокирован в read), либо движок завис."
    Write-Host "  watchdog.log (в итоге ниже) покажет, жив ли движок."
    Stop-Process -Id $p.Id -Force
    $timedOut = $true
} else {
    Write-Host ("процесс завершился, код={0}" -f $p.ExitCode)
}
Write-Host ""

$out = if (Test-Path $outFile) { Get-Content $outFile } else { @() }
$err = if (Test-Path $errFile) { Get-Content $errFile } else { @() }

# ---- UPnP (последняя строка по теме = итоговое состояние) ----
$upnp = "не запускался/нет строк"
foreach ($line in $err) {
    if ($line -match "\[upnp\] port (\d+) mapped") { $upnp = "OK: порт $($Matches[1]) проброшен" }
    elseif ($line -match "no IGD found via SSDP") { $upnp = "IGD не найден (UPnP недоступен)" }
    elseif ($line -match "AddPortMapping failed") { $upnp = "IGD найден, но маппинг отказал" }
    elseif ($line -match "no WANIP/WANPPP service") { $upnp = "IGD без WANIP/WANPPP-сервиса" }
    elseif ($line -match "failed to fetch the device description") { $upnp = "IGD есть, описание не скачалось" }
}

# ---- Listener ----
$listenPort = "нет"
foreach ($line in $err) {
    if ($line -match "\[listen\] listening on port (\d+)") { $listenPort = $Matches[1]; break }
}

# ---- Счётчики из stderr ----
$inc = @($err | Select-String "incoming handshake ok").Count
$hsOk = @($err | Select-String -Pattern "handshake ok live=").Count
$connTimeout = @($err | Select-String "close connect timeout").Count
$soerr = @($err | Select-String "connect soerr").Count
$mseRefused = @($err | Select-String "mse refused").Count
$starvRec = @($err | Select-String "starvation recovered").Count

# Максимум живых пиров, видимый в логе хендшейков (работает и когда тест
# заблокирован в read и не печатает статистику).
$maxLiveErr = 0
foreach ($line in $err) {
    if ($line -match "handshake ok live=(\d+)/") {
        $n = [int]$Matches[1]
        if ($n -gt $maxLiveErr) { $maxLiveErr = $n }
    }
}

$maxTotalPeers = 0
foreach ($line in $err) {
    if ($line -match "added \d+ peers \(total (\d+)\)") {
        $n = [int]$Matches[1]
        if ($n -gt $maxTotalPeers) { $maxTotalPeers = $n }
    }
}

# ---- Строки статистики из stdout (форматы apptest и diagtest) ----
$maxLive = 0; $maxKnown = 0; $maxDl = 0.0; $sumDl = 0.0; $dlN = 0
$lastProgress = 0.0; $lastState = "нет данных"; $lastDone = 0
foreach ($line in $out) {
    # apptest: [ 12.3s] st=StreamingOrInstalling  dl=123.4KB/s seeds=1 peers=2 known=30 ... progress= 1.23% ... done=5
    if ($line -match "st=(\S+)\s+dl=([\d.]+)KB/s seeds=\d+ peers=(\d+) known=(\d+) .*progress=\s*([\d.]+)% .*done=(\d+)") {
        $lastState = $Matches[1]
        $dl = [double]$Matches[2]
        $live = [int]$Matches[3]
        $known = [int]$Matches[4]
        $lastProgress = [double]$Matches[5]
        $lastDone = [int64]$Matches[6]
        if ($live -gt $maxLive) { $maxLive = $live }
        if ($known -gt $maxKnown) { $maxKnown = $known }
        if ($dl -gt $maxDl) { $maxDl = $dl }
        $sumDl += $dl; $dlN++
        continue
    }
    # diagtest: [  3.8s] peers=32 live=2 peak=5 conn=23 claiming=2 idle=0 speed=1372.9KB/s progress=0.21% ... timeouts=73 hs_fail=0 ...
    if ($line -match "peers=(\d+) live=(\d+) peak=\d+ conn=\d+ .*speed=([\d.]+)KB/s progress=([\d.]+)%") {
        $known = [int]$Matches[1]
        $live = [int]$Matches[2]
        $dl = [double]$Matches[3]
        $lastProgress = [double]$Matches[4]
        $lastState = "diagtest"
        if ($live -gt $maxLive) { $maxLive = $live }
        if ($known -gt $maxKnown) { $maxKnown = $known }
        if ($dl -gt $maxDl) { $maxDl = $dl }
        $sumDl += $dl; $dlN++
    }
}
$avgDl = if ($dlN -gt 0) { $sumDl / $dlN } else { 0.0 }
if ($maxLiveErr -gt $maxLive) { $maxLive = $maxLiveErr }

# ---- SUMMARY-блок apptest (если тест дошёл до конца) ----
$netAvg = $null; $elapsed = $null
foreach ($line in $out) {
    if ($line -match "SUMMARY elapsed=(\d+)s") { $elapsed = [int]$Matches[1] }
    if ($line -match "NET_avg=([\d.]+)KB/s") { $netAvg = [double]$Matches[1] }
}

# ---- Watchdog (последняя строка) ----
$wdLine = $null
if (Test-Path $wdFile) {
    $wdLine = (Get-Content $wdFile -Tail 1)
}

Write-Host "============= ИТОГ ПРОВЕРКИ ============="
Write-Host ("Порт листенера     : {0}" -f $listenPort)
Write-Host ("UPnP               : {0}" -f $upnp)
Write-Host ("Входящие подключ.  : {0}" -f $inc)
Write-Host ("Исходящие хендшейки: {0}   (макс. живых пиров: {1})" -f $hsOk, $maxLive)
Write-Host ("Пул пиров (макс.)  : {0}   (known в статистике: {1})" -f $maxTotalPeers, $maxKnown)
Write-Host ("Таймауты коннекта  : {0}   отказы(soerr): {1}   MSE-refused+retry: {2}" -f $connTimeout, $soerr, $mseRefused)
Write-Host ("Восстановления голода: {0}" -f $starvRec)
if ($dlN -gt 0) {
    Write-Host ("Скорость (средн/пик): {0:N1} / {1:N1} KB/s   ({2} сэмплов)" -f $avgDl, $maxDl, $dlN)
}
if ($null -ne $netAvg) {
    Write-Host ("Скорость (сеть)    : {0:N1} KB/s за {1} с" -f $netAvg, $elapsed)
}
Write-Host ("Прогресс           : {0}%   (кусков скачано: {1})" -f $lastProgress, $lastDone)
Write-Host ("Состояние в конце  : {0}" -f $lastState)
if ($timedOut) {
    Write-Host "ВНИМАНИЕ           : тест не завершился сам — смотри watchdog ниже."
}
if ($wdLine) {
    Write-Host ("watchdog (возраст потоков в сек): {0}" -f $wdLine)
    Write-Host "  порядок: dht disc listen upnp net writer reader wd; 99999 = поток ни разу не тикал."
    Write-Host "  все возрасты малы = движок жив (завис только тест в read); net/dht/writer растут = движок завис."
}
Write-Host ("Полные логи        : {0}" -f $outFile)
Write-Host ("                     {0}" -f $errFile)
Write-Host "================================================"
