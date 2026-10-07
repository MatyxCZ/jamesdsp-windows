# Build the x64 Release application from a recursive checkout (Windows PowerShell 5.1+).
[CmdletBinding()]
param(
    [string]$QtDir = $env:QTDIR,
    [string]$BuildDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

try {
    . (Join-Path $PSScriptRoot 'Environment.ps1')
    $QtDir = Resolve-QtDirectory $QtDir
    $qmake = Join-Path $QtDir 'bin/qmake.exe'
    if (-not (Test-Path -LiteralPath $qmake -PathType Leaf)) { throw "qmake.exe not found: $qmake" }

    Initialize-MsvcEnvironment
    $env:PATH = (Join-Path $QtDir 'bin') + ';' + $env:PATH
    $nmake = (Get-Command nmake.exe -ErrorAction Stop).Source
    $repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
    if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $repository '../build' }
    $BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
    if ($BuildDirectory.TrimEnd('\', '/') -eq $repository.TrimEnd('\', '/')) {
        throw 'Use a separate shadow-build directory, not the source directory.'
    }
    New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null
    Push-Location -LiteralPath $BuildDirectory
    try {
        & $qmake -r (Join-Path $repository 'JDSP4Linux.pro') -spec win32-msvc 'CONFIG+=release' 'CONFIG-=debug'
        if ($LASTEXITCODE -ne 0) { throw "Recursive qmake failed with exit code $LASTEXITCODE." }
        & $nmake /nologo release
        if ($LASTEXITCODE -ne 0) { throw "nmake release failed with exit code $LASTEXITCODE." }
    }
    finally { Pop-Location }
    $executable = Join-Path $BuildDirectory 'src/release/jamesdsp.exe'
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "The build did not produce $executable" }
    Write-Host "Built: $executable"
}
catch {
    Write-Error -Message $_ -ErrorAction Continue
    exit 1
}
