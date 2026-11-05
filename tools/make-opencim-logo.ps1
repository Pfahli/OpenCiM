# Renders the "OPENCiM" title of the port's main menu (port/data/regs/ui/opencim-title.dds), styled after the game's
# CITIES in MOTION logo: condensed bold capitals with a small lowercase "i" (as the logo's small "in"), a white-to-grey
# fill, a dark outline and a soft light glow. The art is the port's own: the letters come from Barlow Condensed
# (SIL Open Font License), installed on this machine; nothing of the game's logo is used.
#   powershell -ExecutionPolicy Bypass -File tools\make-opencim-logo.ps1 [-Out <path.dds>] [-Png <preview.png>]
param(
  [string]$Out = "$PSScriptRoot\..\port\data\regs\ui\opencim-title.dds",
  [string]$Png = "",
  [string]$Font = "Barlow Condensed ExtraBold"
)
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = "Stop"

$S = 4                    # supersampling
$W = 256; $H = 64         # the texture
$cw = $W * $S; $ch = $H * $S
$big = New-Object System.Drawing.Bitmap $cw, $ch, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($big)
$g.SmoothingMode = 'AntiAlias'
$g.Clear([System.Drawing.Color]::FromArgb(0, 0, 0, 0))

$family = New-Object System.Drawing.FontFamily $Font
$capEm = 46 * $S          # em size of the capitals
$smallEm = 24 * $S        # em size of the small "i"
$fmt = [System.Drawing.StringFormat]::GenericTypographic

# letter by letter, like the logo: narrow capitals, a lowercase "i" (as the logo's small "in"; about two thirds of the
# capital height) whose foot sits just above the base line, and letters that grow taller towards both ends (the logo's slight perspective)
$capEm = 50 * $S
$smallEm = 62 * $S
$letters = @(
  @{ t = "O"; small = $false }, @{ t = "P"; small = $false }, @{ t = "E"; small = $false }, @{ t = "N"; small = $false },
  @{ t = "C"; small = $false }, @{ t = "i"; small = $true }, @{ t = "M"; small = $false }
)
$sx = 0.80          # horizontal squeeze
$spread = 0.16      # extra height at the ends
$track = 1.5 * $S
$probe = New-Object System.Drawing.Drawing2D.GraphicsPath
$probe.AddString("O", $family, 0, [single]$capEm, (New-Object System.Drawing.PointF 0, 0), $fmt)
$ob = $probe.GetBounds(); $probe.Dispose()
$capH = $ob.Height
$parts = @()
foreach ($l in $letters) {
  $p = New-Object System.Drawing.Drawing2D.GraphicsPath
  $p.AddString($l.t, $family, 0, [single]($(if ($l.small) { $smallEm } else { $capEm })), (New-Object System.Drawing.PointF 0, 0), $fmt)
  $b = $p.GetBounds()
  $parts += @{ path = $p; b = $b; small = $l.small; w = $b.Width * $sx }
}
$total = ($parts | ForEach-Object { $_.w } | Measure-Object -Sum).Sum + $track * ($parts.Count - 1)
$x = ($cw - $total) / 2
$mid = $ch / 2
$path = New-Object System.Drawing.Drawing2D.GraphicsPath
foreach ($pt in $parts) {
  $cx = $x + $pt.w / 2
  $k = 1 + $spread * [math]::Abs($cx - $cw / 2) / ($total / 2)
  $m = New-Object System.Drawing.Drawing2D.Matrix
  if ($pt.small) {
    # foot at 3/4 of the (scaled) capital height measured from the top
    $top = $mid - $capH * $k / 2
    $foot = $top + $capH * $k * 0.9
    $m.Translate([single]$x, [single]($foot - $pt.b.Bottom * $sx))
    $m.Scale([single]$sx, [single]$sx)
    $m.Translate([single](-$pt.b.Left), 0)
  } else {
    $m.Translate([single]$x, [single]$mid)
    $m.Scale([single]$sx, [single]$k)
    $m.Translate([single](-$pt.b.Left), [single](-($ob.Top + $capH / 2)))
  }
  $pt.path.Transform($m)
  $path.AddPath($pt.path, $false)
  $pt.path.Dispose()
  $x += $pt.w + $track
}

# glow: wide, faint, cool light strokes behind
foreach ($gw in @(14, 10, 7)) {
  $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(24, 190, 215, 255)), ([single]($gw * $S))
  $pen.LineJoin = 'Round'
  $g.DrawPath($pen, $path); $pen.Dispose()
}
# dark outline
$pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 24, 22, 22)), ([single](3.2 * $S))
$pen.LineJoin = 'Round'
$g.DrawPath($pen, $path); $pen.Dispose()
# fill: white at the top to a cool light grey at the bottom
$bb = $path.GetBounds()
$brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, $bb.Top), (New-Object System.Drawing.PointF 0, $bb.Bottom), ([System.Drawing.Color]::FromArgb(255, 255, 255, 255)), ([System.Drawing.Color]::FromArgb(255, 196, 204, 214))
$g.FillPath($brush, $path)
$brush.Dispose(); $g.Dispose()

# downsample
$small = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g2 = [System.Drawing.Graphics]::FromImage($small)
$g2.InterpolationMode = 'HighQualityBicubic'
$g2.PixelOffsetMode = 'HighQuality'
$g2.CompositingMode = 'SourceCopy'
$g2.DrawImage($big, (New-Object System.Drawing.Rectangle 0, 0, $W, $H))
$g2.Dispose(); $big.Dispose()
if ($Png) { $small.Save($Png, [System.Drawing.Imaging.ImageFormat]::Png) }

# DDS, uncompressed A8R8G8B8 (BGRA in memory), one level, rows top to bottom
$rect = New-Object System.Drawing.Rectangle 0, 0, $W, $H
$bd = $small.LockBits($rect, 'ReadOnly', ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb))
$pix = New-Object byte[] ($W * $H * 4)
for ($row = 0; $row -lt $H; $row++) {
  [System.Runtime.InteropServices.Marshal]::Copy([IntPtr]($bd.Scan0.ToInt64() + $row * $bd.Stride), $pix, $row * $W * 4, $W * 4)
}
$small.UnlockBits($bd); $small.Dispose()
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $ms
$bw.Write([byte[]][char[]]"DDS ")
$bw.Write([uint32]124); $bw.Write([uint32]0x100F)       # size; CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT
$bw.Write([uint32]$H); $bw.Write([uint32]$W); $bw.Write([uint32]($W * 4)); $bw.Write([uint32]0); $bw.Write([uint32]1)
for ($k = 0; $k -lt 11; $k++) { $bw.Write([uint32]0) }
$bw.Write([uint32]32); $bw.Write([uint32]0x41); $bw.Write([uint32]0); $bw.Write([uint32]32)   # RGB|ALPHAPIXELS
$bw.Write([uint32]0x00FF0000); $bw.Write([uint32]0x0000FF00); $bw.Write([uint32]0x000000FF); $bw.Write([uint32]4278190080)  # 0xFF000000
$bw.Write([uint32]0x1000); $bw.Write([uint32]0); $bw.Write([uint32]0); $bw.Write([uint32]0); $bw.Write([uint32]0)
$bw.Write($pix)
$bw.Flush()
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[System.IO.File]::WriteAllBytes([System.IO.Path]::GetFullPath($Out), $ms.ToArray())
"wrote $Out ($W x $H)"
