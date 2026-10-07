# Remove integration owned by this copy only. Never remove app settings or files.
[CmdletBinding()]
param([string]$InstallDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-AppPath([string]$Candidate, [string]$Expected) {
    if ([string]::IsNullOrWhiteSpace($Candidate) -or -not [IO.Path]::IsPathRooted($Candidate)) { return $false }
    try { return [string]::Equals([IO.Path]::GetFullPath($Candidate), $Expected, [StringComparison]::OrdinalIgnoreCase) }
    catch { return $false }
}

try {
    if ([string]::IsNullOrWhiteSpace($InstallDirectory)) { $InstallDirectory = $PSScriptRoot }
    # Allow cleanup after the exe has been deleted; neither files nor this folder are removed.
    $executable = [IO.Path]::GetFullPath((Join-Path $InstallDirectory 'jamesdsp.exe'))
    $programs = [Environment]::GetFolderPath('Programs')
    if ([string]::IsNullOrWhiteSpace($programs)) { throw 'The current user Start Menu Programs folder could not be found.' }
    $shortcutPath = Join-Path $programs 'JamesDSP.lnk'
    if (Test-Path -LiteralPath $shortcutPath -PathType Leaf) {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($shortcutPath)
        if (Test-AppPath $shortcut.TargetPath $executable) {
            Remove-Item -LiteralPath $shortcutPath
            Write-Host 'Removed this copy from the Start Menu.'
        }
    }

    $runKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run', $true)
    if ($null -ne $runKey) {
        try {
            $command = $runKey.GetValue('JamesDSP', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            if ($command -is [string]) {
                $target = $null
                if ($command -match '^"([^"]+)"(?:\s|$)') { $target = $Matches[1] }
                elseif ($command -match '^(\S+)(?:\s|$)') { $target = $Matches[1] }
                if (Test-AppPath $target $executable) {
                    $runKey.DeleteValue('JamesDSP', $false)
                    Write-Host 'Removed this copy from Windows startup.'
                }
            }
        }
        finally { $runKey.Dispose() }
    }
    Write-Host 'Unregistration complete. Application files and user settings were not changed.'
}
catch {
    Write-Error -Message $_ -ErrorAction Continue
    exit 1
}
