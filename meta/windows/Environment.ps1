# Shared toolchain setup for Build.ps1 and Package.ps1; dot-source this file.
function Resolve-QtDirectory([string]$QtDirectory) {
    if ([string]::IsNullOrWhiteSpace($QtDirectory)) { $QtDirectory = $env:QT_ROOT_DIR }
    if ([string]::IsNullOrWhiteSpace($QtDirectory)) { $QtDirectory = $env:Qt5_DIR }
    if ([string]::IsNullOrWhiteSpace($QtDirectory)) {
        throw 'Provide -QtDir or set QTDIR / QT_ROOT_DIR / Qt5_DIR to Qt 5.15.2 msvc2019_64.'
    }
    return (Resolve-Path -LiteralPath $QtDirectory).Path
}

function Initialize-MsvcEnvironment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio Installer vswhere.exe is required. Install Visual Studio 2022 or Build Tools with Desktop development with C++.'
    }
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($visualStudio)) {
        throw 'No Visual Studio installation with the x64 MSVC tools was found.'
    }
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) { throw "vcvars64.bat not found: $vcvars" }
    $compilerEnvironment = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed with exit code $LASTEXITCODE." }
    foreach ($entry in $compilerEnvironment) {
        if ($entry -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
}
