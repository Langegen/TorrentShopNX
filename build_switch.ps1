$env:DEVKITPRO = "C:/devkitPro"
$env:DEVKITARM = "C:/devkitPro/devkitARM"
$env:DEVKITPPC = "C:/devkitPro/devkitPPC"
$env:PATH = "C:\devkitPro\devkitA64\bin;C:\devkitPro\tools\bin;C:\devkitPro\msys2\usr\bin;$env:PATH"

Write-Host "Starting Nintendo Switch build..."
Write-Host "DEVKITPRO = $env:DEVKITPRO"

$nproc = [System.Environment]::ProcessorCount
if ($nproc -lt 1) { $nproc = 4 }
Write-Host "Parallel jobs: $nproc"
& make -j $nproc

$exitCode = $LASTEXITCODE
if ($exitCode -eq 0) {
    Write-Host "Nintendo Switch build SUCCESSFUL!" -ForegroundColor Green
    if (Test-Path "TorrentShopNX.nro") {
        $nro = Get-Item "TorrentShopNX.nro"
        Write-Host "Target: $($nro.FullName) ($([math]::Round($nro.Length / 1MB, 2)) MB, LastWriteTime: $($nro.LastWriteTime))"
    }
} else {
    Write-Host "Nintendo Switch build FAILED with exit code $exitCode" -ForegroundColor Red
}
exit $exitCode
