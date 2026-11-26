# Puts an OpenCiM release together (docs/installer.md §6, step 5):
#   .\make-release.ps1 [-Out <folder>] [-NoBuild]
# builds the port (build.ps1), collects the install folder (regs.exe, regs-tool.exe, data\, README, licences) into
# <Out>\OpenCiM-<version>, zips it with its .sha256 (the plain download), and appends the same files plus the setup's
# pictures (src\setup\show) to opencim-setup.exe: <Out>\OpenCiM-<version>-setup.exe, the offline installer.
# Nothing of the game is in any of it. Signing and the GitHub release come later (no certificate, no repository yet).
param([string]$Out = "$PSScriptRoot\build\release", [switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$bin = "$root\build\x64-Release"
if (-not $NoBuild) { & "$root\build.ps1" | Out-Host }
foreach ($f in 'regs.exe', 'regs-tool.exe', 'opencim-setup.exe') {
  if (-not (Test-Path "$bin\$f")) { throw "missing $bin\$f (build first)" }
}
$info = Get-Content "$bin\generated\build_info.h" -Raw
$version = [regex]::Match($info, 'REGS_VERSION_STRING "([^"]+)"').Groups[1].Value
$build = [regex]::Match($info, 'REGS_BUILD_NUMBER (\d+)').Groups[1].Value
$name = "OpenCiM-$version"
$stage = Join-Path $Out $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# the install folder
Copy-Item "$bin\regs.exe", "$bin\regs-tool.exe" $stage
Copy-Item "$bin\data" $stage -Recurse
Copy-Item "$root\README.md" $stage
Copy-Item "$root\..\LICENSE" "$stage\LICENSE.txt"  # OpenCiM itself: GPL-3.0-or-later
Copy-Item "$root\..\LICENSE-EXCEPTION.txt" "$stage\LICENSE-EXCEPTION.txt"  # its section 7 permission (Leia SR runtime)
$lic = New-Item -ItemType Directory -Force "$stage\licences"
Copy-Item "$root\third_party\glslang\LICENSE.txt" "$lic\glslang.txt"
Copy-Item "$root\third_party\vulkan-headers\LICENSE.md" "$lic\vulkan-headers.md"
Copy-Item "$root\third_party\fidelityfx-fsr\LICENSE.txt" "$lic\fidelityfx-fsr.txt"
# miniaudio's licence is the notice at the end of its header (public domain or MIT No Attribution)
$ma = Get-Content "$root\third_party\miniaudio\miniaudio.h" -Raw
$at = $ma.LastIndexOf('This software is available as a choice of the following licenses')
if ($at -ge 0) { [IO.File]::WriteAllText("$lic\miniaudio.txt", $ma.Substring($at)) }
[IO.File]::WriteAllText("$stage\VERSION.txt", "OpenCiM $version (build $build)`r`nNeeds the original Cities in Motion; nothing of the game is in this download.`r`n")

# the plain download: zip + sha256
$zip = Join-Path $Out "$name-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
[IO.File]::WriteAllText("$zip.sha256", "$hash  $(Split-Path $zip -Leaf)`n")

# the symbols (not for players): the exes and PDBs of this build, so that regs-tool symbolize and Visual Studio can
# read crash reports and minidumps sent in later (docs/crash-reports.md)
$sym = Join-Path $Out "$name-symbols.zip"
if (Test-Path $sym) { Remove-Item $sym -Force }
$symFiles = foreach ($f in 'regs', 'regs-tool', 'opencim-setup') {
  foreach ($e in 'exe', 'pdb') { if (Test-Path "$bin\$f.$e") { "$bin\$f.$e" } }
}
Compress-Archive -Path $symFiles -DestinationPath $sym -CompressionLevel Optimal

# the offline installer: the setup program with the install folder and its pictures appended
$setup = Join-Path $Out "$name-setup.exe"
& "$bin\opencim-setup.exe" --make "$bin\opencim-setup.exe" $stage "$root\src\setup\show" $setup | Out-Host
$deadline = (Get-Date).AddSeconds(120)
while (-not (Test-Path $setup) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
if (-not (Test-Path $setup)) { throw "the offline installer was not written" }
Get-Item $zip, "$zip.sha256", $setup, $sym | Select-Object Name, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } }
