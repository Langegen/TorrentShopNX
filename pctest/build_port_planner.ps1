# Builds and runs the port planner table test (pctest/test_port_planner.cpp).
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File pctest\build_port_planner.ps1

$env:PATH = "C:\devkitPro\msys2\mingw64\bin;C:\devkitPro\msys2\usr\bin;$env:PATH"
$GPP = "C:\devkitPro\msys2\mingw64\bin\g++.exe"
if (-not (Test-Path $GPP)) { $GPP = "g++" }

& $GPP -O1 -g -Wall -std=c++17 -Isource -Isource\utils -I_external\borealis\library\include `
    -o pctest\test_port_planner.exe `
    pctest\test_port_planner.cpp source\installer\PortInstallPlanner.cpp source\utils\archive_peek.cpp
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED"; exit 1 }

& .\pctest\test_port_planner.exe pctest\port_fixtures
exit $LASTEXITCODE
