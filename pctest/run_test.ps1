param(
    [string]$TestExe = "pctest\test_engine.exe"
)

$env:PATH = "C:\devkitPro\msys2\mingw64\bin;C:\devkitPro\msys2\usr\bin;$env:PATH"
Write-Host "Running: $TestExe"
& $TestExe
exit $LASTEXITCODE
