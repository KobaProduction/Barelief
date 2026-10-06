param(
    [ValidateSet('quick','dsp','pipeline','ui','capture','network','integration','performance','full')]
    [string]$Profile = 'quick',
    [string]$BuildDirectory = 'cmake-build-release',
    [string]$CTest = 'ctest',
    [string[]]$TestName = @(),
    [switch]$List
)
$ErrorActionPreference = 'Stop'
$testArgs = @('--test-dir', $BuildDirectory, '--output-on-failure', '--no-tests=error')
if ($TestName.Count) {
    # Exact named scenarios take precedence over broad profiles.
    $names = ($TestName | ForEach-Object { [regex]::Escape($_) }) -join '|'
    $testArgs += @('-R', "^($names)$")
} else {
    $label = if ($Profile -eq 'full') { 'quick|integration|runtime' } else { "^$Profile`$" }
    $testArgs += @('-L', $label)
    # Default/portable passes must never launch UI, devices or benchmarks,
    # even if a test accidentally receives a second 'quick' label later.
    if ($Profile -in @('quick','dsp')) {
        $testArgs += @('-LE', '^(ui|native|graphics|performance|device|integration)$')
    } elseif ($Profile -ne 'performance') {
        $testArgs += @('-LE', 'native|performance')
    }
}
if ($List) { $testArgs += '-N' }
& $CTest @testArgs
exit $LASTEXITCODE
