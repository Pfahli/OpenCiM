# Makes the autosave indicator (data\regs\ui\save-icon.dds; 32x32, uncompressed 32-bit): a floppy disk drawn here (no
# game art), light grey with a dark outline and a soft shadow so it reads on the sky, the map and the menus alike.
# Shown blinking in the top right corner while an autosave runs (regs/extend/metro/ui/ui.script, regsShowSaveIcon).
#
#   tools\make-save-icon.ps1 [-Out data\regs\ui\save-icon.dds] [-Preview icon.png]
param(
  [string]$Out = (Join-Path $PSScriptRoot "..\data\regs\ui\save-icon.dds"),
  [string]$Preview = ""
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$n = 32
$bmp = New-Object System.Drawing.Bitmap $n, $n, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.Clear([System.Drawing.Color]::FromArgb(0, 0, 0, 0))

function Body([float]$dx, [float]$dy) {
  # the disk: a square with the top right corner cut off
  $p = New-Object System.Drawing.Drawing2D.GraphicsPath
  $pts = @(
    (New-Object System.Drawing.PointF (4 + $dx), (4 + $dy)),
    (New-Object System.Drawing.PointF (23 + $dx), (4 + $dy)),
    (New-Object System.Drawing.PointF (28 + $dx), (9 + $dy)),
    (New-Object System.Drawing.PointF (28 + $dx), (28 + $dy)),
    (New-Object System.Drawing.PointF (4 + $dx), (28 + $dy)))
  $p.AddPolygon([System.Drawing.PointF[]]$pts)
  return $p
}

# shadow, body, outline
$shadow = Body 1.5 1.5
$g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(110, 0, 0, 0))), $shadow)
$body = Body 0 0
$g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 58, 96, 150))), $body)
$g.DrawPath((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 20, 30, 48)), 1.4), $body)
# metal shutter at the top with its slot
$g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 205, 210, 218))), 9, 5, 12, 8)
$g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 58, 96, 150))), 16, 6.5, 3, 5)
# label at the bottom
$g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 245, 245, 240))), 8, 17, 16, 10)
$linePen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 150, 160, 175)), 1
$g.DrawLine($linePen, 10, 20.5, 22, 20.5)
$g.DrawLine($linePen, 10, 23.5, 22, 23.5)
$g.Dispose()

if ($Preview) { $bmp.Save($Preview, [System.Drawing.Imaging.ImageFormat]::Png) }

# BGRA rows top to bottom (LockBits on a 32bppArgb bitmap gives exactly that on little-endian)
$rect = New-Object System.Drawing.Rectangle 0, 0, $n, $n
$data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $bmp.PixelFormat)
$px = New-Object byte[] ($n * $n * 4)
for ($y = 0; $y -lt $n; $y++) {
  [System.Runtime.InteropServices.Marshal]::Copy([IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $px, $y * $n * 4, $n * 4)
}
$bmp.UnlockBits($data)
$bmp.Dispose()

$h = New-Object byte[] 128
$put = { param($at, [uint32]$v) for ($k = 0; $k -lt 4; $k++) { $h[$at + $k] = [byte](($v -shr (8 * $k)) -band 0xFF) } }
$h[0] = 0x44; $h[1] = 0x44; $h[2] = 0x53; $h[3] = 0x20
& $put 4 124; & $put 8 0x1007; & $put 12 $n; & $put 16 $n; & $put 28 1
& $put 76 32; & $put 80 0x41; & $put 88 32
& $put 92 0x00FF0000; & $put 96 0x0000FF00; & $put 100 0x000000FF; $h[107] = 0xFF  # alpha mask 0xFF000000
& $put 108 0x1000
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[IO.File]::WriteAllBytes($Out, [byte[]]($h + $px))
Write-Output "icon: $Out"
