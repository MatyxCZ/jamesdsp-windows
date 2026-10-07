# Publish committed source plus the exact recursively pinned submodule sources.
[CmdletBinding()]
param([string]$OutputDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$stage = $null
try {
    $repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $repository '../dist' }
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $revision = & git -C $repository rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Source packaging requires a Git checkout.' }
    $submodules = @(& git -C $repository submodule status --recursive)
    if ($LASTEXITCODE -ne 0) { throw 'Could not read the pinned submodules.' }
    $stage = Join-Path ([IO.Path]::GetTempPath()) ('JamesDSP-source-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage | Out-Null
    $prefix = 'JamesDSP-windows-source/'
    $snapshot = Join-Path $stage 'snapshot.zip'
    $contents = Join-Path $stage 'contents'
    & git -C $repository archive --format=zip "--prefix=$prefix" -o $snapshot HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Could not archive the committed application source.' }
    Expand-Archive -LiteralPath $snapshot -DestinationPath $contents
    $provenance = @("Application commit: $revision")
    foreach ($entry in $submodules) {
        if ($entry -notmatch '^(?<state>[-+U ])(?<revision>[0-9a-f]{40})\s+(?<path>\S+)(?:\s|$)') {
            throw "Unrecognized submodule status: $entry"
        }
        if ($Matches['state'] -ne ' ') { throw "Initialize the exact pinned submodule before packaging: $entry" }
        $path = $Matches['path']
        $commit = $Matches['revision']
        & git -C (Join-Path $repository $path) archive --format=zip "--prefix=$prefix$path/" -o $snapshot $commit
        if ($LASTEXITCODE -ne 0) { throw "Could not archive submodule $path." }
        Expand-Archive -LiteralPath $snapshot -DestinationPath $contents -Force
        $provenance += "$path commit: $commit"
    }
    $source = Join-Path $contents 'JamesDSP-windows-source'
    $provenance | Set-Content -LiteralPath (Join-Path $source 'SOURCE_REVISION.txt') -Encoding UTF8
    $archive = Join-Path $OutputDirectory 'JamesDSP-windows-source.zip'
    Compress-Archive -LiteralPath $source -DestinationPath $archive -CompressionLevel Optimal -Force
    Write-Host "Complete corresponding source: $archive"
}
catch {
    Write-Error -Message $_ -ErrorAction Continue
    exit 1
}
finally {
    if ($null -ne $stage -and (Test-Path -LiteralPath $stage)) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
