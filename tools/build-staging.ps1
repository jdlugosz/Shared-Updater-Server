# Shared Updater Server - tools/build-staging.ps1
# <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

[CmdletBinding()]
param(
    [ValidatePattern('^[a-zA-Z0-9_.-]+$')]
    [string]$SshTarget = 'vps-stage',
    [ValidatePattern('^[a-zA-Z0-9_.-]+$')]
    [string]$AdminSshTarget = 'vps-stage-root',
    [switch]$Bootstrap
)

$ErrorActionPreference = 'Stop'
$sshOptions = @('-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '-o', 'ConnectTimeout=10')

function Invoke-NativeChecked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE."
    }
}

if ($Bootstrap) {
    Invoke-NativeChecked -Program ssh -Arguments ($sshOptions + @($AdminSshTarget,
        'bash /mnt/hgfs/Shared-Updater-Server/tools/bootstrap-staging.sh'))
}

Invoke-NativeChecked -Program ssh -Arguments ($sshOptions + @($SshTarget,
    'cd /mnt/hgfs/Shared-Updater-Server && cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc'))
