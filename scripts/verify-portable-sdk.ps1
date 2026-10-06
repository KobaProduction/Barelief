param(
    [string]$BuildDirectory = 'build-sdk-verify',
    [string]$CxxCompiler = 'C:/Qt/Tools/mingw1310_64/bin/g++.exe',
    [string]$CCompiler = 'C:/Qt/Tools/mingw1310_64/bin/gcc.exe',
    [string]$BdspSourceDirectory = ''
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBuild = [IO.Path]::GetFullPath((Join-Path $taskRoot $BuildDirectory))
$taskPrefix = Join-Path $taskBuild 'prefix'
$taskConsumer = Join-Path $taskBuild 'consumer'
$env:PATH = (Split-Path -Parent $CxxCompiler) + ';' + $env:PATH
function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
$taskConfigure = @('-S', $taskRoot, '-B', $taskBuild, '-G', 'Ninja',
    '-DBUILD_PORTABLE_SDK=ON', '-DBUILD_DSP_ONLY=OFF', '-DBUILD_GUI=OFF',
    '-DPORTABLE_DISABLE_EXCEPTIONS_RTTI=ON', '-DBUILD_TESTING=ON',
    '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_CXX_COMPILER=$CxxCompiler", "-DCMAKE_C_COMPILER=$CCompiler")
if ($BdspSourceDirectory) { $taskConfigure += "-DFETCHCONTENT_SOURCE_DIR_BDSP=$BdspSourceDirectory" }
Invoke-Checked cmake $taskConfigure
Invoke-Checked cmake @('--build', $taskBuild, '--parallel', '3')
Invoke-Checked ctest @('--test-dir', $taskBuild, '--output-on-failure')
Invoke-Checked cmake @('--install', $taskBuild, '--prefix', $taskPrefix)
Invoke-Checked cmake @('-S', (Join-Path $taskRoot 'tests/sdk_consumer'), '-B', $taskConsumer,
    '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_CXX_COMPILER=$CxxCompiler", "-DCMAKE_PREFIX_PATH=$taskPrefix")
Invoke-Checked cmake @('--build', $taskConsumer, '--parallel', '2')
Invoke-Checked (Join-Path $taskConsumer 'sdk_consumer.exe') @()
