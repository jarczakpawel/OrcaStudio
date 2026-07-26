param(
    [string]$PackageDir = "",
    [string]$DistroName = "",
    [string]$InstallDir = "",
    [string]$ComponentCacheDir = "",
    [switch]$AllowMissingComponent,
    [switch]$SkipProbe,
    [switch]$PackageOnly
)

$ErrorActionPreference = 'Stop'
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $script:__slicer_runtime_prev_native_pref = $PSNativeCommandUseErrorActionPreference
    $PSNativeCommandUseErrorActionPreference = $false
}

function Get-ScriptDir {
    if (-not [string]::IsNullOrWhiteSpace($PSScriptRoot)) {
        return $PSScriptRoot
    }
    if (-not [string]::IsNullOrWhiteSpace($PSCommandPath)) {
        return (Split-Path -Parent $PSCommandPath)
    }
    if ($MyInvocation.MyCommand -and -not [string]::IsNullOrWhiteSpace($MyInvocation.MyCommand.Path)) {
        return (Split-Path -Parent $MyInvocation.MyCommand.Path)
    }
    return (Get-Location).Path
}


function Resolve-WslExecutable {
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:WINDIR)) {
        $candidates += (Join-Path $env:WINDIR 'System32\wsl.exe')
        $candidates += (Join-Path $env:WINDIR 'Sysnative\wsl.exe')
    }

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    $command = Get-Command wsl.exe -ErrorAction SilentlyContinue
    if ($command -and -not [string]::IsNullOrWhiteSpace($command.Source) -and (Test-Path $command.Source)) {
        return $command.Source
    }

    throw @'
Windows Subsystem for Linux (wsl.exe) was not found.
Open PowerShell as Administrator and run:
  wsl --install
Restart Windows, then verify:
  where.exe wsl
  wsl --status
  wsl -l -v
If wsl.exe is still unavailable, run:
  dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
  dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
Then restart Windows and start OrcaStudio again.
'@
}

function Convert-FileToLf([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or !(Test-Path $Path)) {
        return
    }

    $content = [System.IO.File]::ReadAllText($Path)
    $content = $content.Replace("`r`n", "`n").Replace("`r", "`n")
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $content, $utf8NoBom)
}

function To-WslPath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    if ($full.Length -ge 2 -and $full[1] -eq ':') {
        $drive = $full.Substring(0, 1).ToLowerInvariant()
        $tail = ($full.Substring(2) -replace '\\', '/')
        if ($tail.StartsWith('/')) {
            $tail = $tail.Substring(1)
        }
        return "/mnt/$drive/$tail"
    }
    return ($full -replace '\\', '/')
}

function Read-TextAuto([string]$Path) {
    if (!(Test-Path $Path)) {
        return ''
    }

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -eq 0) {
        return ''
    }

    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        return ([System.Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2) -replace "`0", '')
    }
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF) {
        return ([System.Text.Encoding]::BigEndianUnicode.GetString($bytes, 2, $bytes.Length - 2) -replace "`0", '')
    }
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        return ([System.Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3) -replace "`0", '')
    }

    for ($i = 1; $i -lt [Math]::Min($bytes.Length, 64); $i += 2) {
        if ($bytes[$i] -eq 0) {
            return ([System.Text.Encoding]::Unicode.GetString($bytes) -replace "`0", '')
        }
    }

    return ([System.Text.Encoding]::UTF8.GetString($bytes) -replace "`0", '')
}

function Normalize-NativeText([string]$Text) {
    if ([string]::IsNullOrEmpty($Text)) {
        return ''
    }
    $value = $Text -replace "`0", ''
    $value = $value -replace "`r`n", "`n"
    $value = $value -replace "`r", "`n"
    return $value
}


function Get-FileSha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Assert-RuntimeManifest([string]$Dir) {
    $manifestPath = Join-Path $Dir 'runtime-files.sha256'
    if (!(Test-Path $manifestPath)) {
        throw 'Missing package file: runtime-files.sha256'
    }
    $checked = 0
    foreach ($line in Get-Content -LiteralPath $manifestPath) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') {
            throw "Invalid runtime manifest line: $line"
        }
        $expected = $matches[1].ToLowerInvariant()
        $relative = $matches[2]
        if ($relative -match '[\\/]' -or $relative -eq '.' -or $relative -eq '..') {
            throw "Unsafe runtime manifest path: $relative"
        }
        $path = Join-Path $Dir $relative
        if (!(Test-Path -LiteralPath $path)) {
            throw "Runtime manifest file missing: $relative"
        }
        $actual = Get-FileSha256 $path
        if ($actual -ne $expected) {
            throw "Runtime manifest hash mismatch: $relative"
        }
        $checked++
    }
    if ($checked -lt 10) {
        throw "Runtime manifest contains too few files: $checked"
    }
}

