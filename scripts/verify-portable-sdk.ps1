param(
    [string]$CxxCompiler = '',
    [string]$CCompiler = '',
    [string]$BdspSourceDirectory = ''
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBuild = Join-Path $taskRoot 'cmake-build-sdk'
$taskPrefix = Join-Path $taskBuild 'install'
$taskConsumer = Join-Path $taskBuild 'consumer'
function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
function Get-SdkCacheValue {
    param([string]$Name)
    $taskPattern = '^' + [regex]::Escape($Name) + ':[^=]*=(.*)$'
    foreach ($taskLine in Get-Content -LiteralPath (Join-Path $taskBuild 'CMakeCache.txt')) {
        if ($taskLine -match $taskPattern) { return $Matches[1] }
    }
    throw "Missing SDK cache entry: $Name"
}
$taskConfigure = @('--preset', 'sdk')
if ($CxxCompiler) { $taskConfigure += "-DCMAKE_CXX_COMPILER=$CxxCompiler" }
if ($CCompiler) { $taskConfigure += "-DCMAKE_C_COMPILER=$CCompiler" }
if ($BdspSourceDirectory) { $taskConfigure += "-DFETCHCONTENT_SOURCE_DIR_BDSP=$BdspSourceDirectory" }
Push-Location -LiteralPath $taskRoot
$taskOriginalPath = $env:PATH
try {
    Invoke-Checked cmake $taskConfigure
    $taskSelectedCxxCompiler = Get-SdkCacheValue 'CMAKE_CXX_COMPILER'
    $taskSelectedCCompiler = Get-SdkCacheValue 'CMAKE_C_COMPILER'
    $taskCompilerOptions = @(
        "-DCMAKE_CXX_COMPILER=$taskSelectedCxxCompiler",
        "-DCMAKE_C_COMPILER=$taskSelectedCCompiler",
        "-DCMAKE_MAKE_PROGRAM=$(Get-SdkCacheValue 'CMAKE_MAKE_PROGRAM')"
    )
    $env:PATH = (Split-Path -Parent $taskSelectedCxxCompiler) + ';' + $env:PATH
    Invoke-Checked cmake @('--build', '--preset', 'sdk', '--parallel', '3')
    Invoke-Checked ctest @('--preset', 'sdk', '--no-tests=error')
    Invoke-Checked cmake @('--install', $taskBuild, '--prefix', $taskPrefix)
    Invoke-Checked cmake (@('-S', (Join-Path $taskRoot 'tests/sdk_consumer'), '-B', $taskConsumer,
        '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$taskPrefix") + $taskCompilerOptions)
    Invoke-Checked cmake @('--build', $taskConsumer, '--parallel', '2')
    Invoke-Checked (Join-Path $taskConsumer 'sdk_consumer.exe') @()

    # Host component smoke compiles the installed firmware sources with API stubs.
    # It checks manifests/adapter contracts, never replaces an ESP-IDF cross-build.
    $taskComponentBuild = Join-Path $taskBuild 'installed-component'
    $taskBdspSources = if ($BdspSourceDirectory) { $BdspSourceDirectory } else { Join-Path $taskBuild '_deps/bdsp-src' }
    Invoke-Checked cmake (@('-S', (Join-Path $taskRoot 'tests/embedded_sdk_component'), '-B', $taskComponentBuild,
        '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        "-DSDK_ROOT=$(Join-Path $taskPrefix 'share/PortableDSP/embedded')", "-DBDSP_SOURCE_DIR=$taskBdspSources") + $taskCompilerOptions)
    Invoke-Checked cmake @('--build', $taskComponentBuild, '--parallel', '3')
    Invoke-Checked ctest @('--test-dir', $taskComponentBuild, '--output-on-failure', '--no-tests=error')
    Write-Output 'Installed host SDK and component smoke verified. Native ESP-IDF, optimized backend and hardware remain separate checks.'
} finally {
    $env:PATH = $taskOriginalPath
    Pop-Location
}
