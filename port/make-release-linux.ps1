# Puts the Linux release together (docs/linux.md): builds with build-linux.ps1 (Docker) and packs
# <Out>\OpenCiM-<version>-linux-x64.tar.gz (+ .sha256) and the debug symbols with linux/package.sh in the container.
#   .\make-release-linux.ps1 [-Out <folder>] [-NoBuild]
# The controller glyphs come from the Windows build (build\x64-Release\data\regs\ui\pad): build that first.
param([string]$Out = "$PSScriptRoot\build\release", [switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$repo = Split-Path $root
if (-not (Test-Path "$root\build\x64-Release\data\regs\ui\pad")) { throw 'no controller glyphs: build the Windows version first (build.ps1)' }
if (-not $NoBuild) { & "$root\build-linux.ps1" | Out-Host }
$bin = "$root\build\linux-Release"
foreach ($f in 'regs', 'regs-tool') { if (-not (Test-Path "$bin\$f")) { throw "missing $bin\$f (build first)" } }
$info = Get-Content "$bin\generated\build_info.h" -Raw
$version = [regex]::Match($info, 'REGS_VERSION_STRING "([^"]+)"').Groups[1].Value
$build = [regex]::Match($info, 'REGS_BUILD_NUMBER (\d+)').Groups[1].Value
New-Item -ItemType Directory -Force $Out | Out-Null
$outAbs = (Resolve-Path $Out).Path
if (-not $outAbs.StartsWith("$repo\", [StringComparison]::OrdinalIgnoreCase)) { throw "-Out must be inside the repository ($repo)" }
$outRel = $outAbs.Substring($repo.Length + 1).Replace('\', '/')
docker run --rm -v "${repo}:/src" opencim-linux-build bash /src/port/linux/package.sh /src/port/build/linux-Release "/src/$outRel" $version $build
if ($LASTEXITCODE -ne 0) { throw 'packing failed' }
Get-Item "$Out\OpenCiM-$version-linux-x64*" | Select-Object Name, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } }
