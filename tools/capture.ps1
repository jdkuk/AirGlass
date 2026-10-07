# Captures the AirGlass window region (with a small margin) at the given delays (seconds, from now).
param([string]$OutPrefix, [double[]]$At = @(0), [int]$Pad = 24)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class AgWin {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  public struct RECT { public int L, T, R, B; }
}
"@
[AgWin]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$start = Get-Date
$i = 0
foreach ($t in $At) {
  $wait = $t - ((Get-Date) - $start).TotalSeconds
  if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
  $h = [AgWin]::FindWindow("AirGlassWindow", $null)
  if ($h -eq [IntPtr]::Zero -or -not [AgWin]::IsWindowVisible($h)) { "t=$t : window not visible"; $i++; continue }
  $r = New-Object AgWin+RECT
  [AgWin]::GetWindowRect($h, [ref]$r) | Out-Null
  $x = [Math]::Max(0, $r.L - $Pad); $y = [Math]::Max(0, $r.T - $Pad)
  $w = ($r.R - $r.L) + 2 * $Pad; $hh = ($r.B - $r.T) + 2 * $Pad
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size $w, $hh))
  $file = "{0}_{1:00}.png" -f $OutPrefix, $i
  $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  "t=$t : window {0},{1} {2}x{3} -> $file" -f $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T)
  $i++
}
