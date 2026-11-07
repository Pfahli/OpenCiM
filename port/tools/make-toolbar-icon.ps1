# Makes a toolbar icon set (<name>.dds, <name>_se.dds, <name>_ho.dds; 64x64, uncompressed 32-bit) for the port's own
# buttons from a picture of an isometric tile (the curve and roundabout icons were generated with Magnific from the
# game's road icons as a style reference). The tile is fitted into the game's own diamond (44x36 at 10,16; hover 49x41
# at 8,14) inside the outline of a game icon of the same kind, so size and border match the buttons beside it; the
# selected state is the normal one at 55 % brightness, as the game's are.
#
#   tools\make-toolbar-icon.ps1 -Source tile.png -Box 1823:1223:112:413 -Outline smallroad-road0 -Out data\regs\ui\toolbar\curve-small
#
# -Box: the tile's crop (w:h:x:y) in the source; -Outline: the game icon (metro/ui/toolbar/<name>) whose border is kept.
param(
  [Parameter(Mandatory)] [string]$Source,
  [Parameter(Mandatory)] [string]$Box,
  [string]$Outline = "smallroad-road0",
  [Parameter(Mandatory)] [string]$Out,
  [string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Cities In Motion",
  [string]$Ffmpeg = "C:\Program Files\DownloadHelper CoApp\ffmpeg.exe",
  [string]$PreviewDir = ""
)
$ErrorActionPreference = "Stop"
$tool = Join-Path $PSScriptRoot "..\build\x64-Release\regs-tool.exe"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("regs-icon-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Force $tmp | Out-Null
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null

function Write-Dds([string]$raw, [string]$dds) {
  $px = [IO.File]::ReadAllBytes($raw)
  $h = New-Object byte[] 128
  $put = { param($at, [uint32]$v) for ($k = 0; $k -lt 4; $k++) { $h[$at + $k] = [byte](($v -shr (8 * $k)) -band 0xFF) } }
  $h[0] = 0x44; $h[1] = 0x44; $h[2] = 0x53; $h[3] = 0x20
  & $put 4 124; & $put 8 0x1007; & $put 12 64; & $put 16 64; & $put 28 1
  & $put 76 32; & $put 80 0x41; & $put 88 32
  & $put 92 0x00FF0000; & $put 96 0x0000FF00; & $put 100 0x000000FF; $h[107] = 0xFF  # alpha mask 0xFF000000
  & $put 108 0x1000
  [IO.File]::WriteAllBytes($dds, [byte[]]($h + $px))
}

foreach ($state in @("", "_se", "_ho")) {
  $base = "$Outline$state"
  & $tool vfs $Game cat "metro/ui/toolbar/$base.dds" "$tmp\$base.dds" | Out-Null
  & $Ffmpeg -loglevel error -y -i "$tmp\$base.dds" "$tmp\$base.png"
  if ($state -eq "_ho") { $size = "49:41"; $pad = "8:14" } else { $size = "44:36"; $pad = "10:16" }
  $dim = if ($state -eq "_se") { ",colorchannelmixer=rr=0.55:gg=0.55:bb=0.55" } else { "" }
  # the tile (its white background keyed out) over the game icon, inside the icon's diamond less its 2-pixel border
  $f = "[0]crop=$Box,colorkey=white:0.12:0.05,scale=${size}:flags=area,format=rgba$dim,pad=64:64:${pad}:color=black@0,split[g1][g2];" +
       "[1]split[b1][b2];[b1]alphaextract,erosion,erosion[m];[g1]alphaextract[ga];[ga][m]blend=all_mode=multiply[am];" +
       "[g2][am]alphamerge[gm];[b2][gm]overlay=format=auto"
  & $Ffmpeg -loglevel error -y -i $Source -i "$tmp\$base.png" -filter_complex $f -f rawvideo -pix_fmt bgra "$tmp\icon.raw"
  Write-Dds "$tmp\icon.raw" "$Out$state.dds"
  if ($PreviewDir) {
    & $Ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s 64x64 -i "$tmp\icon.raw" (Join-Path $PreviewDir ((Split-Path $Out -Leaf) + "$state.png"))
  }
}
Remove-Item -Recurse -Force $tmp
Write-Output "icon: $Out (.dds, _se.dds, _ho.dds)"
