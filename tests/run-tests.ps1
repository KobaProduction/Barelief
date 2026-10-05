param(
    [ValidateSet('quick','dsp','pipeline','ui','capture','network','integration','performance','full')]
    [string]$Profile = 'quick',
    [string]$BuildDirectory = 'cmake-build-release',
    [string]$CTest = 'ctest',
    [switch]$List
)
$ErrorActionPreference = 'Stop'
$label = if ($Profile -eq 'full') { 'quick|integration' } else { "^$Profile`$" }
$testArgs = @('--test-dir', $BuildDirectory, '--output-on-failure', '--no-tests=error', '-L', $label)
if ($List) { $testArgs += '-N' }
& $CTest @testArgs
exit $LASTEXITCODE
