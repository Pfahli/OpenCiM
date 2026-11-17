# Draws the minimap's satellite-picture button (the OSM import's ground overlay, editor.script extension) in the style
# of the game's minimap buttons: a glossy satellite with a black outline and a faint reflection under it; _se (shown)
# darker with a pale glow, _ho (hover) a little larger. Own drawing (System.Drawing), 64x64 uncompressed 32-bit DDS
# like tools\make-toolbar-icon.ps1 writes; the buttons crop (8, 8, 46, 49) of it.
#
#   tools\make-satellite-icon.ps1 -Out data\regs\ui\minimap\satellite [-PreviewDir <folder>]
param(
  [Parameter(Mandatory)] [string]$Out,
  [string]$PreviewDir = ""
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null

function Write-Dds([byte[]]$px, [string]$dds) {
  $h = New-Object byte[] 128
  $put = { param($at, [uint32]$v) for ($k = 0; $k -lt 4; $k++) { $h[$at + $k] = [byte](($v -shr (8 * $k)) -band 0xFF) } }
  $h[0] = 0x44; $h[1] = 0x44; $h[2] = 0x53; $h[3] = 0x20
  & $put 4 124; & $put 8 0x1007; & $put 12 64; & $put 16 64; & $put 28 1
  & $put 76 32; & $put 80 0x41; & $put 88 32
  & $put 92 0x00FF0000; & $put 96 0x0000FF00; & $put 100 0x000000FF; $h[107] = 0xFF
  & $put 108 0x1000
  [IO.File]::WriteAllBytes($dds, [byte[]]($h + $px))
}

# the satellite at 4x (256 px), centred at (cx, cy) with scale s, brightness k, as paths drawn twice: outline, then fill
function Draw-Satellite($g, [double]$cx, [double]$cy, [double]$s, [double]$k, [int]$alpha) {
  $c = { param([int]$r, [int]$gg, [int]$b) [System.Drawing.Color]::FromArgb($alpha, [int]($r * $k), [int]($gg * $k), [int]($b * $k)) }
  $state = $g.Save()
  $g.TranslateTransform([single]$cx, [single]$cy)
  $g.RotateTransform(-30)
  $g.ScaleTransform([single]$s, [single]$s)
  $outline = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb($alpha, 0, 0, 0)), 9
  $outline.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
  # the panels (left and right), the arms, the body, the dish
  $panels = @((New-Object System.Drawing.RectangleF -78, -18, 52, 36), (New-Object System.Drawing.RectangleF 26, -18, 52, 36))
  $body = New-Object System.Drawing.RectangleF -17, -22, 34, 44
  foreach ($p in $panels) { $g.DrawRectangle($outline, $p.X, $p.Y, $p.Width, $p.Height) }
  $g.DrawLine($outline, -26, 0, 26, 0)
  $g.DrawRectangle($outline, $body.X, $body.Y, $body.Width, $body.Height)
  $g.DrawEllipse($outline, -12, -46, 24, 16)
  $g.DrawLine($outline, 0, -30, 0, -22)
  foreach ($p in $panels) {
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush $p, (& $c 120 200 255), (& $c 20 90 200), 90.0
    $g.FillRectangle($grad, $p)
    $cell = New-Object System.Drawing.Pen (& $c 10 40 110), 2.5
    for ($i = 1; $i -lt 4; $i++) { $g.DrawLine($cell, $p.X + $p.Width * $i / 4, $p.Y, $p.X + $p.Width * $i / 4, $p.Y + $p.Height) }
    $g.DrawLine($cell, $p.X, $p.Y + $p.Height / 2, $p.X + $p.Width, $p.Y + $p.Height / 2)
    $shine = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb([int]($alpha * 0.35), 255, 255, 255))
    $g.FillRectangle($shine, $p.X + 2, $p.Y + 2, $p.Width - 4, $p.Height * 0.3)
  }
  $arm = New-Object System.Drawing.Pen (& $c 150 150 150), 4
  $g.DrawLine($arm, -26, 0, 26, 0)
  $bodyBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $body, (& $c 235 235 235), (& $c 110 110 110), 0.0
  $g.FillRectangle($bodyBrush, $body)
  $gold = New-Object System.Drawing.SolidBrush (& $c 220 170 60)
  $g.FillRectangle($gold, -11, -14, 22, 10)
  $g.FillEllipse((New-Object System.Drawing.SolidBrush (& $c 230 230 230)), -12, -46, 24, 16)
  $g.DrawLine((New-Object System.Drawing.Pen (& $c 200 200 200), 3), 0, -30, 0, -22)
  $g.Restore($state)
}

foreach ($state in @("", "_se", "_ho")) {
  $big = New-Object System.Drawing.Bitmap 256, 256, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($big)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)
  $s = if ($state -eq "_ho") { 1.1 } else { 1.0 }
  $k = if ($state -eq "_se") { 0.55 } else { 1.0 }
  if ($state -eq "_se") {  # the pale glow of a selected game button
    for ($r = 22; $r -ge 2; $r -= 4) {
      $glow = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(14, 255, 255, 255)), ($r * 2)
      $glow.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
      $st = $g.Save(); $g.TranslateTransform(128, 112); $g.RotateTransform(-30)
      $g.DrawRectangle($glow, -78, -18, 156, 36); $g.DrawRectangle($glow, -17, -40, 34, 62)
      $g.Restore($st)
    }
  }
  # the reflection: the satellite mirrored under it, faint and fading
  $refl = New-Object System.Drawing.Bitmap 256, 256, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $rg = [System.Drawing.Graphics]::FromImage($refl)
  $rg.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  Draw-Satellite $rg 128 112 (1.1 * $s) $k 255
  $rg.Dispose()
  $refl.RotateFlip([System.Drawing.RotateFlipType]::RotateNoneFlipY)
  for ($y = 0; $y -lt 256; $y++) {
    for ($x = 0; $x -lt 256; $x++) {
      $p = $refl.GetPixel($x, $y)
      if ($p.A -eq 0) { continue }
      $fade = [Math]::Max(0.0, 0.28 - ($y - 150) / 130.0 * 0.28)
      if ($y -lt 150) { $fade = 0 }
      $refl.SetPixel($x, $y, [System.Drawing.Color]::FromArgb([int]($p.A * $fade), $p.R, $p.G, $p.B))
    }
  }
  $g.DrawImage($refl, 0, 70)
  Draw-Satellite $g 128 112 (1.1 * $s) $k 255
  $g.Dispose()
  $small = New-Object System.Drawing.Bitmap 64, 64, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $sg = [System.Drawing.Graphics]::FromImage($small)
  $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $sg.DrawImage($big, 0, 0, 64, 64)
  $sg.Dispose()
  $data = $small.LockBits((New-Object System.Drawing.Rectangle 0, 0, 64, 64), [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $px = New-Object byte[] (64 * 64 * 4)
  for ($row = 0; $row -lt 64; $row++) {
    [Runtime.InteropServices.Marshal]::Copy([IntPtr]($data.Scan0.ToInt64() + $row * $data.Stride), $px, $row * 256, 256)
  }
  $small.UnlockBits($data)
  Write-Dds $px "$Out$state.dds"
  if ($PreviewDir) { $small.Save((Join-Path $PreviewDir ((Split-Path $Out -Leaf) + "$state.png")), [System.Drawing.Imaging.ImageFormat]::Png) }
  $small.Dispose(); $big.Dispose(); $refl.Dispose()
}
Write-Output "icon: $Out (.dds, _se.dds, _ho.dds)"