function Assert-FileMagic([string]$Path, [byte[]]$Expected, [string]$Description) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt $Expected.Length) {
        throw "$Description is truncated: $Path"
    }
    for ($i = 0; $i -lt $Expected.Length; $i++) {
        if ($bytes[$i] -ne $Expected[$i]) {
            throw "$Description has invalid binary format: $Path"
        }
    }
}

function Assert-PosixScript([string]$Path, [string]$Description) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 10) {
        throw "$Description is empty or truncated: $Path"
    }
    if (($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) -or
        ($bytes.Length -ge 2 -and (($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) -or ($bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF)))) {
        throw "$Description must be UTF-8 without BOM: $Path"
    }
    if ($bytes -contains 0x00) {
        throw "$Description contains NUL bytes: $Path"
    }
    if ($bytes -contains 0x0D) {
        throw "$Description contains CR/CRLF line endings: $Path"
    }
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    if (-not ($text.StartsWith("#!/bin/sh`n") -or $text.StartsWith("#!/usr/bin/env bash`n"))) {
        throw "$Description has an invalid POSIX shell shebang: $Path"
    }
}


function Test-CaBundle([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or !(Test-Path $Path)) {
        return $false
    }

    $item = Get-Item $Path
    if ($item.Length -lt 65536) {
        return $false
    }

    $text = [System.IO.File]::ReadAllText($Path)
    $matches = [regex]::Matches($text, '-----BEGIN CERTIFICATE-----')
    return $matches.Count -ge 50
}


function ConvertTo-NormalizedRootfsTarEntry {
    param([AllowEmptyString()][string]$Name)

    $normalized = ([string]$Name) -replace '[\r\n]+$', ''
    while ($normalized.StartsWith('./', [System.StringComparison]::Ordinal)) {
        $normalized = $normalized.Substring(2)
    }
    $normalized = $normalized.TrimStart([char[]]@('/'))
    $normalized = $normalized.TrimEnd([char[]]@('/'))
    return $normalized
}

function Get-RootfsTarEntryMap {
    param(
        [Parameter(Mandatory = $true)][string]$TarPath,
        [Parameter(Mandatory = $true)][string]$TarExecutable
    )

    $entries = @(& $TarExecutable -tf $TarPath 2>$null)
    if ($LASTEXITCODE -ne 0 -or $entries.Count -eq 0) {
        throw "Invalid or empty WSL rootfs tar: $TarPath"
    }

    $entryMap = [System.Collections.Generic.Dictionary[string,string]]::new([System.StringComparer]::Ordinal)
    foreach ($entry in $entries) {
        $raw = [string]$entry
        $normalized = ConvertTo-NormalizedRootfsTarEntry $raw
        if (-not [string]::IsNullOrWhiteSpace($normalized) -and -not $entryMap.ContainsKey($normalized)) {
            $entryMap.Add($normalized, $raw)
        }
    }
    return ,$entryMap
}


function Read-RootfsTarTextMember {
    param(
        [Parameter(Mandatory = $true)][string]$TarPath,
        [Parameter(Mandatory = $true)][string]$TarExecutable,
        [Parameter(Mandatory = $true)]$EntryMap,
        [Parameter(Mandatory = $true)][string]$NormalizedName
    )

    if (-not $EntryMap.ContainsKey($NormalizedName)) {
        throw "WSL rootfs is missing required archive member: $NormalizedName"
    }
    $text = (& $TarExecutable -xOf $TarPath $EntryMap[$NormalizedName] 2>$null | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to read WSL rootfs archive member: $NormalizedName"
    }
    return $text
}

