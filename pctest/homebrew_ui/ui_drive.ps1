# Drives build_pc\homebrew_ui.exe for screenshots: start, keys, mouse clicks, window shots.
#   . pctest\homebrew_ui\ui_drive.ps1
#   Start-HbUi -Lang ru -Theme light -Fresh ; Key DOWN ; Key RETURN ; Shot card ; Click 590 95 ; Stop-HbUi
# Keys: UP DOWN LEFT RIGHT RETURN(=A) ESC(=B) X Y LB RB MINUS (ZL/ZR have no key: click the category).
# Data: prepare_data.ps1 (build_pc\hb_ui_data). Shots: build_pc\hb_ui_shots or $env:HB_UI_SHOTS.
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
}
"@
[W]::SetProcessDPIAware() | Out-Null

$script:Root = (Resolve-Path "$PSScriptRoot\..\..\build_pc").Path
$script:Out = if ($env:HB_UI_SHOTS) { $env:HB_UI_SHOTS } else { "$script:Root\hb_ui_shots" }
New-Item -ItemType Directory -Force $script:Out | Out-Null
$script:Proc = $null

function Start-HbUi([string]$Lang = "ru", [string]$Theme = "light", [string]$Open = "", [switch]$Fresh) {
  if ($Fresh) { Remove-Item -Recurse -Force "$script:Root\hb_ui_sd", "$script:Root\hb_ui_cache" -ErrorAction SilentlyContinue }
  if (-not $env:HB_UI_DATA) { $env:HB_UI_DATA = "$script:Root\hb_ui_data" }
  $env:HB_UI_LANG = $Lang; $env:HB_UI_THEME = $Theme
  if ($env:PATH -notlike "*mingw64\bin*") { $env:PATH = "C:\devkitPro\msys2\mingw64\bin;$env:PATH" }
  if ($Open) { $env:HB_UI_OPEN = $Open } else { Remove-Item Env:HB_UI_OPEN -ErrorAction SilentlyContinue }
  $script:Proc = Start-Process -FilePath "$script:Root\homebrew_ui.exe" -WorkingDirectory $script:Root -PassThru `
      -RedirectStandardOutput "$script:Out\stdout.txt" -RedirectStandardError "$script:Out\stderr.txt"
  for ($i = 0; $i -lt 50 -and $script:Proc.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 200; $script:Proc.Refresh() }
  Start-Sleep -Milliseconds 2500
  Focus-HbUi
}

function Focus-HbUi { [W]::ShowWindow($script:Proc.MainWindowHandle, 9) | Out-Null; [W]::SetForegroundWindow($script:Proc.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 150 }

function Shot([string]$Name) {
  Focus-HbUi
  $h = $script:Proc.MainWindowHandle
  $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
  $p = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$p) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($p.X, $p.Y, 0, 0, $bmp.Size)
  $file = "$script:Out\$Name.png"
  $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
  return $file
}

$script:VK = @{ UP=0x26; DOWN=0x28; LEFT=0x25; RIGHT=0x27; RETURN=0x0D; ESC=0x1B; X=0x58; Y=0x59; LB=0xDB; RB=0xDD; MINUS=0xBD; PLUS=0xBB }
function Key([string]$K, [int]$Times = 1) {
  Focus-HbUi
  for ($i = 0; $i -lt $Times; $i++) {
    $vk = [byte]$script:VK[$K]
    $scan = [byte][W]::MapVirtualKey($vk, 0)
    # GLFW reads scan codes; arrows are "extended" keys (otherwise they are the numpad ones).
    $ext = if ($K -in 'UP','DOWN','LEFT','RIGHT') { 1 } else { 0 }
    [W]::keybd_event($vk, $scan, $ext, [UIntPtr]::Zero); Start-Sleep -Milliseconds 90
    [W]::keybd_event($vk, $scan, $ext -bor 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 260
  }
}

function Click([int]$X, [int]$Y) {
  Focus-HbUi
  $h = $script:Proc.MainWindowHandle
  $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
  $p = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$p) | Out-Null
  $sx = $p.X + [int]($X * ($r.R - $r.L) / 1280); $sy = $p.Y + [int]($Y * ($r.B - $r.T) / 720)
  [W]::SetCursorPos($sx, $sy) | Out-Null; Start-Sleep -Milliseconds 80
  [W]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [W]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 300
}

function Stop-HbUi { if ($script:Proc -and -not $script:Proc.HasExited) { $script:Proc.Kill() }; Start-Sleep -Milliseconds 300 }
function Alive { $script:Proc.Refresh(); return -not $script:Proc.HasExited }
