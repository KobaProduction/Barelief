param(
    [string]$BinaryDirectory = 'build-gui/bin',
    [string[]]$Executables = @('core_app.exe', 'pipeline_cli.exe', 'aggregator_server_app.exe', 'aggregator_client_app.exe'),
    [string]$Objdump = 'C:/Qt/Tools/mingw1310_64/bin/objdump.exe'
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBinaryDirectory = [IO.Path]::GetFullPath((Join-Path $taskRoot $BinaryDirectory))
if (-not (Test-Path -LiteralPath $Objdump -PathType Leaf)) { throw "Missing import inspection tool: $Objdump" }
$taskQueue = [Collections.Generic.Queue[string]]::new()
$taskVisited = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($taskExecutable in $Executables) {
    $taskPath = Join-Path $taskBinaryDirectory $taskExecutable
    if (-not (Test-Path -LiteralPath $taskPath -PathType Leaf)) { throw "Missing executable: $taskPath" }
    $taskQueue.Enqueue($taskPath)
}
# Inspect deployed plugins as well: Qt loads them dynamically, outside EXE imports.
foreach ($taskPlugin in (Get-ChildItem -LiteralPath $taskBinaryDirectory -Recurse -File -Filter '*.dll')) {
    if ($taskPlugin.DirectoryName -ne $taskBinaryDirectory) { $taskQueue.Enqueue($taskPlugin.FullName) }
}
$taskRequiresGui = $false
while ($taskQueue.Count) {
    $taskPath = $taskQueue.Dequeue()
    if (-not $taskVisited.Add($taskPath)) { continue }
    $taskImports = & $Objdump -p $taskPath
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect PE imports: $taskPath" }
    foreach ($taskLine in $taskImports) {
        if ($taskLine -notmatch 'DLL Name:\s*(\S+)') { continue }
        $taskDll = $Matches[1]
        if ($taskDll -ieq 'Qt6Gui.dll') { $taskRequiresGui = $true }
        # Windows resolves these API-set contracts; they are not deployed DLL files.
        if ($taskDll -match '^(api|ext)-ms-win-') { continue }
        $taskCandidates = @(
            (Join-Path (Split-Path -Parent $taskPath) $taskDll),
            (Join-Path $taskBinaryDirectory $taskDll)
        )
        $taskResolved = $taskCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        if ($taskResolved) { $taskQueue.Enqueue($taskResolved); continue }
        # OS files are dependencies supplied by Windows, not the user's shell PATH.
        $taskSystemDll = Join-Path (Join-Path $env:WINDIR 'System32') $taskDll
        if (-not (Test-Path -LiteralPath $taskSystemDll -PathType Leaf)) {
            throw "Missing runtime DLL $taskDll imported by $taskPath"
        }
    }
}
if ($taskRequiresGui) {
    $taskPlatform = Join-Path $taskBinaryDirectory 'platforms/qwindows.dll'
    if (-not (Test-Path -LiteralPath $taskPlatform -PathType Leaf)) { throw "Missing native Qt platform plugin: $taskPlatform" }
}
Write-Output "Verified $($Executables.Count) executable(s) and $($taskVisited.Count) local PE dependencies in $taskBinaryDirectory."
Write-Output 'This checks deployed files/imports; it does not verify native graphics or physical devices.'
