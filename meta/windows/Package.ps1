# Stage only the Release exe, deployed runtime, integration, and license notices.
[CmdletBinding()]
param(
    [string]$QtDir = $env:QTDIR,
    [string]$BuildDirectory,
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$stage = $null

function Copy-LicenseFiles([string]$Root, [string]$Destination, [switch]$Recurse) {
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { return }
    foreach ($file in Get-ChildItem -LiteralPath $Root -File -Recurse:$Recurse) {
        if ($file.Name -notmatch '^(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE)([._-].*)?$') { continue }
        $relative = $file.FullName.Substring($Root.TrimEnd('\', '/').Length + 1)
        $target = Join-Path $Destination $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target
    }
}

function Export-SourceNotices([string]$Root, [string]$Destination) {
    # Several bundled single-header libraries keep their entire license in comments.
    # Preserve these verbatim, including end-of-file dual-license notices.
    $notices = New-Object Text.StringBuilder
    foreach ($file in Get-ChildItem -LiteralPath $Root -File -Recurse | Sort-Object FullName) {
        if ($file.Extension -notin '.c', '.cpp', '.h', '.hpp') { continue }
        $content = [IO.File]::ReadAllText($file.FullName)
        $comments = [regex]::Matches($content, '(?s)/\*.*?\*/|(?m)(?:^[ \t]*//[^\r\n]*(?:\r?\n|$))+')
        foreach ($comment in $comments) {
            if ($comment.Value -notmatch '(?i)copyright|permission is (hereby )?granted|redistribution and use|this software is available as a choice|public domain|SPDX-License-Identifier') { continue }
            $relative = $file.FullName.Substring($Root.TrimEnd('\', '/').Length + 1)
            [void]$notices.AppendLine("===== $relative =====")
            [void]$notices.AppendLine($comment.Value)
            [void]$notices.AppendLine()
        }
    }
    if ($notices.Length -gt 0) { [IO.File]::WriteAllText($Destination, $notices.ToString(), (New-Object Text.UTF8Encoding($false))) }
}

try {
    . (Join-Path $PSScriptRoot 'Environment.ps1')
    $QtDir = Resolve-QtDirectory $QtDir
    $deploy = Join-Path $QtDir 'bin/windeployqt.exe'
    if (-not (Test-Path -LiteralPath $deploy -PathType Leaf)) { throw "windeployqt.exe not found: $deploy" }
    if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $PSScriptRoot '../../../build' }
    $BuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $executable = Join-Path $BuildDirectory 'src/release/jamesdsp.exe'
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Build the Release executable first: $executable" }
    $repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $repository '../dist' }
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $archive = Join-Path $OutputDirectory 'JamesDSP-windows-x64.zip'
    # Always use a brand-new staging folder. Never copy or publish a build/release tree.
    $stage = Join-Path ([IO.Path]::GetTempPath()) ('JamesDSP-package-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage | Out-Null
    $stagedExe = Join-Path $stage 'jamesdsp.exe'
    Copy-Item -LiteralPath $executable -Destination $stagedExe
    Initialize-MsvcEnvironment
    $env:PATH = (Join-Path $QtDir 'bin') + ';' + $env:PATH
    # QWidget/QPainter rendering does not need the separate Mesa/LLVM software GL binary.
    & $deploy --release --compiler-runtime --no-opengl-sw --dir $stage $stagedExe
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE." }
    if (-not (Test-Path -LiteralPath (Join-Path $stage 'platforms/qwindows.dll') -PathType Leaf)) {
        throw 'windeployqt did not deploy the Windows platform plugin.'
    }
    # Qt 5.15 deploys an MSVC redistributable installer for Release builds.
    # Use the official app-local CRT DLLs instead, keeping the ZIP portable.
    if ([string]::IsNullOrWhiteSpace($env:VCToolsRedistDir)) {
        throw 'vcvars64 did not provide VCToolsRedistDir for app-local compiler runtime deployment.'
    }
    $crtDirectories = @(Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT')
    if ($crtDirectories.Count -ne 1) { throw 'Expected exactly one x64 Microsoft.VC*.CRT redistributable directory.' }
    foreach ($dll in Get-ChildItem -LiteralPath $crtDirectories[0].FullName -File -Filter '*.dll') {
        Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $stage $dll.Name)
    }
    foreach ($requiredDll in 'msvcp140.dll', 'vcruntime140.dll') {
        if (-not (Test-Path -LiteralPath (Join-Path $stage $requiredDll) -PathType Leaf)) {
            throw "App-local compiler runtime was not deployed: $requiredDll"
        }
    }
    foreach ($installer in 'vc_redist.x64.exe', 'vcredist_x64.exe') {
        $installerPath = Join-Path $stage $installer
        if (Test-Path -LiteralPath $installerPath -PathType Leaf) { Remove-Item -LiteralPath $installerPath }
    }
    foreach ($name in 'Register-Application.ps1', 'Unregister-Application.ps1', 'Install.cmd', 'NOTICE.txt') {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $stage $name)
    }
    Copy-Item -LiteralPath (Join-Path $repository 'LICENSE') -Destination (Join-Path $stage 'LICENSE')
    $licenses = Join-Path $stage 'licenses'
    New-Item -ItemType Directory -Path $licenses | Out-Null
    foreach ($component in '3rdparty', 'src/subprojects', 'libjamesdsp/subtree/Main') {
        $componentRoot = Join-Path $repository $component
        Copy-LicenseFiles $componentRoot (Join-Path $licenses $component) -Recurse
        Export-SourceNotices $componentRoot (Join-Path $licenses (($component -replace '[/\\]', '-') + '-source-notices.txt'))
    }
    # Qt installers/aqt distributions vary in the location of their legal texts.
    Copy-LicenseFiles $QtDir (Join-Path $licenses 'Qt')
    foreach ($legalDirectory in 'licenses', 'doc/global', '../Licenses', '../../Licenses') {
        $legalRoot = [IO.Path]::GetFullPath((Join-Path $QtDir $legalDirectory))
        Copy-LicenseFiles $legalRoot (Join-Path $licenses ('Qt/' + ($legalDirectory -replace '[/\\.]', '_'))) -Recurse
    }
    # Exact tagged Qt 5.15.2 manifests, copyright notices, and referenced license texts.
    $qtNotices = Join-Path $PSScriptRoot 'Qt-5.15.2-notices'
    if (-not (Test-Path -LiteralPath (Join-Path $qtNotices 'PROVENANCE.txt') -PathType Leaf)) {
        throw 'The checked-in Qt 5.15.2 license notice collection is missing.'
    }
    Copy-Item -LiteralPath $qtNotices -Destination (Join-Path $licenses 'Qt-5.15.2-notices') -Recurse
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -CompressionLevel Optimal -Force
    Write-Host "Portable package: $archive"
    Write-Host 'Extract the complete ZIP to a permanent folder, then run Install.cmd for Windows Search registration.'
}
catch {
    Write-Error -Message $_ -ErrorAction Continue
    exit 1
}
finally {
    if ($null -ne $stage -and (Test-Path -LiteralPath $stage)) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
