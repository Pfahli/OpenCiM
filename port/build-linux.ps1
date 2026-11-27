# Builds the Linux version in Docker (docs/linux.md): the image from linux/Dockerfile (Ubuntu 24.04, GCC, CMake,
# SDL3's build dependencies), then CMake + Ninja into build/linux-<Config>. Needs Docker Desktop running.
# Usage: .\build-linux.ps1 [-Config Release|Debug] [-Clean] [-KeepGoing]
param(
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Config = 'Release',
  [switch]$Clean,
  [switch]$KeepGoing,  # keep compiling after errors (to see them all)
  [switch]$Asan  # AddressSanitizer (build/linux-asan, with debug info; for memory errors)
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$repo = Split-Path $root
$image = 'opencim-linux-build'
# (made again when linux/Dockerfile changed: Docker reuses its cached layers otherwise)
docker build -q -t $image "$root\linux" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'docker build failed' }
$dir = if ($Asan) { 'build/linux-asan' } else { "build/linux-$Config" }
if ($Clean -and (Test-Path "$root\$dir")) { Remove-Item -Recurse -Force "$root\$dir" }
# the build number and commit from this checkout's git (a worktree's .git points to a Windows path the container
# cannot follow)
$count = git -C $repo rev-list --count HEAD
$hash = git -C $repo rev-parse --short=8 HEAD
if (git -C $repo status --porcelain --untracked-files=no) { $hash += '-modified' }
$k = if ($KeepGoing) { '-k 0' } else { '' }
# the controller glyphs from the Windows build (their letters need GDI), when there is one
$pad = if (Test-Path "$root\build\x64-Release\data\regs\ui\pad") { "-DREGS_PAD_GLYPHS_DIR=/src/port/build/x64-Release/data/regs/ui/pad" } else { "" }
$san = if ($Asan) { "-DCMAKE_BUILD_TYPE=RelWithDebInfo '-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer' '-DCMAKE_C_FLAGS=-fsanitize=address -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address -static-libasan'" } else { "-DCMAKE_BUILD_TYPE=$Config" }
$sh = "cmake -S /src/port -B /src/port/$dir -G Ninja $san $pad && cmake --build /src/port/$dir -- $k"
docker run --rm -e REGS_BUILD_NUMBER=$count -e REGS_BUILD_COMMIT=$hash -v "${repo}:/src" $image bash -c $sh
if ($LASTEXITCODE -ne 0) { throw "linux build failed ($LASTEXITCODE)" }
Write-Host "built: $root\$($dir.Replace('/', '\'))"
