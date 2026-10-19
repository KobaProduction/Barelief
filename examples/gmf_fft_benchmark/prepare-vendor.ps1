param([string]$Destination='.agent-workspace/gmf-fft-vendor')
$ErrorActionPreference='Stop'
$gmfLock=Get-Content -LiteralPath "$PSScriptRoot/vendor.lock.json" -Raw | ConvertFrom-Json
foreach($gmfFile in $gmfLock.files) {
    $gmfTarget=Join-Path $Destination $gmfFile.path
    if(Test-Path -LiteralPath $gmfTarget) {
        if((Get-FileHash -LiteralPath $gmfTarget -Algorithm SHA256).Hash.ToLower() -ne $gmfFile.sha256) {
            throw "Existing file differs; preserve it and select another destination: $gmfTarget"
        }
        continue
    }
    New-Item -ItemType Directory -Force (Split-Path $gmfTarget) | Out-Null
    $gmfUrl="https://raw.githubusercontent.com/$($gmfLock.repository)/$($gmfLock.revision)/$($gmfLock.package)/$($gmfFile.path)"
    Invoke-WebRequest -UseBasicParsing $gmfUrl -OutFile $gmfTarget
    if((Get-FileHash -LiteralPath $gmfTarget -Algorithm SHA256).Hash.ToLower() -ne $gmfFile.sha256) {
        throw "Downloaded file hash mismatch: $gmfTarget"
    }
}
Write-Output (Resolve-Path -LiteralPath $Destination).Path
