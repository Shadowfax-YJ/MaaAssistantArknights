param([string]$Python = 'python', [string]$SourceRef, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo 'build\blackflow-interaction-replay' }
$out = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $out -Force | Out-Null
if (!(Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'MSVC developer tools are required.' }
    & (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
}
$generatorArgs = @((Join-Path $PSScriptRoot 'generate_blackflow_interaction_replay.py'), (Join-Path $out 'replay.cpp'))
if ($SourceRef) { $generatorArgs += @('--source-ref', $SourceRef) }
& $Python @generatorArgs
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$dep = Join-Path $repo 'src\MaaUtils\MaaDeps\vcpkg\installed\maa-x64-windows'
$includes = @('src\MaaCore', 'src\MaaUtils\include', '3rdparty\include') | ForEach-Object { '/I' + (Join-Path $repo $_) }
Push-Location $out
try {
    & cl.exe /nologo /std:c++20 /EHsc /MD /O2 /utf-8 @includes ('/I'+$dep+'\include\opencv4') ('/I'+$dep+'\include') ('/Fe'+$out+'\replay.exe') ($out+'\replay.cpp') (Join-Path $repo 'src\MaaCore\Task\Roguelike\BlackFlow\BlackFlowModel.cpp') /link ($dep+'\lib\opencv_world4.lib')
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    $env:PATH = $dep + '\bin;' + $env:PATH
    & ($out+'\replay.exe') (Join-Path $PSScriptRoot 'fixtures\blackflow-interaction') (Join-Path $out 'reward-click-events.json')
    exit $LASTEXITCODE
}
finally { Pop-Location }