function Get-RootfsRuntimeManifest {
    param(
        [Parameter(Mandatory = $true)][string]$TarPath,
        [Parameter(Mandatory = $true)][string]$TarExecutable,
        [Parameter(Mandatory = $true)]$EntryMap
    )

    $markerName = 'etc/orcastudio-linux-auth-runtime'
    $manifestName = 'etc/orcastudio-linux-auth-runtime.manifest'
    $marker = (Read-RootfsTarTextMember -TarPath $TarPath -TarExecutable $TarExecutable -EntryMap $EntryMap -NormalizedName $markerName).Trim()
    if ($marker -ne 'ubuntu-24.04-linux-auth-v3') {
        $printableMarker = $marker -replace '[\r\n]+', ''
        throw "WSL rootfs marker is missing or invalid: '$printableMarker'"
    }

    $manifestText = Read-RootfsTarTextMember -TarPath $TarPath -TarExecutable $TarExecutable -EntryMap $EntryMap -NormalizedName $manifestName
    $manifest = [System.Collections.Generic.Dictionary[string,string]]::new([System.StringComparer]::Ordinal)
    foreach ($rawLine in ($manifestText -split '\r?\n')) {
        $line = $rawLine.Trim()
        if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('#')) { continue }
        $parts = $line -split '=', 2
        if ($parts.Count -ne 2 -or [string]::IsNullOrWhiteSpace($parts[0]) -or [string]::IsNullOrWhiteSpace($parts[1])) {
            throw "Invalid WSL rootfs manifest line: $rawLine"
        }
        if ($manifest.ContainsKey($parts[0])) {
            throw "Duplicate WSL rootfs manifest key: $($parts[0])"
        }
        $manifest.Add($parts[0], $parts[1])
    }

    if (-not $manifest.ContainsKey('schema') -or $manifest['schema'] -ne '1') {
        throw "Unsupported WSL rootfs manifest schema: '$($manifest['schema'])'"
    }
    if (-not $manifest.ContainsKey('marker') -or $manifest['marker'] -ne 'ubuntu-24.04-linux-auth-v3') {
        throw "WSL rootfs manifest marker is missing or invalid: '$($manifest['marker'])'"
    }

    foreach ($key in @('ca', 'xvfb', 'xvfb_run', 'xdpyinfo', 'xkbcomp', 'xkb_data', 'x11vnc', 'websockify', 'python3', 'browser', 'novnc')) {
        if (-not $manifest.ContainsKey($key)) {
            throw "WSL rootfs manifest is missing key: $key"
        }
        $rawPath = $manifest[$key]
        if (-not $rawPath.StartsWith('/')) {
            throw "WSL rootfs manifest path is not absolute: $key=$rawPath"
        }
        $normalizedPath = ConvertTo-NormalizedRootfsTarEntry $rawPath
        if (-not $EntryMap.ContainsKey($normalizedPath)) {
            throw "WSL rootfs manifest path is missing from archive: $key=$rawPath"
        }
    }

    $caName = ConvertTo-NormalizedRootfsTarEntry $manifest['ca']
    $caText = Read-RootfsTarTextMember -TarPath $TarPath -TarExecutable $TarExecutable -EntryMap $EntryMap -NormalizedName $caName
    if ($caText.Length -lt 65536 -or ([regex]::Matches($caText, '-----BEGIN CERTIFICATE-----')).Count -lt 50) {
        throw 'WSL rootfs CA certificate bundle is missing or incomplete'
    }

    return ,$manifest
}

function Repair-CaBundleFromRootFs([string]$Dir) {
    $caPath = Join-Path $Dir 'ca-certificates.crt'
    if (Test-CaBundle $caPath) {
        return
    }

    $rootFsPath = Join-Path $Dir 'windows-wsl2-rootfs.tar'
    if (!(Test-Path $rootFsPath)) {
        throw 'CA bundle is missing or invalid and windows-wsl2-rootfs.tar is unavailable for recovery'
    }

    $tarCommand = Get-Command tar.exe -ErrorAction SilentlyContinue
    if (-not $tarCommand) {
        $tarCommand = Get-Command tar -ErrorAction SilentlyContinue
    }
    if (-not $tarCommand) {
        throw 'CA bundle is missing or invalid and tar is unavailable for recovery'
    }

    $entryMap = Get-RootfsTarEntryMap -TarPath $rootFsPath -TarExecutable $tarCommand.Source
    $caEntryName = 'etc/ssl/certs/ca-certificates.crt'
    if (-not $entryMap.ContainsKey($caEntryName)) {
        throw 'The bundled WSL rootfs does not contain the CA certificate bundle entry'
    }

    $tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ('orcastudio-ca-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
    try {
        $rawEntry = $entryMap[$caEntryName]
        & $tarCommand.Source -xf $rootFsPath -C $tempDir $rawEntry 2>$null
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to extract CA certificate bundle member: $rawEntry"
        }

        $recovered = Join-Path $tempDir 'etc/ssl/certs/ca-certificates.crt'
        if (!(Test-CaBundle $recovered)) {
            throw 'The bundled WSL rootfs does not contain a valid CA certificate bundle'
        }

        Copy-Item -Force $recovered $caPath
        Write-Host "Recovered CA bundle from windows-wsl2-rootfs.tar: $caPath"
    } finally {
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $tempDir
    }

    if (!(Test-CaBundle $caPath)) {
        throw 'Failed to recover a valid CA certificate bundle'
    }
}

