# pctest\qb_reference.ps1
#
# Контрольный замер: запускает установленный qBittorrent с заданным magnet на
# N секунд и снимает его реальные показатели (скорость, сиды, пиры, DHT).
# Это эталон для сравнения с нашим движком: если qBittorrent качает быстро,
# а движок нет — проблема в движке; если qBittorrent тоже медленный — сворм
# беден, и дело не в клиенте.
#
# Конфиг qBittorrent бэкапится и восстанавливается; временная загрузка
# удаляется. Использование:
#   powershell -ExecutionPolicy Bypass -File pctest\qb_reference.ps1 "magnet:?..." [duration_sec]

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Magnet,

    [Parameter(Position = 1)]
    [int]$Duration = 120
)

$ErrorActionPreference = "Stop"
$qbIni = Join-Path $env:APPDATA "qBittorrent\qBittorrent.ini"
$qbExe = "C:\Program Files\qBittorrent\qbittorrent.exe"
$webPort = 18080
$savePath = Join-Path $env:TEMP "qb_reference_dl"

if (-not (Test-Path $qbIni)) { Write-Error "qBittorrent.ini not found: $qbIni"; exit 1 }
if (-not (Test-Path $qbExe)) { Write-Error "qbittorrent.exe not found"; exit 1 }

$backup = "$qbIni.reference_bak"
Copy-Item $qbIni $backup -Force

try {
    # --- включить WebUI в конфиге (бэкап уже сделан) ---
    $lines = Get-Content $qbIni | Where-Object { $_ -notmatch '^WebUI\\' }
    $webui = @(
        "WebUI\Enabled=true",
        "WebUI\Port=$webPort",
        "WebUI\LocalHostAuth=false",
        "WebUI\HostHeaderValidation=false",
        "WebUI\CSRFProtection=false"
    )
    # WebUI-опции живут в секции [Preferences]; вставить ключи сразу за ней.
    $prefIdx = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\[Preferences\]') { $prefIdx = $i; break }
    }
    if ($prefIdx -lt 0) {
        $lines += "[Preferences]"
        $prefIdx = $lines.Count - 1
    }
    $result = @()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $result += $lines[$i]
        if ($i -eq $prefIdx) { $result += $webui }
    }
    Set-Content -Path $qbIni -Value $result -Encoding ASCII

    Remove-Item $savePath -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $savePath | Out-Null

    Write-Host "== qBittorrent reference =="
    $p = Start-Process -FilePath $qbExe -PassThru
    Write-Host "qbittorrent started pid=$($p.Id), waiting for WebUI..."

    $api = "http://127.0.0.1:$webPort/api/v2"
    $ready = $false
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Seconds 1
        try {
            $v = Invoke-RestMethod -Uri "$api/app/version" -TimeoutSec 3
            Write-Host "WebUI up, qBittorrent $v"
            $ready = $true
            break
        } catch { }
    }
    if (-not $ready) { Write-Error "WebUI did not come up"; exit 1 }

    # --- добавить magnet ---
    $body = @{ urls = $Magnet; savepath = $savePath }
    Invoke-RestMethod -Method Post -Uri "$api/torrents/add" -Body $body | Out-Null
    Start-Sleep -Seconds 2

    # --- снимать показатели ---
    $hash = ""
    $start = Get-Date
    $samples = 0
    $sumDl = 0.0; $maxDl = 0.0; $maxSeeds = 0; $maxPeers = 0; $maxComplete = 0
    Write-Host ("[{0,6}s]  dlspeed  seeds/complete  leechs/incomplete  progress   state" -f 0)
    while ((Get-Date) -lt $start.AddSeconds($Duration)) {
        Start-Sleep -Seconds 3
        try {
            $data = Invoke-RestMethod -Uri "$api/sync/maindata" -TimeoutSec 5
        } catch { continue }
        if (-not $hash -and $data.torrents) {
            $hash = ($data.torrents.PSObject.Properties | Select-Object -First 1).Name
        }
        if ($hash) {
            $t = $data.torrents.$hash
            if ($t) {
                $dl = [double]$t.dlspeed
                $el = [int]((Get-Date) - $start).TotalSeconds
                $seeds = [int]$t.num_seeds; $complete = [int]$t.num_complete
                $leechs = [int]$t.num_leechs; $incomplete = [int]$t.num_incomplete
                $prog = [double]$t.progress
                Write-Host ("[{0,6}s]  {1,7:N0} B/s  {2}/{3}  {4}/{5}  {6:P1}  {7}" -f `
                    $el, $dl, $seeds, $complete, $leechs, $incomplete, $prog, $t.state)
                if ($dl -gt $maxDl) { $maxDl = $dl }
                $sumDl += $dl; $samples++
                if ($seeds -gt $maxSeeds) { $maxSeeds = $seeds }
                if (($seeds + $leechs) -gt $maxPeers) { $maxPeers = $seeds + $leechs }
                if ($complete -gt $maxComplete) { $maxComplete = $complete }
            }
        }
    }
    $avg = if ($samples -gt 0) { $sumDl / $samples } else { 0 }
    Write-Host "============= QBITTORRENT REFERENCE ============="
    Write-Host ("max seeds (num_seeds) : {0}" -f $maxSeeds)
    Write-Host ("max complete (всего в сворме): {0}" -f $maxComplete)
    Write-Host ("max peers (seeds+leechs): {0}" -f $maxPeers)
    Write-Host ("download: avg {0:N0} B/s  peak {1:N0} B/s  ({2} samples)" -f $avg, $maxDl, $samples)
    Write-Host ("hash: {0}" -f $hash)
    Write-Host "=================================================="
} finally {
    Get-Process qbittorrent -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Copy-Item $backup $qbIni -Force
    Remove-Item $savePath -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "cleanup done (config restored, temp download removed)"
}
