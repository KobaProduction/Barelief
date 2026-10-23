param(
    [ValidateSet('Setup','Open','Capture','Export')][string]$Action='Open',
    [string]$Output,
    [ValidateRange(1,600)][int]$Seconds=10,
    [ValidateRange(1024,65535)][int]$Port=8086
)
$ErrorActionPreference='Stop'
$toolRoot=Join-Path (Split-Path $PSScriptRoot -Parent) '.agent-workspace/tools/tracy/0.14.1'
$viewer=Join-Path $toolRoot 'tracy-profiler.exe'
if($Action -eq 'Setup') {
    New-Item -ItemType Directory -Force -Path $toolRoot | Out-Null
    $archive=Join-Path $toolRoot 'windows-0.14.1.zip'
    if(!(Test-Path -LiteralPath $archive)){Invoke-WebRequest 'https://github.com/wolfpld/tracy/releases/download/v0.14.1/windows-0.14.1.zip' -OutFile $archive}
    if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne 'F7499D74914AA3BA94A2C1CE72F36477D7B61D9D0F7C9790E05274C258C97FB5') {
        throw 'Tracy download checksum mismatch'
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $toolRoot -Force
    Write-Output $toolRoot
    exit 0
}
if(!(Test-Path -LiteralPath $viewer)){throw 'Run scripts/tracy.ps1 -Action Setup first'}
switch($Action) {
    'Open' {
        if($Output){Start-Process -FilePath $viewer -ArgumentList ('"'+(Resolve-Path -LiteralPath $Output).Path+'"') | Out-Null}
        else{Start-Process -FilePath $viewer | Out-Null}
    }
    'Capture' {
        if(!$Output){throw 'Capture requires -Output <fresh-file.tracy>'}
        if(Test-Path -LiteralPath $Output){throw 'Output already exists; choose a fresh file'}
        & (Join-Path $toolRoot 'tracy-capture.exe') -a 127.0.0.1 -p $Port -s $Seconds -o $Output
        exit $LASTEXITCODE
    }
    'Export' {
        if(!$Output){throw 'Export requires -Output <existing-file.tracy>'}
        & (Join-Path $toolRoot 'tracy-csvexport.exe') $Output
        exit $LASTEXITCODE
    }
}