function Get-RootFsHashMarkerPath([string]$Dir) {
    return (Join-Path $Dir 'slicer-linux-runtime-rootfs-sha256.txt')
}

function Read-RootFsHashMarker([string]$Dir) {
    if ([string]::IsNullOrWhiteSpace($Dir)) {
        return ''
    }
    $path = Get-RootFsHashMarkerPath $Dir
    if (!(Test-Path $path)) {
        return ''
    }
    return ((Get-Content $path -Raw).Trim().ToLowerInvariant())
}

function Invoke-NativeCapture([string]$FilePath, [string[]]$ArgumentList) {
    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()
    try {
        & $FilePath @ArgumentList 1> $stdoutPath 2> $stderrPath
        $exitCode = $LASTEXITCODE
        $stdoutText = if (Test-Path $stdoutPath) { Normalize-NativeText (Read-TextAuto $stdoutPath) } else { '' }
        $stderrText = if (Test-Path $stderrPath) { Normalize-NativeText (Read-TextAuto $stderrPath) } else { '' }
        $combined = (($stdoutText + "`n" + $stderrText).Trim())
        return @{
            ExitCode = $exitCode
            StdOut = $stdoutText
            StdErr = $stderrText
            Combined = $combined
        }
    } finally {
        Remove-Item -Force -ErrorAction SilentlyContinue $stdoutPath, $stderrPath
    }
}


function Invoke-WslRootCapture(
    [string]$WslPath,
    [string]$Name,
    [string[]]$CommandArgumentList
) {
    if ([string]::IsNullOrWhiteSpace($WslPath)) {
        throw 'Missing wsl.exe path'
    }
    if ([string]::IsNullOrWhiteSpace($Name)) {
        throw 'Missing WSL distro name'
    }
    if ($null -eq $CommandArgumentList -or $CommandArgumentList.Count -eq 0) {
        throw 'Missing Linux command'
    }

    [string[]]$wslArgs = @('-d', $Name, '--user', 'root', '--')
    $wslArgs += $CommandArgumentList
    return (Invoke-NativeCapture $WslPath $wslArgs)
}

function Assert-WslRootCommand(
    [string]$WslPath,
    [string]$Name,
    [string]$Description,
    [string[]]$CommandArgumentList
) {
    $result = Invoke-WslRootCapture $WslPath $Name $CommandArgumentList
    if ($result.ExitCode -ne 0) {
        $details = $result.Combined
        if ([string]::IsNullOrWhiteSpace($details)) {
            $details = "process exited with code $($result.ExitCode)"
        }
        throw ("{0}: {1}" -f $Description, $details)
    }
    return $result
}

function Get-WslRuntimeDiagnostics([string]$WslPath, [string]$Name) {
    $sections = @()

    foreach ($item in @(
        @{ Label = 'kernel'; Args = @('uname', '-a') },
        @{ Label = 'identity'; Args = @('id') },
        @{ Label = 'page_size'; Args = @('getconf', 'PAGESIZE') },
        @{ Label = 'overcommit_memory'; Args = @('cat', '/proc/sys/vm/overcommit_memory') },
        @{ Label = 'overcommit_ratio'; Args = @('cat', '/proc/sys/vm/overcommit_ratio') },
        @{ Label = 'memory'; Args = @('grep', '-E', '^(MemTotal|MemAvailable|SwapTotal|SwapFree):', '/proc/meminfo') },
        @{ Label = 'webkit_packages'; Args = @('dpkg-query', '-W', 'libwebkit2gtk-4.1-0', 'libjavascriptcoregtk-4.1-0') }
    )) {
        $result = Invoke-WslRootCapture $WslPath $Name ([string[]]($item.Args))
        $details = $result.Combined
        if ([string]::IsNullOrWhiteSpace($details)) {
            $details = "exit=$($result.ExitCode)"
        }
        $sections += ("{0}: {1}" -f $item.Label, $details)
    }

    return ($sections -join "`n")
}

