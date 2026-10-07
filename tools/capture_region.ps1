# Captures a fixed screen region at the given delays (seconds from now).
param([string]$OutPrefix, [double[]]$At = @(0), [int]$X = 1150, [int]$Y = 300, [int]$W = 1550, [int]$H = 1550)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class AgDpi { [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v); }
"@
[AgDpi]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$start = Get-Date
$i = 0
foreach ($t in $At) {
  $wait = $t - ((Get-Date) - $start).TotalSeconds
  if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
  $bmp = New-Object System.Drawing.Bitmap $W, $H
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H))
  $file = "{0}_{1:00}.png" -f $OutPrefix, $i
  $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  "t=$t -> $file"
  $i++
}
