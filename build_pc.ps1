# Builds TorrentShopNX for PC (Windows) using MinGW-w64 and CMake
$env:PATH = "C:\devkitPro\msys2\mingw64\bin;C:\devkitPro\msys2\usr\bin;$env:PATH"

$env:CC = "C:/devkitPro/msys2/mingw64/bin/gcc.exe"
$env:CXX = "C:/devkitPro/msys2/mingw64/bin/g++.exe"

$buildDir = "build_pc"

if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

Write-Host "Configuring CMake for Desktop (Windows)..." -ForegroundColor Cyan
cmake -B $buildDir -G "Unix Makefiles" -DCMAKE_SYSTEM_NAME=Windows -DPLATFORM_DESKTOP=ON -DCMAKE_BUILD_TYPE=Debug

if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configuration failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "Building TorrentShopNX..." -ForegroundColor Cyan
cmake --build $buildDir -j8

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "Build succeeded: $buildDir\TorrentShopNX.exe" -ForegroundColor Green