function Get-FreeLoopbackPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    try {
        $listener.Start()
        return ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    } finally {
        $listener.Stop()
    }
}

function Test-WslDistroExists([string]$WslPath, [string]$Name) {
    $probe = Invoke-WslRootCapture $WslPath $Name @('true')
    if ($probe.ExitCode -eq 0) {
        return @{
            Exists = $true
            Reason = ''
        }
    }

    $text = $probe.Combined
    $lower = $text.ToLowerInvariant()

    if ($lower.Contains('there is no distribution with the supplied name') -or
        $lower.Contains('wsl_e_distribution_not_found') -or
        $lower.Contains('wsl_e_distro_not_found') -or
        $lower.Contains('brak dystrybucji o podanej nazwie') -or
        ($lower.Contains('distribution') -and $lower.Contains('not') -and $lower.Contains('found'))) {
        return @{
            Exists = $false
            Reason = "WSL distro '$Name' is not installed"
        }
    }

    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "Failed to start WSL distro '$Name'"
    }

    throw ("Failed to start WSL distro '{0}': {1}" -f $Name, $text)
}

if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    $PackageDir = Get-ScriptDir
}
$PackageDir = [System.IO.Path]::GetFullPath($PackageDir)

if ([string]::IsNullOrWhiteSpace($ComponentCacheDir)) {
    if ($env:SLICER_LINUX_RUNTIME_WINDOWS_COMPONENT_CACHE_DIR) {
        $ComponentCacheDir = $env:SLICER_LINUX_RUNTIME_WINDOWS_COMPONENT_CACHE_DIR
    } else {
        $subdirFile = Join-Path $PackageDir 'slicer_linux_runtime_component_dir.txt'
        if ((-not [string]::IsNullOrWhiteSpace($env:APPDATA)) -and (Test-Path $subdirFile)) {
            $subdir = (Get-Content $subdirFile -Raw).Trim()
            if (-not [string]::IsNullOrWhiteSpace($subdir)) {
                $ComponentCacheDir = Join-Path $env:APPDATA $subdir
            }
        }
        if ([string]::IsNullOrWhiteSpace($ComponentCacheDir) -and $env:APPDATA) {
            $ComponentCacheDir = Join-Path $env:APPDATA 'BambuStudio_OrcaSlicer\ota\plugins'
        }
    }
}
if (-not [string]::IsNullOrWhiteSpace($ComponentCacheDir)) {
    $ComponentCacheDir = [System.IO.Path]::GetFullPath($ComponentCacheDir)
    $pluginsChild = Join-Path $ComponentCacheDir 'plugins'
    if ((Split-Path -Leaf $ComponentCacheDir) -ieq 'ota' -and (Test-Path $pluginsChild)) {
        $ComponentCacheDir = [System.IO.Path]::GetFullPath($pluginsChild)
    } elseif ((Test-Path $pluginsChild) -and !(Test-Path (Join-Path $ComponentCacheDir 'libbambu_networking.so')) -and !(Test-Path (Join-Path $ComponentCacheDir 'libBambuSource.so'))) {
        $ComponentCacheDir = [System.IO.Path]::GetFullPath($pluginsChild)
    }
}

if ([string]::IsNullOrWhiteSpace($DistroName)) {
    $distroFile = Join-Path $PackageDir 'slicer_linux_runtime_wsl_distro.txt'
    if (Test-Path $distroFile) {
        $DistroName = (Get-Content $distroFile -Raw).Trim()
    }
}
if ([string]::IsNullOrWhiteSpace($DistroName)) {
    throw 'Missing distro name. Set SLICER_LINUX_RUNTIME_WSL_DISTRO or provide slicer_linux_runtime_wsl_distro.txt.'
}

if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    $InstallDir = Join-Path $env:LOCALAPPDATA $DistroName
}
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)

Repair-CaBundleFromRootFs $PackageDir

