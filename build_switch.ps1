$env:DEVKITPRO = "C:/devkitPro"
$env:DEVKITARM = "C:/devkitPro/devkitARM"
$env:DEVKITPPC = "C:/devkitPro/devkitPPC"
$env:PATH = "C:\devkitPro\devkitA64\bin;C:\devkitPro\tools\bin;C:\devkitPro\msys2\usr\bin;$env:PATH"

Write-Host "Starting Nintendo Switch build..."
Write-Host "DEVKITPRO = $env:DEVKITPRO"

# libarchive with 7z ARM64 filter support (see scripts/build_libarchive_switch.sh)
if (-not (Test-Path "_external/libarchive-switch/lib/libarchive.a")) {
    Write-Host "Building libarchive for Switch (one-time)..."
    & "C:\devkitPro\msys2\usr\bin\bash.exe" -lc "cd '$($PWD.Path -replace '\\','/')' && bash scripts/build_libarchive_switch.sh"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "libarchive build FAILED" -ForegroundColor Red
        exit $LASTEXITCODE
    }
}

$nproc = 2
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
