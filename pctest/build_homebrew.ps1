# Builds and runs the homebrew store tests:
#   pctest/test_homebrew.cpp          data, path rules, merge, install/removal plans
#   pctest/test_homebrew_catalog.cpp  download schedule, ETag, cache, registry (fake network)
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File pctest\build_homebrew.ps1 [full repo.json [homebrew.json]]

$env:PATH = "C:\devkitPro\msys2\mingw64\bin;C:\devkitPro\msys2\usr\bin;$env:PATH"
$GPP = "C:\devkitPro\msys2\mingw64\bin\g++.exe"
if (-not (Test-Path $GPP)) { $GPP = "g++" }
$flags = @("-O1", "-g", "-Wall", "-Wextra", "-std=c++17", "-Isource", "-I_external\borealis\library\include")

& $GPP @flags -o pctest\test_homebrew.exe pctest\test_homebrew.cpp source\catalog\homebrew_package.cpp
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED: test_homebrew"; exit 1 }
& $GPP @flags -o pctest\test_homebrew_catalog.exe pctest\test_homebrew_catalog.cpp `
    source\catalog\homebrew_catalog.cpp source\catalog\homebrew_package.cpp
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED: test_homebrew_catalog"; exit 1 }

$failed = 0
if ($args.Count -gt 1) {
    & .\pctest\test_homebrew.exe pctest\homebrew_fixtures $args[0] $args[1]
} elseif ($args.Count -gt 0) {
    & .\pctest\test_homebrew.exe pctest\homebrew_fixtures $args[0]
} else {
    & .\pctest\test_homebrew.exe pctest\homebrew_fixtures
}
if ($LASTEXITCODE -ne 0) { $failed = 1 }
& .\pctest\test_homebrew_catalog.exe pctest\homebrew_fixtures
if ($LASTEXITCODE -ne 0) { $failed = 1 }

Remove-Item pctest\test_homebrew.exe, pctest\test_homebrew_catalog.exe -ErrorAction SilentlyContinue
exit $failed
