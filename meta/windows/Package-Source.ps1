# Publish committed source plus the exact recursively pinned submodule sources.
[CmdletBinding()]
param([string]$OutputDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$stage = $null
$bundleArchive = $null
$bundleStream = $null

function Copy-ArchiveEntries([string]$Snapshot, $Destination) {
    # Merge ZIP entries directly: historical source paths exceed legacy Windows
    # filesystem limits when expanded under a long temporary directory.
    $inputArchive = [IO.Compression.ZipFile]::OpenRead($Snapshot)
    try {
        foreach ($entry in $inputArchive.Entries) {
            if ($entry.FullName.EndsWith('/')) { continue }
            $copy = $Destination.CreateEntry($entry.FullName, [IO.Compression.CompressionLevel]::Optimal)
            $copy.LastWriteTime = $entry.LastWriteTime
            $copy.ExternalAttributes = $entry.ExternalAttributes
            $input = $entry.Open()
            try {
                $output = $copy.Open()
                try { $input.CopyTo($output) }
                finally { $output.Dispose() }
            }
            finally { $input.Dispose() }
        }
    }
    finally { $inputArchive.Dispose() }
}

try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    Add-Type -AssemblyName System.IO.Compression
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
    $bundle = Join-Path $stage 'source.zip'
    $bundleStream = [IO.File]::Create($bundle)
    $bundleArchive = [IO.Compression.ZipArchive]::new($bundleStream, [IO.Compression.ZipArchiveMode]::Create)
    & git -C $repository archive --format=zip "--prefix=$prefix" -o $snapshot HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Could not archive the committed application source.' }
    Copy-ArchiveEntries $snapshot $bundleArchive
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
        Copy-ArchiveEntries $snapshot $bundleArchive
        $provenance += "$path commit: $commit"
    }
    $metadata = $bundleArchive.CreateEntry($prefix + 'SOURCE_REVISION.txt')
    $writer = [IO.StreamWriter]::new($metadata.Open(), [Text.UTF8Encoding]::new($false))
    try { foreach ($line in $provenance) { $writer.WriteLine($line) } }
    finally { $writer.Dispose() }
    $bundleArchive.Dispose()
    $bundleArchive = $null
    $bundleStream = $null
    $archive = Join-Path $OutputDirectory 'JamesDSP-windows-source.zip'
    Move-Item -LiteralPath $bundle -Destination $archive -Force
    Write-Host "Complete corresponding source: $archive"
}
catch {
    Write-Error -Message $_ -ErrorAction Continue
    exit 1
}
finally {
    if ($null -ne $bundleArchive) { $bundleArchive.Dispose() }
    if ($null -ne $bundleStream) { $bundleStream.Dispose() }
    if ($null -ne $stage -and (Test-Path -LiteralPath $stage)) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
