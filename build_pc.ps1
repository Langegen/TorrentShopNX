# Builds TorrentShopNX for PC (Windows) using MinGW-w64 and CMake
$env:TEMP = "D:\temp"
$env:TMP = "D:\temp"
$env:PATH = "C:\devkitPro\msys2\mingw64\bin;C:\devkitPro\msys2\usr\bin;$env:PATH"

$env:CC = "gcc"
$env:CXX = "g++"

$buildDir = "build_pc"

if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

Write-Host "Configuring CMake for Desktop (Windows)..." -ForegroundColor Cyan
cmake -B $buildDir -G "Ninja" -DCMAKE_SYSTEM_NAME=Windows -DPLATFORM_DESKTOP=ON -DCMAKE_BUILD_TYPE=Debug

if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configuration failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "Building TorrentShopNX..." -ForegroundColor Cyan
cmake --build $buildDir -j4

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "Syncing resources to build directory..." -ForegroundColor Cyan
Copy-Item -Path "resources\*" -Destination "$buildDir\resources" -Recurse -Force

Write-Host "Syncing MinGW runtime DLLs to build directory..." -ForegroundColor Cyan
$dlls = @(
    'libarchive-13.dll', 'libb2-1.dll', 'libbrotlicommon.dll', 'libbrotlidec.dll',
    'libbz2-1.dll', 'libcrypto-3-x64.dll', 'libcurl-4.dll', 'libexpat-1.dll',
    'libgcc_s_seh-1.dll', 'libiconv-2.dll', 'libidn2-0.dll', 'libintl-8.dll',
    'liblz4.dll', 'liblzma-5.dll', 'libnghttp2-14.dll', 'libnghttp3-9.dll',
    'libngtcp2-16.dll', 'libngtcp2_crypto_ossl-0.dll', 'libpsl-5.dll', 'libssh2-1.dll',
    'libssl-3-x64.dll', 'libstdc++-6.dll', 'libunistring-5.dll', 'libwinpthread-1.dll',
    'libzstd.dll', 'zlib1.dll'
)
foreach ($dll in $dlls) {
    $src = "C:\devkitPro\msys2\mingw64\bin\$dll"
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination $buildDir -Force
    }
}

Write-Host "Build succeeded: $buildDir\TorrentShopNX.exe" -ForegroundColor Green
