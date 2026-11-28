# Builds the Android version in Docker (docs/android.md): the image from android/Dockerfile (Android SDK and NDK, curl on
# mbedTLS), libmain.so for each ABI (android/build.sh) and the APK (android/package.sh) into build/android-release.
# The port's data comes from the Linux build (build/linux-Release/data, with its generated sounds and the controller
# glyphs): run .\build.ps1 and .\build-linux.ps1 before. Needs Docker Desktop running.
# Usage: .\build-android.ps1 [-Abi arm64-v8a,x86_64] [-KeepGoing] [-Keystore <file> -KeyAlias <alias>]
#   without a keystore the APK is signed with a debug key (tests); KEY_PASS holds the release keystore's password
param(
  [string[]]$Abi = @('arm64-v8a', 'x86_64'),
  [switch]$KeepGoing,
  [string]$Keystore = '',
  [string]$KeyAlias = ''
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$repo = Split-Path $root
$image = 'opencim-android-build'
docker build -q -t $image "$root\android" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'docker build failed' }
if (-not (Test-Path "$root\build\linux-Release\data\regs")) { throw 'no build\linux-Release\data: run .\build-linux.ps1 first' }
# the build number (the APK's versionCode) and commit from this checkout's git (a worktree's .git points to a Windows
# path the container cannot follow)
$count = git -C $repo rev-list --count HEAD
$hash = git -C $repo rev-parse --short=8 HEAD
if (git -C $repo status --porcelain --untracked-files=no) { $hash += '-modified' }
$ks = ''
$mounts = @('-v', "${repo}:/src")
if ($Keystore) {
  $ksDir = Split-Path (Resolve-Path $Keystore)
  $mounts += @('-v', "${ksDir}:/keys")
  $ks = "/keys/$(Split-Path -Leaf $Keystore) $KeyAlias"
}
$sh = "bash /src/port/android/build.sh $($Abi -join ' ') && bash /src/port/android/package.sh /src/port/build/linux-Release/data /src/port/build/android-release $ks"
$envs = @('-e', "REGS_BUILD_NUMBER=$count", '-e', "REGS_BUILD_COMMIT=$hash")
if ($env:KEY_PASS) { $envs += @('-e', 'KEY_PASS') }
if ($KeepGoing) { $envs += @('-e', 'KEEP_GOING=1') }
docker run --rm @envs @mounts $image bash -c $sh
if ($LASTEXITCODE -ne 0) { throw "android build failed ($LASTEXITCODE)" }
Write-Host "built: $root\build\android-release"
