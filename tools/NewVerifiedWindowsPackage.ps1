# Creates a UTF-8 ZIP and refuses to publish it unless the unpacked package is
# complete and its WPF/Core versions agree with the requested release version.
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDir,

    [Parameter(Mandatory = $true)]
    [string]$ArchivePath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v\d+\.\d+\.\d+(?:[-.][0-9A-Za-z.-]+)?$')]
    [string]$ExpectedVersion
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-CoreVersion([string]$CorePath) {
    # Do not load the DLL: an x64 CI process cannot load the arm64 matrix
    # artifact. Logger.cpp stores this stable marker immediately before the
    # MAA_VERSION literal, so it is architecture-independent to inspect.
    $binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CorePath))
    $match = [regex]::Match(
        $binaryText,
        'MaaCore Process Start\x00+(?<version>DEBUG_VERSION|v[0-9A-Za-z.+-]+)\x00+Version')
    if (-not $match.Success) {
        throw "Unable to read the embedded MaaCore version: $CorePath"
    }

    return $match.Groups['version'].Value
}

function Assert-PackageTree([string]$Root, [string]$Version) {
    $maaDll = Join-Path $Root 'MAA.dll'
    $coreDll = Join-Path $Root 'MaaCore.dll'
    $fileList = Join-Path $Root 'filelist.txt'
    foreach ($required in @($maaDll, $coreDll, $fileList)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Required package file is missing: $required"
        }
    }

    $uiVersion = (Get-Item -LiteralPath $maaDll).VersionInfo.ProductVersion
    $uiReleaseVersion = ($uiVersion -split '\+', 2)[0]
    $coreVersion = Get-CoreVersion $coreDll
    if ($uiReleaseVersion -ne $Version -or $coreVersion -ne $Version) {
        throw "UI/Core version mismatch: UI=$uiVersion, Core=$coreVersion, expected=$Version"
    }

    $missing = [System.Collections.Generic.List[string]]::new()
    foreach ($relativePath in [IO.File]::ReadAllLines($fileList)) {
        if ([string]::IsNullOrWhiteSpace($relativePath)) {
            continue
        }

        $fullPath = Join-Path $Root ($relativePath.Replace('/', [IO.Path]::DirectorySeparatorChar))
        if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
            $missing.Add($relativePath)
        }
    }

    if ($missing.Count -ne 0) {
        $sample = ($missing | Select-Object -First 5) -join ', '
        throw "Package file list contains $($missing.Count) missing file(s): $sample"
    }

    return [pscustomobject]@{
        UiVersion = $uiVersion
        CoreVersion = $coreVersion
        ListedFiles = ([IO.File]::ReadAllLines($fileList) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count
        ActualFiles = (Get-ChildItem -LiteralPath $Root -Recurse -File).Count
    }
}

$installRoot = (Resolve-Path -LiteralPath $InstallDir).Path
$archiveFullPath = [IO.Path]::GetFullPath($ArchivePath)
$archiveParent = Split-Path -Parent $archiveFullPath
if (-not (Test-Path -LiteralPath $archiveParent -PathType Container)) {
    New-Item -ItemType Directory -Path $archiveParent | Out-Null
}

$sourceResult = Assert-PackageTree $installRoot $ExpectedVersion

$temporaryArchive = Join-Path $archiveParent ('.' + [IO.Path]::GetFileName($archiveFullPath) + '.' + [guid]::NewGuid().ToString('N') + '.tmp')
$verificationRoot = Join-Path $archiveParent ('verify-' + [guid]::NewGuid().ToString('N'))

try {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $installRoot,
        $temporaryArchive,
        [IO.Compression.CompressionLevel]::Optimal,
        $false,
        [Text.Encoding]::UTF8)

    New-Item -ItemType Directory -Path $verificationRoot | Out-Null
    [IO.Compression.ZipFile]::ExtractToDirectory(
        $temporaryArchive,
        $verificationRoot,
        [Text.Encoding]::UTF8,
        $false)

    $extractedResult = Assert-PackageTree $verificationRoot $ExpectedVersion
    if ($sourceResult.ActualFiles -ne $extractedResult.ActualFiles) {
        throw "Extracted file count differs: source=$($sourceResult.ActualFiles), extracted=$($extractedResult.ActualFiles)"
    }

    foreach ($sourceFile in Get-ChildItem -LiteralPath $installRoot -Recurse -File) {
        $relativePath = $sourceFile.FullName.Substring($installRoot.Length + 1)
        $extractedFile = Join-Path $verificationRoot $relativePath
        if (-not (Test-Path -LiteralPath $extractedFile -PathType Leaf)) {
            throw "Extracted package is missing: $relativePath"
        }

        if ($sourceFile.Length -ne (Get-Item -LiteralPath $extractedFile).Length) {
            throw "Extracted file size differs: $relativePath"
        }
    }

    Move-Item -LiteralPath $temporaryArchive -Destination $archiveFullPath -Force
    Write-Host "Verified package created: $archiveFullPath"
    Write-Host "UI=$($extractedResult.UiVersion), Core=$($extractedResult.CoreVersion), files=$($extractedResult.ActualFiles)"
}
finally {
    if (Test-Path -LiteralPath $temporaryArchive) {
        Remove-Item -LiteralPath $temporaryArchive -Force
    }
    if (Test-Path -LiteralPath $verificationRoot) {
        Remove-Item -LiteralPath $verificationRoot -Recurse -Force
    }
}
