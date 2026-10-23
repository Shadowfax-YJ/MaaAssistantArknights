param([string]$Python = 'python', [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo 'build/blackflow-expedition-evidence-replay' }
$out = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $out -Force | Out-Null
if (!(Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    & (Join-Path $vs 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
}
& $Python (Join-Path $PSScriptRoot 'generate_blackflow_expedition_evidence_replay.py') (Join-Path $out 'replay.cpp')
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$dep = Join-Path $repo 'src/MaaUtils/MaaDeps/vcpkg/installed/maa-x64-windows'
$includes = @('src/MaaCore', 'src/MaaUtils/include', '3rdparty/include') | ForEach-Object { '/I' + (Join-Path $repo $_) }
& cl.exe /nologo /std:c++20 /EHsc /MD /O2 /utf-8 @includes ('/I'+$dep+'/include/opencv4') ('/I'+$dep+'/include') ('/Fo'+$out+'/replay.obj') ('/Fe'+$out+'/replay.exe') ($out+'/replay.cpp') /link ($dep+'/lib/opencv_world4.lib')
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$env:PATH = $dep + '/bin;' + $env:PATH
& ($out+'/replay.exe') (Join-Path $out 'producer-payloads.json') (Join-Path $repo 'unit_test/MaaCore/fixtures/blackflow-expedition/20261024/lifecycle-payloads.json') (Join-Path $repo 'unit_test/MaaCore/fixtures/blackflow-encounter/20261024/lifecycle-payloads.json')
exit $LASTEXITCODE
