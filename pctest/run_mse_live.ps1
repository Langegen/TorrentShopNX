# Live MSE test against libtorrent with encryption forced.
#   powershell -ExecutionPolicy Bypass -File pctest\run_mse_live.ps1 -Python <python.exe with libtorrent> [-Mode forced|enabled]
# Needs: pctest\test_mse_live.exe (pctest\build.ps1) and python-libtorrent
# (pip install libtorrent, ideally in a venv).
param([Parameter(Mandatory = $true)][string]$Python, [string]$Mode = "forced", [int]$Port = 47123)

$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $env:TEMP "tsnx_mse_live"
Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $dir | Out-Null
$env:PATH = "C:\devkitPro\msys2\mingw64\bin;$env:PATH"
Push-Location $root
try {
    & .\pctest\test_mse_live.exe --prepare $dir
    $seed = Start-Process -FilePath $Python -ArgumentList @("-I", "pctest\mse_seed.py", $dir, "$Port", "120", $Mode) `
            -NoNewWindow -PassThru -RedirectStandardOutput "$dir\seed.out" -RedirectStandardError "$dir\seed.err"
    Start-Sleep -Seconds 3
    $p = Start-Process -FilePath .\pctest\test_mse_live.exe -ArgumentList @("--run", $dir, "$Port") -NoNewWindow -PassThru `
         -RedirectStandardOutput "$dir\run.out" -RedirectStandardError "$dir\run.err"
    $null = $p.Handle
    if (-not $p.WaitForExit(150000)) { $p.Kill() }
    $code = $p.ExitCode
    Stop-Process -Id $seed.Id -Force -ErrorAction SilentlyContinue
    Get-Content "$dir\run.out"
    "--- seeder:"; Get-Content "$dir\seed.out" | Select-Object -First 12
    "--- engine MSE lines:"; Select-String -Path "$dir\run.err" -Pattern "\[mse\] sess|MSE handshake ok" | Select-Object -First 6 | ForEach-Object { $_.Line }
    exit $code
} finally { Pop-Location }