$requiredFiles = @(
    'slicer_linux_runtime.dll',
    'slicer_linux_runtime_wsl_distro.txt',
    'install_runtime.ps1',
    'verify_runtime.ps1',
    'slicer_linux_runtime_host',
    'slicer_linux_runtime_host_abi1',
    'slicer_linux_runtime_host_abi0',
    'slicer_linux_auth_browser',
    'run_auth_browser.sh',
    'windows-wsl2-rootfs.tar',
    'runtime-files.sha256',
    'ca-certificates.crt',
    'slicer_base64.cer'
)

foreach ($name in $requiredFiles) {
    $path = Join-Path $PackageDir $name
    if (!(Test-Path $path)) {
        throw "Missing package file: $name"
    }
}

if (!(Test-CaBundle (Join-Path $PackageDir 'ca-certificates.crt'))) {
    throw 'Invalid CA certificate bundle: ca-certificates.crt'
}
Assert-RuntimeManifest $PackageDir

$bootstrapPath = Join-Path $PackageDir 'slicer_linux_runtime_wsl_run_host.sh'
if (!(Test-Path $bootstrapPath)) {
    throw 'Missing package file: slicer_linux_runtime_wsl_run_host.sh'
}
Convert-FileToLf $bootstrapPath
Assert-PosixScript $bootstrapPath 'WSL runtime bootstrap script'
Assert-PosixScript (Join-Path $PackageDir 'slicer_linux_runtime_host') 'Linux runtime dispatcher script'
Assert-PosixScript (Join-Path $PackageDir 'run_auth_browser.sh') 'Linux authentication launcher script'

if ($PackageOnly) {
    foreach ($name in $requiredFiles + @('slicer_linux_runtime_wsl_run_host.sh')) {
        $path = Join-Path $PackageDir $name
        if ((Get-Item $path).Length -le 0) {
            throw "Package file is empty: $name"
        }
    }

    Assert-FileMagic (Join-Path $PackageDir 'slicer_linux_runtime.dll') ([byte[]](0x4D, 0x5A)) 'Windows forwarder DLL'
    foreach ($name in @('slicer_linux_runtime_host_abi1', 'slicer_linux_runtime_host_abi0', 'slicer_linux_auth_browser')) {
        Assert-FileMagic (Join-Path $PackageDir $name) ([byte[]](0x7F, 0x45, 0x4C, 0x46)) "Linux runtime binary $name"
    }

    $tarCommand = Get-Command tar.exe -ErrorAction SilentlyContinue
    if (-not $tarCommand) {
        $tarCommand = Get-Command tar -ErrorAction SilentlyContinue
    }
    if (-not $tarCommand) {
        throw 'tar executable is required for rootfs package validation'
    }

    $rootFsPath = Join-Path $PackageDir 'windows-wsl2-rootfs.tar'
    $entryMap = Get-RootfsTarEntryMap -TarPath $rootFsPath -TarExecutable $tarCommand.Source
    $null = Get-RootfsRuntimeManifest -TarPath $rootFsPath -TarExecutable $tarCommand.Source -EntryMap $entryMap

    Write-Host 'Slicer Linux runtime package-only verification OK'
    exit 0
}

$wsl = Resolve-WslExecutable

$distroStatus = Test-WslDistroExists $wsl $DistroName
if (-not $distroStatus.Exists) {
    throw $distroStatus.Reason
}


$rootFsPath = Join-Path $PackageDir 'windows-wsl2-rootfs.tar'
$expectedRootFsHash = Get-FileSha256 $rootFsPath
$storedRootFsHash = Read-RootFsHashMarker $InstallDir
if ([string]::IsNullOrWhiteSpace($storedRootFsHash)) {
    throw "WSL runtime rootfs marker missing for '$DistroName'; reinstall required"
}
if ($storedRootFsHash -ne $expectedRootFsHash) {
    throw "WSL runtime rootfs marker out of date for '$DistroName'; reinstall required"
}

$packageDirWsl = To-WslPath $PackageDir
$pluginCacheDirWsl = ""
if (-not [string]::IsNullOrWhiteSpace($ComponentCacheDir)) {
    $pluginCacheDirWsl = To-WslPath $ComponentCacheDir
}
$bootstrapWsl = "$packageDirWsl/$([System.IO.Path]::GetFileName($bootstrapPath))"
$authBrowserWsl = "$packageDirWsl/slicer_linux_auth_browser"

