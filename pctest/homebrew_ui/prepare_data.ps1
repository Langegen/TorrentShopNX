# Data for the homebrew store PC window: repo.json (fortheusers), homebrew.json
# (our index), manifests from pctest fixtures, icons of the most popular packages
# and of the index.
# Run from the repo root:  powershell -File pctest\homebrew_ui\prepare_data.ps1 [-Out build_pc\hb_ui_data] [-Top 70]
param([string]$Out = "build_pc\hb_ui_data", [int]$Top = 70)

[Net.ServicePointManager]::SecurityProtocol = 'Tls12'
New-Item -ItemType Directory -Force "$Out\icons", "$Out\manifests" | Out-Null
Invoke-WebRequest -UseBasicParsing "https://switch.cdn.fortheusers.org/repo.json" -OutFile "$Out\repo.json"
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/Langegen/switch-game-collection/refs/heads/main/homebrew.json" -OutFile "$Out\homebrew.json"
Copy-Item "pctest\homebrew_fixtures\manifests\*" "$Out\manifests\" -Force

$repo = Get-Content "$Out\repo.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$urls = @()
foreach ($p in ($repo.packages | Sort-Object app_dls -Descending | Select-Object -First $Top)) {
    $urls += "https://switch.cdn.fortheusers.org/packages/$($p.name)/icon.png"
}
foreach ($n in 'Checkpoint', 'sys-clk', 'Status-Monitor-Overlay', 'MissionControl') {
    $urls += "https://switch.cdn.fortheusers.org/packages/$n/screen.png"
}
# homebrew.json has keys differing only in case (Taisei / taisei): read it with .NET JSON, not ConvertFrom-Json.
Add-Type -AssemblyName System.Web.Extensions
$ser = New-Object System.Web.Script.Serialization.JavaScriptSerializer
$ser.MaxJsonLength = 64MB
$index = $ser.DeserializeObject((Get-Content "$Out\homebrew.json" -Raw -Encoding UTF8))
foreach ($p in $index["packages"]) { if ($p["icon_url"]) { $urls += $p["icon_url"] } }

# The window looks icons up by the URL with every non-alphanumeric character as "_".
$ok = 0
foreach ($u in $urls) {
    $safe = $u -replace '[^A-Za-z0-9]', '_'
    try {
        $b = (Invoke-WebRequest -UseBasicParsing $u -TimeoutSec 20).RawContentStream.ToArray()
        $ext = if ($b[0] -eq 0xFF) { '.jpg' } else { '.png' }
        [IO.File]::WriteAllBytes((Join-Path (Resolve-Path $Out) "icons\$safe$ext"), $b)
        $ok++
    } catch {}
}
Write-Host "data in $Out, icons: $ok of $($urls.Count)"
