#Requires -Version 7.0
# Shared Updater Server - tools/show-catalog.ps1
# <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9_.][A-Za-z0-9_.-]*(?:@[A-Za-z0-9_.][A-Za-z0-9_.-]*)?$')]
    [string] $SshTarget = 'root@vps',
    [string] $OutputPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out/catalog-report/live.html'),
    [switch] $IncludeTargetIds,
    [switch] $NoOpen
)

$ErrorActionPreference = 'Stop'
$destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
if ([IO.Path]::GetExtension($destination) -ne '.html') { throw 'Choose an .html output path.' }
$source = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'catalog_report.py') -Raw
$command = if ($IncludeTargetIds) { 'python3 - --include-target-ids' } else { 'python3 -' }
$options = @('-T', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '-o', 'ConnectTimeout=10')
$html = @($source | ssh @options $SshTarget $command)
if ($LASTEXITCODE -ne 0 -or $html.Count -eq 0 -or $html[0] -ne '<!doctype html>') {
    throw 'Could not capture the live catalog over SSH.'
}

[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
$temporary = "$destination.$([Guid]::NewGuid().ToString('N')).tmp"
try {
    [IO.File]::WriteAllLines($temporary, $html)
    [IO.File]::Move($temporary, $destination, $true)
}
finally {
    if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
}

if (!$NoOpen) { Start-Process -FilePath $destination }
Write-Output $destination
