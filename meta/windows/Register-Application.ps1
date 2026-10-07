# Register this extracted copy for the current user; no administrator rights required.
[CmdletBinding()]
param(
    [string]$InstallDirectory,
    [switch]$StartWithWindows
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

try {
    if ([string]::IsNullOrWhiteSpace($InstallDirectory)) { $InstallDirectory = $PSScriptRoot }
    $InstallDirectory = (Resolve-Path -LiteralPath $InstallDirectory).Path
    $executable = Join-Path $InstallDirectory 'jamesdsp.exe'
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "jamesdsp.exe was not found in $InstallDirectory. Extract the complete portable ZIP before registering it."
    }
    $programs = [Environment]::GetFolderPath('Programs')
    if ([string]::IsNullOrWhiteSpace($programs)) { throw 'The current user Start Menu Programs folder could not be found.' }
    New-Item -ItemType Directory -Path $programs -Force | Out-Null
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut((Join-Path $programs 'JamesDSP.lnk'))
    $shortcut.TargetPath = $executable
    $shortcut.Arguments = ''
    $shortcut.WorkingDirectory = $InstallDirectory
    $shortcut.Description = 'JamesDSP - system-wide audio effects and equalizer'
    $shortcut.IconLocation = "$executable,0"
    $shortcut.Save()

    # No switch means no registry write: preserve the user's existing startup choice.
    if ($StartWithWindows) {
        $runKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
        try { $runKey.SetValue('JamesDSP', ('"{0}" --tray' -f $executable), [Microsoft.Win32.RegistryValueKind]::String) }
        finally { $runKey.Dispose() }
    }
    Write-Host 'JamesDSP is registered in your Start Menu. Windows Search may take a moment to refresh.'
    Write-Host "Keep this extracted folder in place: $InstallDirectory"
    if ($StartWithWindows) { Write-Host 'JamesDSP will start in the tray when you sign in.' }
}
catch {
    Write-Error -Message $_ -ErrorAction Continue
    exit 1
}
