# Builds and runs the homebrew store tests (pctest/test_homebrew.cpp).
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File pctest\build_homebrew.ps1 [full repo.json [homebrew.json]]

$env:PATH = "C:\devkitPro\msys2\mingw64\bin;C:\devkitPro\msys2\usr\bin;$env:PATH"
$GPP = "C:\devkitPro\msys2\mingw64\bin\g++.exe"
if (-not (Test-Path $GPP)) { $GPP = "g++" }

& $GPP -O1 -g -Wall -Wextra -std=c++17 -Isource -I_external\borealis\library\include `
    -o pctest\test_homebrew.exe `
    pctest\test_homebrew.cpp source\catalog\homebrew_package.cpp
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED"; exit 1 }

if ($args.Count -gt 1) {
    & .\pctest\test_homebrew.exe pctest\homebrew_fixtures $args[0] $args[1]
} elseif ($args.Count -gt 0) {
    & .\pctest\test_homebrew.exe pctest\homebrew_fixtures $args[0]
} else {
    & .\pctest\test_homebrew.exe pctest\homebrew_fixtures
}
exit $LASTEXITCODE