Write-Host "Runtime package dir: $PackageDir"
Write-Host "Component cache dir: $ComponentCacheDir"
Write-Host "WSL distro: $DistroName"
Write-Host "WSL install dir: $InstallDir"
Write-Host "Bootstrap script: $bootstrapPath"

if ($SkipProbe) {
    Write-Host 'WSL runtime core OK'
    exit 0
}

$authMarker = Assert-WslRootCommand $wsl $DistroName 'WSL Linux authentication runtime marker verification failed' @('cat', '/etc/orcastudio-linux-auth-runtime')
if ($authMarker.StdOut.Trim() -ne 'ubuntu-24.04-linux-auth-v3') {
    throw "WSL Linux authentication runtime marker is invalid: $($authMarker.StdOut.Trim())"
}

foreach ($path in @(
    '/usr/bin/Xvfb',
    '/usr/bin/xvfb-run',
    '/usr/bin/xdpyinfo',
    '/usr/bin/xkbcomp',
    '/usr/bin/x11vnc',
    '/usr/bin/websockify',
    '/usr/bin/timeout'
)) {
    $null = Assert-WslRootCommand $wsl $DistroName "Missing WSL Linux authentication executable: $path" @('test', '-x', $path)
}

$epiphany = Invoke-WslRootCapture $wsl $DistroName @('test', '-x', '/usr/bin/epiphany')
if ($epiphany.ExitCode -ne 0) {
    $epiphany = Invoke-WslRootCapture $wsl $DistroName @('test', '-x', '/usr/bin/epiphany-browser')
}
if ($epiphany.ExitCode -ne 0) {
    throw 'Missing WSL Linux authentication executable: epiphany'
}

foreach ($path in @(
    '/usr/share/X11/xkb/rules/evdev',
    '/usr/share/novnc/vnc.html'
)) {
    $null = Assert-WslRootCommand $wsl $DistroName "Missing WSL Linux authentication file: $path" @('test', '-f', $path)
}

$null = Assert-WslRootCommand $wsl $DistroName 'Failed to mark WSL Linux authentication browser executable' @('chmod', '755', $authBrowserWsl)
$authPrerequisite = Assert-WslRootCommand $wsl $DistroName 'WSL Linux authentication browser prerequisite verification failed' @($authBrowserWsl, '--probe')

[string[]]$authSelfTestCommand = @(
    'env',
    '-u', 'LD_LIBRARY_PATH',
    '-u', 'LD_PRELOAD',
    '-u', 'GIGACAGE_ENABLED',
    'GDK_BACKEND=x11',
    'LIBGL_ALWAYS_SOFTWARE=1',
    'WEBKIT_DISABLE_COMPOSITING_MODE=1',
    'WEBKIT_DISABLE_DMABUF_RENDERER=1',
    'timeout', '30',
    'xvfb-run', '-a', '-e', '/dev/stderr',
    $authBrowserWsl, '--self-test'
)
$authSelfTest = Invoke-WslRootCapture $wsl $DistroName $authSelfTestCommand
if ($authSelfTest.ExitCode -ne 0) {
    Start-Sleep -Milliseconds 500
    $authSelfTestRetry = Invoke-WslRootCapture $wsl $DistroName $authSelfTestCommand
    if ($authSelfTestRetry.ExitCode -eq 0) {
        $authSelfTest = $authSelfTestRetry
    } else {
        $authSelfTest = @{
            ExitCode = $authSelfTestRetry.ExitCode
            StdOut = $authSelfTestRetry.StdOut
            StdErr = $authSelfTestRetry.StdErr
            Combined = (($authSelfTest.Combined + "`nretry:`n" + $authSelfTestRetry.Combined).Trim())
        }
    }
}

