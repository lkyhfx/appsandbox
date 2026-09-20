<#
  Build the local Release binaries.

  Usage:
    .\build-release.ps1
    .\build-release.ps1 -Platform ARM64
    .\build-release.ps1 -Clean

  This is a compile-only helper. It does not EV-sign binaries or create a
  publishable release ZIP. Use tools\sign\make-release.ps1 for that workflow.
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$solution = Join-Path $repo 'AppSandbox.sln'

$vsDevShell = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
$vcTargetsPath = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Microsoft\VC\v170\'

foreach ($path in @($vsDevShell, $msbuild, $solution, (Join-Path $vcTargetsPath 'Microsoft.Cpp.Default.props'))) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required build file not found: $path"
    }
}

$outputDirectory = if ($Platform -eq 'ARM64') {
    Join-Path $repo 'bin\Release-ARM64'
} else {
    Join-Path $repo 'bin\Release'
}

Write-Host "Loading Visual Studio Developer PowerShell..."
& $vsDevShell -Arch amd64 -HostArch amd64
if ($LASTEXITCODE -ne 0) {
    throw "Visual Studio Developer PowerShell failed (exit $LASTEXITCODE)."
}

# Prevent AppSandboxPackage from recursively invoking make-release.ps1.
$env:ASB_SKIP_PACKAGE_PROJECT = '1'

# The repository's projects import $(VCTargetsPath)\Microsoft.Cpp.*. Keep the
# trailing slash; MSBuild concatenates it with Platforms\ and the file name.
$env:VCTargetsPath = $vcTargetsPath

$target = if ($Clean) { 'Clean;Build' } else { 'Build' }
$msbuildArguments = @(
    $solution
    "/t:$target"
    '/p:Configuration=Release'
    "/p:Platform=$Platform"
    '/p:SignMode=Off'
    '/m'
    '/v:minimal'
    '/nologo'
)

Write-Host "Building Release|$Platform ..."
& $msbuild @msbuildArguments
if ($LASTEXITCODE -ne 0) {
    throw "Release build failed (exit $LASTEXITCODE)."
}

$application = Join-Path $outputDirectory 'AppSandbox.exe'
if (-not (Test-Path -LiteralPath $application)) {
    throw "Build completed but the expected output was not found: $application"
}

$version = (Get-Item -LiteralPath $application).VersionInfo.FileVersion
Write-Host ""
Write-Host "Release build succeeded: $version"
Write-Host "Output: $outputDirectory"
