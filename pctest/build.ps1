# Builds the PC engine test on Windows with devkitPro MSYS2.
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File pctest\build.ps1

$GCC = "C:\devkitPro\msys2\usr\bin\gcc.exe"
$GPP = "C:\devkitPro\msys2\usr\bin\g++.exe"

$CFLAGS = @(
    "-O2", "-g", "-Wall",
    "-Ipctest\compat",
    "-Isource\engine",
    "-Iinclude\engine",
    "-Ipctest\deps\include",
    "-IC:\devkitPro\msys2\usr\include"
)

$CXXFLAGS = @(
    "-O2", "-g", "-Wall", "-std=c++11", "-DPOSIX",
    "-Ipctest\compat",
    "-Isource\engine",
    "-Iinclude\engine",
    "-Ipctest\deps\include",
    "-IC:\devkitPro\msys2\usr\include"
)

$COMMON_SOURCES = @(
    "source\engine\bencode.c",
    "source\engine\torrent_meta.c",
    "source\engine\udp_tracker.c",
    "source\engine\bt_peer.c",
    "source\engine\torrentfs.c",
    "source\engine\magnet.c",
    "source\engine\dht.c",
    "source\engine\dhtclient.c",
    "source\engine\engine.c",
    "source\engine\engine_log.c",
    "source\engine\mse.c",
    "pctest\utpstub.c"
)

$CPP_SOURCES = @(
    "source\engine\utp.cpp",
    "source\engine\utp_utils.cpp",
    "source\engine\utp_nb.cpp"
)

$CPP_OBJ = @(
    "pctest\utp.o",
    "pctest\utp_utils.o",
    "pctest\utp_nb.o"
)

$LIBS = @(
    "-Lpctest\deps\lib",
    "-lmbedcrypto", "-lmbedtls", "-lmbedx509",
    "-lcurl", "-lz", "-lpthread"
)

function Build-CppObjects {
    for ($i = 0; $i -lt $CPP_SOURCES.Count; $i++) {
        & $GPP @CXXFLAGS -c $CPP_SOURCES[$i] -o $CPP_OBJ[$i]
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}

# Builds the full app-stack test (real CustomEngineBackend/Scheduler/Health/
# Client + RingBuffer + engine) the same way the app links them.
function Build-AppTest {
    $OBJDIR = "pctest\obj"
    if (-not (Test-Path $OBJDIR)) { New-Item -ItemType Directory -Path $OBJDIR | Out-Null }

    $cobj = @()
    foreach ($src in $COMMON_SOURCES) {
        $o = Join-Path $OBJDIR ([System.IO.Path]::GetFileName($src) + ".o")
        & $GCC @CFLAGS -c $src -o $o
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        $cobj += $o
    }

    $APP_CPP = @(
        "pctest\apptest.cpp",
        "source\datasource\custom_engine_backend.cpp",
        "source\datasource\custom_engine_client.cpp",
        "source\datasource\custom_engine_scheduler.cpp",
        "source\datasource\custom_engine_health.cpp",
        "source\utils\log.cpp",
        "source\buffer\ring_buffer.cpp"
    )
    $appcpp = @()
    foreach ($src in $APP_CPP) {
        $o = Join-Path $OBJDIR ([System.IO.Path]::GetFileName($src) + ".o")
        & $GPP @CXXFLAGS -std=c++17 -pthread -DTSNX_USE_CUSTOM_ENGINE=1 -Isource -Iinclude -Isource\datasource -Isource\utils -Isource\buffer -c $src -o $o
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        $appcpp += $o
    }

    & $GPP @CXXFLAGS -std=c++17 $cobj $appcpp $CPP_OBJ -o "pctest\apptest" @LIBS
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "OK -> pctest\apptest"
}

function Build-Test($name) {
    $src = @("pctest\$name.c") + $COMMON_SOURCES + $CPP_OBJ
    & $GCC @CFLAGS @src -o "pctest\$name" @LIBS
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "OK -> pctest\$name"
}

Build-CppObjects

Build-Test "test_bencode"
Build-Test "test_magnet"
Build-Test "test_torrent_meta"
Build-Test "test_engine"
Build-Test "streamtest"
Build-Test "diagtest"
Build-Test "dhttest"
Build-AppTest
