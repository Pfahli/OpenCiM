# Run Ghidra headless on GSystem.dll with the CiM pre/post scripts, calling the Java launcher directly
# (avoids analyzeHeadless.bat argument handling with spaces and "(x86)" in paths).
#
#   .\run-ghidra.ps1                 import + analyze + recover + decompile (fresh project)
#   .\run-ghidra.ps1 -Reuse          reuse the saved project (skips the cold import/analysis)
#   .\run-ghidra.ps1 -Reuse -Script DumpFeatures.java -ScriptArgs out.jsonl    run another post-script instead
param(
    [string]$Script = "",
    [string[]]$ScriptArgs = @(),
    [string]$Ghidra = "C:\Users\Marku\Ghidra\ghidra_12.1.4_PUBLIC",
    [string]$JavaHome = "C:\Program Files\Java\jdk-23",
    [string]$Binary = "C:\Program Files (x86)\Steam\steamapps\common\Cities In Motion\GSystem.dll",
    [string]$ProjectDir = "C:\Users\Marku\Ghidra\projects",
    [string]$ProjectName = "CiM",
    [string]$Candidates = "$PSScriptRoot\..\..\decompiled\GSystem-undiscovered-code.txt",
    [string]$OutDir = "$PSScriptRoot\..\..\decompiled\GSystem-v2",
    [int]$Threads = 4,
    [switch]$Reuse
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $ProjectDir, $OutDir | Out-Null
$Candidates = (Resolve-Path $Candidates).Path
$OutDir = (Resolve-Path $OutDir).Path

$vmArgs = @(
    '-Djava.system.class.loader=ghidra.GhidraClassLoader', '-Dfile.encoding=UTF8', '-Duser.country=US',
    '-Duser.language=en', '-Duser.variant=', '-Dsun.java2d.opengl=false', '-Dsun.java2d.d3d=false',
    '-Djdk.tls.client.protocols=TLSv1.2,TLSv1.3', '-Dlog4j.skipJansi=true', '-Xshare:off',
    '--enable-native-access=ALL-UNNAMED', "-Duser.home=$env:USERPROFILE", '-Xmx6G', '-XX:CICompilerCount=2'
)
$target = if ($Reuse) { @('-process', [IO.Path]::GetFileName($Binary), '-noanalysis') } else { @('-import', $Binary, '-overwrite') }
$headless = @($ProjectDir, $ProjectName) + $target + @(
    '-scriptPath', $PSScriptRoot,
    '-preScript', 'CimPreAnalysis.java',
    '-postScript', 'RecoverAndDecompile.java', $Candidates, $OutDir, "$Threads", '60',
    '-log', (Join-Path $OutDir 'ghidra.log'),
    '-scriptlog', (Join-Path $OutDir 'script.log')
)
if ($Reuse) { $headless = $headless | Where-Object { $_ -ne '-preScript' -and $_ -ne 'CimPreAnalysis.java' } }
if ($Script) {
    $headless = @($ProjectDir, $ProjectName) + $target + @('-scriptPath', $PSScriptRoot, '-postScript', $Script) + $ScriptArgs +
        @('-log', (Join-Path $OutDir 'ghidra-script.log'), '-scriptlog', (Join-Path $OutDir 'script-extra.log'))
}

$env:JAVA_HOME = $JavaHome
& "$JavaHome\bin\java.exe" @vmArgs -cp "$Ghidra\Ghidra\Framework\Utility\lib\Utility.jar" ghidra.Ghidra `
    ghidra.app.util.headless.AnalyzeHeadless @headless
exit $LASTEXITCODE
