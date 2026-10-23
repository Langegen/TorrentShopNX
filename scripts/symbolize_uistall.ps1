# Turns "[uistall]" lines from log.txt into function names.
#
#   powershell -File scripts/symbolize_uistall.ps1 -Log C:\path\to\log.txt [-Elf TorrentShopNX.elf]
#
# The ELF must be the exact build that produced the log.
param(
    [Parameter(Mandatory = $true)][string]$Log,
    [string]$Elf = "TorrentShopNX.elf"
)

$addr2line = "C:\devkitPro\devkitA64\bin\aarch64-none-elf-addr2line.exe"
if (-not (Test-Path $addr2line)) { Write-Error "addr2line not found: $addr2line"; exit 1 }
if (-not (Test-Path $Elf)) { Write-Error "ELF not found: $Elf"; exit 1 }

$lines = Select-String -Path $Log -Pattern "\[uistall\] gap=" | ForEach-Object { $_.Line }
if (-not $lines) { Write-Host "No [uistall] samples in $Log"; exit 0 }

foreach ($line in $lines) {
    Write-Host ""
    Write-Host $line -ForegroundColor Cyan
    $addrs = @()
    if ($line -match "pc=(0x[0-9a-f]+)") { $addrs += $Matches[1] }
    if ($line -match "lr=(0x[0-9a-f]+)") { $addrs += $Matches[1] }
    if ($line -match "stack=([0-9a-fx,]+)") { $addrs += $Matches[1].Split(",") }
    $addrs = $addrs | Where-Object { $_ -and $_ -ne "0x0" }
    if (-not $addrs) { continue }
    # A return address points after the call: look up addr-4 for the call site.
    $lookup = $addrs | ForEach-Object { "0x{0:x}" -f ([Convert]::ToUInt64($_.Substring(2), 16) - 4) }
    $out = & $addr2line -f -C -i -p -e $Elf @lookup
    $out | ForEach-Object { Write-Host "    $_" }
}
