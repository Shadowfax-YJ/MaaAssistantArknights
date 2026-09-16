param([Parameter(Mandatory=$true)][string]$CoreBuildDirectory, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$core = (Resolve-Path $CoreBuildDirectory).Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo 'build\blackflow-transition-replay' }
$out = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $out -Force | Out-Null
if (!(Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'MSVC developer tools are required.' }
    & (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
}
$dep = Join-Path $repo 'src\MaaUtils\MaaDeps\vcpkg\installed\maa-x64-windows'
$includes = @('src\MaaCore','src\MaaUtils\include','include','3rdparty\include','3rdparty\EmulatorExtras') | ForEach-Object { '/I'+(Join-Path $repo $_) }
& cl.exe /nologo /c /std:c++20 /EHsc /MD /O2 /utf-8 /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /D_WIN32_WINNT=0x0A00 /DASST_DLL_EXPORTS /DASST_WITH_EMULATOR_EXTRAS=1 /DASST_WITH_EXTRA_CALLERS=0 /DASST_WITH_MAC_SCK=0 @includes ('/I'+$dep+'\include') ('/I'+$dep+'\include\opencv4') ('/Fo'+$out+'\replay.obj') (Join-Path $PSScriptRoot 'BlackFlowTransitionReplay.cpp')
if ($LASTEXITCODE) { exit $LASTEXITCODE }
# Link the built production objects, including Session and recognizers, rather than a mock implementation.
$objs = Get-ChildItem -LiteralPath (Join-Path $core 'src\MaaCore\MaaCore.dir\RelWithDebInfo') -Filter '*.obj' | ForEach-Object { '"'+$_.FullName+'"' }
$libs = @('opencv_world4','fastdeploy_ppocr','onnxruntime','zlib','boost_system-vc143-mt-x64-1_86','boost_regex-vc143-mt-x64-1_86') | ForEach-Object { '"'+$dep+'\lib\'+$_.ToString()+'.lib"' }
$argsFile = @('/NOLOGO','/INCREMENTAL:NO','/SUBSYSTEM:CONSOLE',('/OUT:"'+$out+'\replay.exe"'),('"'+$out+'\replay.obj"'),('"'+$core+'\lib\RelWithDebInfo\MaaUtils.lib"'),('"'+$core+'\lib\RelWithDebInfo\maa-monocypher.lib"')) + $objs + $libs + @('ws2_32.lib','dxgi.lib','kernel32.lib','user32.lib','gdi32.lib','winspool.lib','shell32.lib','ole32.lib','oleaut32.lib','uuid.lib','comdlg32.lib','advapi32.lib')
$argsFile | Set-Content -LiteralPath (Join-Path $out 'replay.rsp') -Encoding utf8
& link.exe ('@'+$out+'\replay.rsp')
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$env:PATH = $core+'\bin\RelWithDebInfo;'+$dep+'\bin;'+$env:PATH
Push-Location $out
try {
    $routing = Get-Content -LiteralPath (Join-Path $repo 'src\MaaCore\Task\Roguelike\BlackFlow\BlackFlowRoutingTaskPlugin.cpp') -Raw
    $match = [regex]::Match($routing, 'm_floor_recovery_attempted = true;\s*Task\.set_task_base\("BlackFlow@Roguelike@RoutingAction", "([^"]+)"\)')
    if (!$match.Success) { throw 'Cannot locate the production floor recovery dispatch; update this replay seam.' }
    & ($out+'\replay.exe') $repo (Join-Path $PSScriptRoot 'fixtures\blackflow-transitions') $match.Groups[1].Value > replay-output.txt
    $result = $LASTEXITCODE
    Get-Content replay-output.txt | Where-Object { $_ -match '^(PASS|FAIL|[0-9]+ passed)' }
    exit $result
}
finally { Pop-Location }
