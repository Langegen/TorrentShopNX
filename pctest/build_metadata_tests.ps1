# Builds and runs the standalone catalog metadata / locale unit tests:
#   pctest/test_language_metadata.cpp
#   pctest/test_localized_genre_terms.cpp
#   pctest/test_metadata_locale.cpp
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File pctest\build_metadata_tests.ps1

$env:PATH = "C:\devkitPro\msys2\mingw64\bin;C:\devkitPro\msys2\usr\bin;$env:PATH"
$GPP = "C:\devkitPro\msys2\mingw64\bin\g++.exe"
if (-not (Test-Path $GPP)) { $GPP = "g++" }
$flags = @("-O1", "-g", "-Wall", "-Wextra", "-std=c++17", "-Isource")

$tests = @(
    @{ Name = "test_language_metadata"; Src = "pctest\test_language_metadata.cpp" },
    @{ Name = "test_localized_genre_terms"; Src = "pctest\test_localized_genre_terms.cpp" },
    @{ Name = "test_metadata_locale"; Src = "pctest\test_metadata_locale.cpp" }
)

$failed = 0
foreach ($t in $tests) {
    $exe = "pctest\$($t.Name).exe"
    & $GPP @flags -o $exe $t.Src
    if ($LASTEXITCODE -ne 0) {
        Write-Host "BUILD FAILED: $($t.Name)" -ForegroundColor Red
        $failed = 1
        continue
    }
    & ".\$exe"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "TEST FAILED: $($t.Name)" -ForegroundColor Red
        $failed = 1
    }
    Remove-Item $exe -ErrorAction SilentlyContinue
}

exit $failed