$authGigacageFallback = $null
if ($authSelfTest.ExitCode -ne 0) {
    [string[]]$authGigacageFallbackCommand = @(
        'env',
        '-u', 'LD_LIBRARY_PATH',
        '-u', 'LD_PRELOAD',
        'GDK_BACKEND=x11',
        'LIBGL_ALWAYS_SOFTWARE=1',
        'WEBKIT_DISABLE_COMPOSITING_MODE=1',
        'WEBKIT_DISABLE_DMABUF_RENDERER=1',
        'GIGACAGE_ENABLED=0',
        'timeout', '30',
        'xvfb-run', '-a', '-e', '/dev/stderr',
        $authBrowserWsl, '--self-test'
    )
    $authGigacageFallbackFirst = Invoke-WslRootCapture $wsl $DistroName $authGigacageFallbackCommand
    if ($authGigacageFallbackFirst.ExitCode -eq 0) {
        Start-Sleep -Milliseconds 500
        $authGigacageFallbackSecond = Invoke-WslRootCapture $wsl $DistroName $authGigacageFallbackCommand
        if ($authGigacageFallbackSecond.ExitCode -eq 0) {
            $authGigacageFallback = $authGigacageFallbackSecond
        } else {
            $authGigacageFallback = @{
                ExitCode = $authGigacageFallbackSecond.ExitCode
                StdOut = $authGigacageFallbackSecond.StdOut
                StdErr = $authGigacageFallbackSecond.StdErr
                Combined = (($authGigacageFallbackFirst.Combined + "`nsecond fallback test:`n" + $authGigacageFallbackSecond.Combined).Trim())
            }
        }
    } else {
        $authGigacageFallback = $authGigacageFallbackFirst
    }
}

if ($authSelfTest.ExitCode -eq 0) {
    $authMarkerUpdate = Invoke-WslRootCapture $wsl $DistroName @('rm', '-f', '/etc/orcastudio-auth-disable-gigacage')
    if ($authMarkerUpdate.ExitCode -ne 0) {
        throw "Failed to clear WSL authentication compatibility marker: $($authMarkerUpdate.Combined)"
    }
    Write-Host 'WSL Linux authentication browser OK'
    Write-Host $authPrerequisite.Combined
    Write-Host $authSelfTest.Combined
} elseif ($null -ne $authGigacageFallback -and $authGigacageFallback.ExitCode -eq 0) {
    $authMarkerUpdate = Invoke-WslRootCapture $wsl $DistroName @('touch', '/etc/orcastudio-auth-disable-gigacage')
    if ($authMarkerUpdate.ExitCode -ne 0) {
        throw "Failed to persist WSL authentication compatibility marker: $($authMarkerUpdate.Combined)"
    }
    $authMarkerMode = Invoke-WslRootCapture $wsl $DistroName @('chmod', '644', '/etc/orcastudio-auth-disable-gigacage')
    if ($authMarkerMode.ExitCode -ne 0) {
        throw "Failed to set WSL authentication compatibility marker permissions: $($authMarkerMode.Combined)"
    }
    Write-Host 'WSL Linux authentication browser OK with verified Gigacage compatibility fallback'
    Write-Host $authPrerequisite.Combined
    Write-Host $authGigacageFallback.Combined
} else {
    $authDiagnostic = Get-WslRuntimeDiagnostics $wsl $DistroName
    $normalDetails = $authSelfTest.Combined
    if ([string]::IsNullOrWhiteSpace($normalDetails)) {
        $normalDetails = "process exited with code $($authSelfTest.ExitCode)"
    }
    $fallbackDetails = if ($null -eq $authGigacageFallback) {
        'not executed'
    } elseif ([string]::IsNullOrWhiteSpace($authGigacageFallback.Combined)) {
        "process exited with code $($authGigacageFallback.ExitCode)"
    } else {
        $authGigacageFallback.Combined
    }
    throw "WSL Linux authentication browser verification failed. Normal test: $normalDetails`nGigacage fallback: $fallbackDetails`nRuntime diagnostics: $authDiagnostic"
}

$hostNoVncPort = Get-FreeLoopbackPort
$vncPort = Get-FreeLoopbackPort
while ($vncPort -eq $hostNoVncPort) {
    $vncPort = Get-FreeLoopbackPort
}

$probe = Invoke-NativeCapture $wsl @('-d', $DistroName, '--user', 'root', '--', 'sh', $bootstrapWsl, '--probe', $packageDirWsl, $pluginCacheDirWsl, "$hostNoVncPort", "$hostNoVncPort", "$vncPort")
if ($probe.ExitCode -ne 0) {
    $probeText = $probe.Combined
    if ($AllowMissingComponent -and $probeText -match 'component_not_downloaded') {
        Write-Host 'WSL runtime package OK, linux component not downloaded yet.'
        Write-Host $probeText
        exit 0
    }
    throw "WSL runtime probe failed: $probeText"
}

Write-Host 'WSL runtime probe OK'
Write-Host $probe.Combined
