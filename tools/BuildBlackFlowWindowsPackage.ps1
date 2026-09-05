# Builds the dedicated BlackFlow Windows package from source. The release
# version has one source of truth: BLACKFLOW_DATA_COLLECTION_VERSION in ci.yml.
param(
    [string]$Version,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$workflowPath = Join-Path $repoRoot '.github/workflows/ci.yml'
$configuredVersion = [regex]::Match(
    [IO.File]::ReadAllText($workflowPath),
    '(?m)^\s*BLACKFLOW_DATA_COLLECTION_VERSION:\s*(v\S+)\s*$').Groups[1].Value
if ([string]::IsNullOrWhiteSpace($configuredVersion)) {
    throw "BLACKFLOW_DATA_COLLECTION_VERSION was not found in $workflowPath"
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $configuredVersion
}
elseif ($Version -ne $configuredVersion) {
    throw "Requested version $Version differs from workflow version $configuredVersion"
}

if ($Version -notmatch '^v(?<numeric>\d+\.\d+\.\d+)$') {
    throw "BlackFlow package version must be vMAJOR.MINOR.PATCH: $Version"
}
$numericVersion = $Matches.numeric

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = $repoRoot
}
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $outputRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $outputRoot | Out-Null
}

$workRoot = Join-Path $repoRoot "build/package-blackflow-$Version"
$coreBuild = Join-Path $workRoot 'core'
$coreInstall = Join-Path $workRoot 'core-install'
$uiPublish = Join-Path $workRoot 'ui-publish'
$staging = Join-Path $workRoot ('staging-' + [guid]::NewGuid().ToString('N'))
$archivePath = Join-Path $outputRoot "MAA-BlackFlow-Data-Collection-$Version-win-x64.zip"

foreach ($directory in @($coreInstall, $uiPublish, $staging)) {
    $fullPath = [IO.Path]::GetFullPath($directory)
    if (-not $fullPath.StartsWith(([IO.Path]::GetFullPath($workRoot) + [IO.Path]::DirectorySeparatorChar), [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a path outside the package work directory: $fullPath"
    }
    if (Test-Path -LiteralPath $fullPath) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
}

New-Item -ItemType Directory -Path $staging | Out-Null

Push-Location $repoRoot
try {
    & cmake -B $coreBuild --preset windows-publish-x64 `
        "-DMAA_HASH_VERSION=$Version" `
        '-DBUILD_WPF_GUI=ON' `
        '-DINSTALL_RESOURCE=ON' `
        '-DINSTALL_PYTHON=ON' `
        "-DCMAKE_INSTALL_PREFIX=$coreInstall"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

    & cmake --build $coreBuild --config RelWithDebInfo --target MaaCore MaaAppHostStub MAA.Updater --parallel $env:NUMBER_OF_PROCESSORS
    if ($LASTEXITCODE -ne 0) { throw "Core build failed: $LASTEXITCODE" }

    & cmake --install $coreBuild --config RelWithDebInfo
    if ($LASTEXITCODE -ne 0) { throw "Core install failed: $LASTEXITCODE" }

    & dotnet publish src/MaaWpfGui/MaaWpfGui.csproj `
        -c Release `
        -p:Platform=x64 `
        "-p:Version=$numericVersion" `
        "-p:FileVersion=$numericVersion" `
        "-p:AssemblyVersion=$numericVersion.0" `
        "-p:InformationalVersion=$Version" `
        -o $uiPublish
    if ($LASTEXITCODE -ne 0) { throw "WPF publish failed: $LASTEXITCODE" }
}
finally {
    Pop-Location
}

Copy-Item -Path (Join-Path $coreInstall '*') -Destination $staging -Recurse -Force
Copy-Item -Path (Join-Path $uiPublish '*') -Destination $staging -Recurse -Force

$nativeOutput = Join-Path $coreBuild 'bin/RelWithDebInfo'
foreach ($file in @('MaaCore.dll', 'MaaUtils.dll', 'fastdeploy_ppocr_maa.dll', 'DirectML.dll', 'onnxruntime_maa.dll', 'opencv_world4_maa.dll', 'MAA.Updater.exe')) {
    Copy-Item -LiteralPath (Join-Path $nativeOutput $file) -Destination (Join-Path $staging $file) -Force
}
Copy-Item -LiteralPath (Join-Path $nativeOutput 'MaaAppHostStub.exe') -Destination (Join-Path $staging 'MAA.exe') -Force

$controlUnitCandidates = @(
    (Join-Path $repoRoot 'build/bin/RelWithDebInfo'),
    (Join-Path $repoRoot 'build/bin/Release'),
    (Join-Path $repoRoot 'build-publish-x64/maaframework-temp/extracted/bin')
)
$controlUnitRoot = $controlUnitCandidates | Where-Object {
    (Test-Path -LiteralPath (Join-Path $_ 'MaaAdbControlUnit.dll') -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $_ 'MaaWin32ControlUnit.dll') -PathType Leaf)
} | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($controlUnitRoot)) {
    throw "A complete MaaFramework control-unit directory was not found: $($controlUnitCandidates -join ', ')"
}
foreach ($file in @('MaaAdbControlUnit.dll', 'MaaWin32ControlUnit.dll')) {
    $source = Join-Path $controlUnitRoot $file
    Copy-Item -LiteralPath $source -Destination (Join-Path $staging $file) -Force
}

Copy-Item -LiteralPath (Join-Path $repoRoot 'tools/DependencySetup_依赖库安装.bat') -Destination (Join-Path $staging 'DependencySetup.bat')
New-Item -ItemType Directory -Path (Join-Path $staging 'config') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'BlackFlowDataCollection/gui.new.json') -Destination (Join-Path $staging 'config/gui.new.json')
$notice = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'BlackFlowDataCollection/NOTICE.txt')).Replace('{{VERSION}}', $Version)
[IO.File]::WriteAllText((Join-Path $staging 'NOTICE.txt'), $notice, [Text.UTF8Encoding]::new($false))

Get-ChildItem -LiteralPath $staging -Recurse -File | Where-Object { $_.Extension -in @('.pdb', '.bak', '.h', '.pyc', '.log') } | Remove-Item -Force
Get-ChildItem -LiteralPath $staging -Recurse -Directory -Filter '__pycache__' | Remove-Item -Recurse -Force
foreach ($directoryName in @('msvc-debug', 'cache', 'data', 'debug', 'resource/debug')) {
    $directory = Join-Path $staging $directoryName
    if (Test-Path -LiteralPath $directory) {
        Remove-Item -LiteralPath $directory -Recurse -Force
    }
}

& (Join-Path $PSScriptRoot 'GenerateFileList.ps1') -InstallDir $staging
if ($LASTEXITCODE -ne 0) { throw "File list generation failed: $LASTEXITCODE" }

& (Join-Path $PSScriptRoot 'NewVerifiedWindowsPackage.ps1') `
    -InstallDir $staging `
    -ArchivePath $archivePath `
    -ExpectedVersion $Version
if ($LASTEXITCODE -ne 0) { throw "Package verification failed: $LASTEXITCODE" }

$package = Get-Item -LiteralPath $archivePath
$hash = Get-FileHash -LiteralPath $archivePath -Algorithm SHA256
Write-Host "Package=$($package.FullName)"
Write-Host "Size=$($package.Length)"
Write-Host "SHA256=$($hash.Hash)"
