# Configure and build with the Visual Studio 2022 Build Tools (MSVC + the bundled CMake/Ninja).
# Usage: .\build.ps1 [-Config Release|Debug] [-Arch x64|x86] [-Name suffix] [-Clean]
# -Name builds into build\<arch>-<config>-<suffix>, a separate tree for parallel work on the same sources.
param(
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Config = 'Release',
  [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
  [string]$Name = '',
  [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root ("build\$Arch-$Config" + $(if ($Name) { "-$Name" } else { '' }))
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
$installer = Split-Path $vswhere
$cmd = "set `"PATH=$installer;%PATH%`" && call `"$vcvars`" $Arch >nul &&cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config && cmake --build `"$build`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "built: $build"
