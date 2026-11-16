# Makes the editor's brush shape icons (the buttons beside the brush sizes): brush-circle, brush-square, brush-diamond,
# brush-line, each as <name>.dds, <name>_se.dds (selected: on a grey tile, as the selected size button) and
# <name>_ho.dds (hover: on a faint tile); 64x64, uncompressed 32-bit, drawn here (no source picture).
#
#   tools\make-brush-icons.ps1 [-Out data\regs\ui\toolbar]
param([string]$Out = (Join-Path $PSScriptRoot "..\data\regs\ui\toolbar"))
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $Out | Out-Null

function Write-Dds([System.Drawing.Bitmap]$bmp, [string]$dds) {
  $rect = New-Object System.Drawing.Rectangle 0, 0, 64, 64
  $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $px = New-Object byte[] (64 * 64 * 4)
  for ($y = 0; $y -lt 64; $y++) {  # BGRA rows, top down
    [Runtime.InteropServices.Marshal]::Copy([IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $px, $y * 256, 256)
  }
  $bmp.UnlockBits($data)
  $h = New-Object byte[] 128
  $put = { param($at, [uint32]$v) for ($k = 0; $k -lt 4; $k++) { $h[$at + $k] = [byte](($v -shr (8 * $k)) -band 0xFF) } }
  $h[0] = 0x44; $h[1] = 0x44; $h[2] = 0x53; $h[3] = 0x20
  & $put 4 124; & $put 8 0x1007; & $put 12 64; & $put 16 64; & $put 28 1
  & $put 76 32; & $put 80 0x41; & $put 88 32
  & $put 92 0x00FF0000; & $put 96 0x0000FF00; & $put 100 0x000000FF; $h[107] = 0xFF
  & $put 108 0x1000
  [IO.File]::WriteAllBytes($dds, [byte[]]($h + $px))
}

function Tile([System.Drawing.Graphics]$g, [int]$alpha, [int]$grey) {
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $r = 10; $x = 4; $y = 4; $w = 56; $hh = 56
  $path.AddArc($x, $y, $r, $r, 180, 90); $path.AddArc($x + $w - $r, $y, $r, $r, 270, 90)
  $path.AddArc($x + $w - $r, $y + $hh - $r, $r, $r, 0, 90); $path.AddArc($x, $y + $hh - $r, $r, $r, 90, 90)
  $path.CloseFigure()
  $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($alpha, $grey, $grey, $grey))), $path)
}

function Symbol([System.Drawing.Graphics]$g, [string]$shape, [System.Drawing.Color]$fill) {
  $shadow = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(150, 0, 0, 0))
  $brush = New-Object System.Drawing.SolidBrush $fill
  foreach ($pass in 0, 1) {
    $b = if ($pass -eq 0) { $shadow } else { $brush }
    $o = if ($pass -eq 0) { 1.5 } else { 0 }  # a soft drop shadow first, so the symbol reads on any background
    switch ($shape) {
      "circle" { $g.FillEllipse($b, 16 + $o, 16 + $o, 32, 32) }
      "square" { $g.FillRectangle($b, 18 + $o, 18 + $o, 28, 28) }
      "diamond" {
        $pts = [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF (32 + $o), (13 + $o)), (New-Object System.Drawing.PointF (51 + $o), (32 + $o)),
          (New-Object System.Drawing.PointF (32 + $o), (51 + $o)), (New-Object System.Drawing.PointF (13 + $o), (32 + $o)))
        $g.FillPolygon($b, $pts)
      }
      "line" {
        $pen = New-Object System.Drawing.Pen $b, 9
        $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Flat; $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Flat
        $g.DrawLine($pen, 18 + $o, 46 + $o, 46 + $o, 18 + $o)
        $g.FillEllipse($b, 13 + $o, 41 + $o, 10, 10)  # the press point
        $g.FillEllipse($b, 41 + $o, 13 + $o, 10, 10)  # the release point
      }
    }
  }
}

foreach ($shape in "circle", "square", "diamond", "line") {
  foreach ($state in "", "_se", "_ho") {
    $bmp = New-Object System.Drawing.Bitmap 64, 64, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::FromArgb(0, 0, 0, 0))
    if ($state -eq "_se") { Tile $g 255 128 } elseif ($state -eq "_ho") { Tile $g 70 200 }
    $c = if ($state -eq "") { [System.Drawing.Color]::FromArgb(255, 214, 214, 214) } else { [System.Drawing.Color]::White }
    Symbol $g $shape $c
    $g.Dispose()
    Write-Dds $bmp (Join-Path $Out "brush-$shape$state.dds")
    $bmp.Dispose()
  }
}
Write-Output "brush icons: $Out (brush-circle, -square, -diamond, -line)"
