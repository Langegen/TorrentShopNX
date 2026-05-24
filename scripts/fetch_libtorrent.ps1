param(
    [string]$Tag = "v1.2.20",
    [string]$Repo = "https://github.com/arvidn/libtorrent.git",
    [string]$OutDir = "_external/libtorrent-v1.2.20"
)

if (!(Test-Path "_external")) {
    New-Item -ItemType Directory -Path "_external" | Out-Null
}

if (Test-Path $OutDir) {
    Write-Output "exists: $OutDir"
    exit 0
}

git clone --depth 1 --branch $Tag $Repo $OutDir

