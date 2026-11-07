param([Parameter(Mandatory=$true)][string]$CoreBuildDirectory, [Parameter(Mandatory=$true)][string]$OutputDirectory, [string]$ResourceRoot)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$core = (Resolve-Path $CoreBuildDirectory).Path
$out = [IO.Path]::GetFullPath($OutputDirectory)
if (!$ResourceRoot) { $ResourceRoot = $repo }
New-Item -ItemType Directory -Path $out -Force | Out-Null
if (!(Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'MSVC developer tools are required.' }
    & (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
}
$dep = Join-Path $repo 'src\MaaUtils\MaaDeps\vcpkg\installed\maa-x64-windows'
$includes = @('src\MaaCore','src\MaaUtils\include','include','3rdparty\include','3rdparty\EmulatorExtras') | ForEach-Object { '/I'+(Join-Path $repo $_) }
& cl.exe /nologo /c /std:c++20 /EHsc /MD /O2 /utf-8 /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /D_WIN32_WINNT=0x0A00 /DASST_DLL_EXPORTS /DASST_WITH_EMULATOR_EXTRAS=1 /DASST_WITH_MAC_SCK=0 @includes ('/I'+$dep+'\include') ('/I'+$dep+'\include\opencv4') ('/Fo'+$out+'\replay.obj') (Join-Path $PSScriptRoot 'BlackFlowOpeningRecoveryReplay.cpp')
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$objs = Get-ChildItem -LiteralPath (Join-Path $core 'src\MaaCore\MaaCore.dir\Release') -Filter '*.obj' | ForEach-Object { '"'+$_.FullName+'"' }
$projectPath = Join-Path $core 'src\MaaCore\MaaCore.vcxproj'
[xml]$projectXml = Get-Content -LiteralPath $projectPath -Raw
$linkGroups = @($projectXml.Project.ItemDefinitionGroup | Where-Object { $_.Condition -like '*Release|x64*' })
if ($linkGroups.Count -ne 1 -or !$linkGroups[0].Link.AdditionalDependencies) { throw 'Missing production link dependencies.' }
$libs = foreach ($library in ([string]$linkGroups[0].Link.AdditionalDependencies).Split(';')) {
    if (!$library -or $library -eq '%(AdditionalDependencies)') { continue }
    if ($library.Contains('$(') -or $library.Contains('%(')) { throw "Unresolved dependency: $library" }
    $resolvedLibrary = $library
    if ($library.Contains('\') -or $library.Contains('/')) {
        if (![IO.Path]::IsPathRooted($library)) { $resolvedLibrary = Join-Path (Split-Path $projectPath -Parent) $library }
        $resolvedLibrary = (Resolve-Path -LiteralPath $resolvedLibrary).Path
    }
    '"'+$resolvedLibrary+'"'
}
$argsFile = @('/NOLOGO','/INCREMENTAL:NO','/SUBSYSTEM:CONSOLE',('/OUT:"'+$out+'\replay.exe"'),('"'+$out+'\replay.obj"')) + $objs + $libs
$argsFile | Set-Content -LiteralPath (Join-Path $out 'replay.rsp') -Encoding utf8
& link.exe ('@'+$out+'\replay.rsp')
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$env:PATH = $core+'\bin\Release;'+$dep+'\bin;'+$env:PATH
Push-Location $out
try {
    & ($out+'\replay.exe') $ResourceRoot (Join-Path $PSScriptRoot 'fixtures\blackflow-opening-recovery') > replay-output.txt
    $result = $LASTEXITCODE
    Get-Content replay-output.txt | Where-Object { $_ -match '^(PASS|FAIL|[0-9]+ passed)' }
    exit $result
} finally { Pop-Location }
