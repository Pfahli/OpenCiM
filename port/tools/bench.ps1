# Frame-time benchmark of the graphics engines (docs/renderer/vulkan.md §10): loads a save in a sandbox document folder
# and measures the game's frames with REGS_BENCH for each engine and option set, one window after another.
#   .\tools\bench.ps1 -Docs run\documents-bench [-Save vienna] [-Sizes 1600x900,2560x1440] [-Repeats 2]
# -Docs must be a sandbox copy (its OpenCiM\settings.script is edited: vsync off, the window size); never point it at
# the real Documents folders. Results: a Markdown table on the console and in <Docs>\bench-<date>.md.
param(
  [Parameter(Mandatory = $true)][string]$Docs,
  [string]$Save = 'vienna',
  [string[]]$Sizes = @('1600x900'),
  [int]$Repeats = 1,
  [int]$Warmup = 1800,       # frames before measuring (loading, shader compiles)
  [int]$Count = 1500,        # frames measured
  [string]$Exe = '.\build\x64-Release\regs.exe',
  [string]$Game = 'C:\Program Files (x86)\Steam\steamapps\common\Cities In Motion'
)
$ErrorActionPreference = 'Stop'
[System.Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture  # 1.5, not 1,5
$port = Split-Path $PSScriptRoot
Set-Location $port
$docsPath = (Resolve-Path $Docs).Path
if ($docsPath -match '\\Documents\\(Cities In Motion|OpenCiM)') { throw "not a sandbox: $docsPath" }
$settings = Join-Path $docsPath 'OpenCiM\settings.script'
if (-not (Test-Path $settings)) { throw "no settings.script in $docsPath\OpenCiM" }

# engine and options per configuration (environment switches of the engine, docs/renderer/vulkan.md)
$configs = @(
  @{ Name = 'OpenGL';                         Env = @{ REGS_RENDERER = 'gl' } },
  @{ Name = 'Vulkan';                         Env = @{ REGS_RENDERER = 'vulkan' } },
  @{ Name = 'OpenGL + TAA';                   Env = @{ REGS_RENDERER = 'gl'; REGS_TAA = '1' } },
  @{ Name = 'Vulkan + TAA';                   Env = @{ REGS_RENDERER = 'vulkan'; REGS_TAA = '1' } },
  @{ Name = 'OpenGL + TAA, 67 %';             Env = @{ REGS_RENDERER = 'gl'; REGS_TAA = '1'; REGS_RENDER_SCALE = '67' } },
  @{ Name = 'Vulkan + TAA, 67 %';             Env = @{ REGS_RENDERER = 'vulkan'; REGS_TAA = '1'; REGS_RENDER_SCALE = '67' } },
  @{ Name = 'Vulkan + RT shadows';            Env = @{ REGS_RENDERER = 'vulkan'; REGS_RT_SHADOWS = '1' } },
  @{ Name = 'Vulkan + RT shadows + RT AO + TAA'; Env = @{ REGS_RENDERER = 'vulkan'; REGS_RT_SHADOWS = '1'; REGS_RT_AO = '1'; REGS_TAA = '1' } }
)
$switches = 'REGS_RENDERER', 'REGS_TAA', 'REGS_RENDER_SCALE', 'REGS_RT_SHADOWS', 'REGS_RT_AO', 'REGS_START', 'REGS_BENCH'

$rows = @()
foreach ($size in $Sizes) {
  $w, $h = $size -split 'x'
  $text = Get-Content $settings
  $text = $text -replace '^\$vsync = .*', '$vsync = false;' -replace '^\$fpsLimit = .*', '$fpsLimit = 0.0;'
  $text = $text -replace '^\$screenwidth = .*', "`$screenwidth = $w;" -replace '^\$screenheight = .*', "`$screenheight = $h;"
  Set-Content -Encoding utf8 $settings $text
  foreach ($c in $configs) {
    for ($r = 1; $r -le $Repeats; ++$r) {
      while (Get-Process regs -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 5 }  # one game window at a time
      foreach ($k in $switches) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
      foreach ($k in $c.Env.Keys) { Set-Item "env:$k" $c.Env[$k] }
      $env:REGS_START = "save:$Save"
      $env:REGS_BENCH = "${Warmup}:$Count"
      $log = Join-Path $env:TEMP ("regs-bench-" + ($c.Name -replace '[^\w]+', '-') + "-$size-$r.log")
      $ErrorActionPreference = 'Continue'  # the game logs to stderr; that is not a failure
      & $Exe $Game --windowed --documents $docsPath --frames ($Warmup + $Count + 30) *> $log
      $ErrorActionPreference = 'Stop'
      $line = (Get-Content $log) -join ''  # PowerShell wraps native output at the console width, even mid-word
      if ($line -match 'bench: (\w+), (\d+)x(\d+), \d+ frames: ([\d.]+) fps average, frame ms median ([\d.]+), p95 ([\d.]+), p99 ([\d.]+), max ([\d.]+); render cpu ms ([\d.]+); gpu ms median ([-\d.]+), p95 ([-\d.]+)') {
        $row = [pscustomobject]@{ Config = $c.Name; Size = "$($Matches[2])x$($Matches[3])"; FPS = [double]$Matches[4];
          Median = [double]$Matches[5]; P95 = [double]$Matches[6]; P99 = [double]$Matches[7]; CpuMs = [double]$Matches[9];
          GpuMs = [double]$Matches[10]; GpuP95 = [double]$Matches[11] }
        $rows += $row
        Write-Host ("{0,-36} {1,-10} {2,7:F1} fps  median {3,6:F2} ms  p99 {4,6:F2} ms  render cpu {5,5:F2} ms  gpu {6,5:F2} ms" -f $row.Config, $row.Size, $row.FPS, $row.Median, $row.P99, $row.CpuMs, $row.GpuMs)
      } else {
        Write-Host "$($c.Name) $size run ${r}: no bench line (see $log)"
      }
    }
  }
}
foreach ($k in $switches) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }

# averages over the repeats, as a Markdown table
$md = @("| Configuration | Window | FPS | Median frame ms | p95 ms | p99 ms | Render CPU ms | GPU ms (median) | GPU ms p95 |", "|---|---|---|---|---|---|---|---|---|")
foreach ($g in ($rows | Group-Object Config, Size)) {
  $a = $g.Group
  $md += "| {0} | {1} | {2:F1} | {3:F2} | {4:F2} | {5:F2} | {6:F2} | {7:F2} | {8:F2} |" -f $a[0].Config, $a[0].Size,
    ($a | Measure-Object FPS -Average).Average, ($a | Measure-Object Median -Average).Average,
    ($a | Measure-Object P95 -Average).Average, ($a | Measure-Object P99 -Average).Average, ($a | Measure-Object CpuMs -Average).Average,
    ($a | Measure-Object GpuMs -Average).Average, ($a | Measure-Object GpuP95 -Average).Average
}
$out = Join-Path $docsPath ("bench-" + (Get-Date -Format 'yyyy-MM-dd-HHmm') + ".md")
Set-Content -Encoding utf8 $out $md
$md | ForEach-Object { Write-Host $_ }
Write-Host "results: $out"
