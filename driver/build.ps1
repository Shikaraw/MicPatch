[CmdletBinding()]
param(
    [ValidateSet('en', 'zh-CN')][string]$Language = 'en',
    [ValidateSet('x64', 'ARM64')][string]$Platform = 'x64',
    [switch]$PrepareOnly
)

$ErrorActionPreference = 'Stop'
$commit = 'd49bb7d72f3651952b2b6e4ad28d14fe2247eb69'
$sha256 = 'e28e89b274c1bece0a51fbdbb43e6a139802ca7985b26b6539940d00993ecc5f'
$scratch = Join-Path $PSScriptRoot '.scratch'
$archive = Join-Path $scratch 'upstream.zip'
$work = Join-Path $scratch "build-$Platform-$Language"
$tree = Join-Path $work "Windows-driver-samples-$commit"
$source = Join-Path $tree 'audio\sysvad'
$package = Join-Path $PSScriptRoot "out\$Platform\$Language"
$patch = Join-Path $PSScriptRoot 'sysvad-capture.patch'
$git = (Get-Command git -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source

if (-not $PrepareOnly) {
    $msbuild = Get-Command MSBuild.exe -CommandType Application -ErrorAction SilentlyContinue
    if ($msbuild) {
        $msbuildPath = $msbuild.Source
    } else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'Install Visual Studio 2022 C++ build tools and the WDK.' }
        $msbuildPath = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\Current\Bin\MSBuild.exe' | Select-Object -First 1
        if (-not $msbuildPath) { throw 'Visual Studio 2022 MSBuild was not found.' }
    }
    $kits = (Get-ItemPropertyValue -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10)
    $wdk = Get-ChildItem -LiteralPath (Join-Path $kits 'Include') -Directory |
        Where-Object { (Test-Path (Join-Path $_.FullName 'km\portcls.h')) -and ([version]$_.Name -ge [version]'10.0.22621.0') } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $wdk) { throw 'Install a matching Windows SDK and WDK (22621 or later), including VS integration.' }
    $inf2cat = Join-Path $kits "bin\$($wdk.Name)\x86\Inf2Cat.exe"
    if (-not (Test-Path -LiteralPath $inf2cat -PathType Leaf)) { throw "Inf2Cat not found: $inf2cat" }
}

New-Item -ItemType Directory -Path $scratch -Force | Out-Null
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    $download = "$archive.download"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri "https://codeload.github.com/microsoft/Windows-driver-samples/zip/$commit" -OutFile $download
        if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $sha256) { throw 'Unexpected upstream ZIP SHA-256.' }
        Move-Item -LiteralPath $download -Destination $archive
    } finally {
        if (Test-Path -LiteralPath $download) { Remove-Item -LiteralPath $download -Force }
    }
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $sha256) { throw "Unexpected SHA-256: $archive" }

if (Test-Path -LiteralPath $tree) { Remove-Item -LiteralPath $tree -Force -Recurse }
New-Item -ItemType Directory -Path $work -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    # Only extract this pinned sample, not the unrelated projects in the ZIP.
    $prefix = "Windows-driver-samples-$commit/audio/sysvad/"
    foreach ($entry in $zip.Entries) {
        if (-not $entry.Name -or -not $entry.FullName.StartsWith($prefix, [StringComparison]::Ordinal)) { continue }
        $file = Join-Path $source $entry.FullName.Substring($prefix.Length)
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($file)) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $file)
    }
} finally {
    $zip.Dispose()
}
& $git -C $source apply --whitespace=nowarn -- $patch
if ($LASTEXITCODE -ne 0) { throw 'The MicPatch SysVAD patch failed to apply.' }
foreach ($file in @('MpMicCapture.h', 'MpMicCapture.cpp', 'MicPatchMic.vcxproj')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $source
}
Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'virtual_mic_protocol.h') -Destination $source
$micName = if ($Language -eq 'zh-CN') {
    'MicPatch ' + (-join ([char[]]@(0x865A,0x62DF,0x9EA6,0x514B,0x98CE)))
} else { 'MicPatch Virtual Microphone' }
$inf = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'MicPatchMic.inx')).Replace('@MIC_NAME@', $micName)
[IO.File]::WriteAllText((Join-Path $source 'MicPatchMic.inx'), $inf, [Text.Encoding]::Unicode)
if ($PrepareOnly) { Write-Output $source; return }

& $msbuildPath (Join-Path $source 'MicPatchMic.vcxproj') /m /nologo /t:Build `
    /p:Configuration=Release "/p:Platform=$Platform" "/p:WindowsTargetPlatformVersion=$($wdk.Name)" `
    /p:SignMode=Off /p:RunInf2Cat=false /p:DeployDriver=false
if ($LASTEXITCODE -ne 0) { throw 'MicPatchMic WDK build failed.' }
$bin = Join-Path $source "bin\$Platform"
$sys = Join-Path $bin 'MicPatchMic.sys'
$stampedInf = Get-ChildItem -LiteralPath $bin -Filter 'MicPatchMic.inf' -Recurse -File | Select-Object -First 1
if (-not (Test-Path -LiteralPath $sys -PathType Leaf) -or -not $stampedInf) { throw 'Missing MicPatchMic.sys or stamped MicPatchMic.inf.' }

$staging = Join-Path $work 'package'
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Force -Recurse }
New-Item -ItemType Directory -Path $staging | Out-Null
Copy-Item -LiteralPath $sys,$stampedInf.FullName -Destination $staging
$os = if ($Platform -eq 'x64') { '10_NI_X64' } else { '10_NI_ARM64' }
& $inf2cat "/driver:$staging" "/os:$os" /uselocaltime
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $staging 'MicPatchMic.cat'))) { throw 'Catalog generation failed.' }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE') -Destination $staging
New-Item -ItemType Directory -Path $package -Force | Out-Null
Copy-Item -Path (Join-Path $staging '*') -Destination $package -Force
Write-Output "Unsigned package: $package"
