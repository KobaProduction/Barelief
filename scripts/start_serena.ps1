param([switch]$Foreground)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (-not $Foreground) {
    $logDirectory = Join-Path $projectRoot '.agent-workspace/serena-server'
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    $shellPath = (Get-Process -Id $PID).Path
    $worker = Start-Process -FilePath $shellPath -WindowStyle Hidden -WorkingDirectory $projectRoot `
        -ArgumentList @('-NoProfile', '-File', ('"{0}"' -f $PSCommandPath), '-Foreground') -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    while ([DateTime]::UtcNow -lt $deadline) {
        $socket = [Net.Sockets.TcpClient]::new()
        try {
            $connection = $socket.ConnectAsync('127.0.0.1', 8766)
            if ($connection.Wait(200) -and $socket.Connected) {
                Write-Output 'Shared Serena is listening at http://127.0.0.1:8766/mcp'
                exit 0
            }
        }
        catch [AggregateException] { }
        finally { $socket.Dispose() }
        if ($worker.HasExited -and $worker.ExitCode -ne 0) {
            throw "Serena startup failed; see $logDirectory"
        }
        Start-Sleep -Milliseconds 250
    }
    throw "Serena startup timed out; see $logDirectory"
}
$logDirectory = Join-Path $projectRoot '.agent-workspace/serena-server'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
Start-Transcript -Path (Join-Path $logDirectory "$PID.log") -Force | Out-Null
$mutex = [Threading.Mutex]::new($false, 'Local\Barelief.Serena.Mcp')
$ownsMutex = $false
try {
    try { $ownsMutex = $mutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
    if (-not $ownsMutex) {
        Write-Output 'The shared Barelief Serena server is already running.'
        exit 0
    }
    $serena = (Get-Command serena -ErrorAction Stop).Source
    $env:PYTHONUTF8 = '1'
    $env:PYTHONIOENCODING = 'utf-8'
    $env:PYTHONUNBUFFERED = '1'
    $env:PSModulePath = [Environment]::GetEnvironmentVariable('PSModulePath', 'Machine')
    & $serena start-mcp-server --project $projectRoot --context codex `
        --agent-interface tools --transport streamable-http --host 127.0.0.1 --port 8766 `
        --enable-web-dashboard false --enable-gui-log-window false --open-web-dashboard false
    if ($LASTEXITCODE -ne 0) { throw "Serena exited with code $LASTEXITCODE" }
}
finally {
    if ($ownsMutex) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
    Stop-Transcript | Out-Null
}
