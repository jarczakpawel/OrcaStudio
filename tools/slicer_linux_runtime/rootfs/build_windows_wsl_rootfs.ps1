param(
    [string]$OutputTar = "",
    [string]$BaseImage = "ubuntu:24.04",
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$rootfsMarker = 'ubuntu-24.04-linux-auth-v3'

function Get-ScriptDir {
    if (-not [string]::IsNullOrWhiteSpace($PSScriptRoot)) { return $PSScriptRoot }
    if (-not [string]::IsNullOrWhiteSpace($PSCommandPath)) { return (Split-Path -Parent $PSCommandPath) }
    if ($MyInvocation.MyCommand -and -not [string]::IsNullOrWhiteSpace($MyInvocation.MyCommand.Path)) {
        return (Split-Path -Parent $MyInvocation.MyCommand.Path)
    }
    return (Get-Location).Path
}

function Invoke-WithRetry {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][scriptblock]$Action
    )

    for ($attempt = 1; $attempt -le 5; $attempt++) {
        Write-Host "$Label attempt $attempt/5"
        try {
            & $Action
            return
        }
        catch {
            if ($attempt -eq 5) { throw }
        }
        if ($attempt -lt 5) { Start-Sleep -Seconds (15 * $attempt) }
    }
    throw "$Label failed after retries"
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

function Test-RootfsTar {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        Write-Warning "WSL rootfs archive does not exist: $Path"
        return $false
    }
    if ((Get-Item -LiteralPath $Path).Length -le 0) {
        Write-Warning "WSL rootfs archive is empty: $Path"
        return $false
    }

    $entries = @(& tar -tf $Path 2>$null)
    if ($LASTEXITCODE -ne 0 -or $entries.Count -eq 0) {
        Write-Warning "WSL rootfs archive cannot be listed: $Path"
        return $false
    }

    $entryMap = [System.Collections.Generic.Dictionary[string,string]]::new([System.StringComparer]::Ordinal)
    foreach ($entry in $entries) {
        $raw = [string]$entry
        $normalized = ConvertTo-NormalizedRootfsTarEntry $raw
        if (-not [string]::IsNullOrWhiteSpace($normalized) -and -not $entryMap.ContainsKey($normalized)) {
            $entryMap.Add($normalized, $raw)
        }
    }

    $markerName = 'etc/orcastudio-linux-auth-runtime'
    $manifestName = 'etc/orcastudio-linux-auth-runtime.manifest'
    foreach ($required in @($markerName, $manifestName)) {
        if (-not $entryMap.ContainsKey($required)) {
            Write-Warning "WSL rootfs is missing required archive member: $required"
            return $false
        }
    }

    $marker = (& tar -xOf $Path $entryMap[$markerName] 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $marker -ne $rootfsMarker) {
        $printableMarker = $marker -replace '[\r\n]+', ''
        Write-Warning "WSL rootfs runtime marker mismatch. Expected '$rootfsMarker', got '$printableMarker'."
        return $false
    }

    $manifestText = (& tar -xOf $Path $entryMap[$manifestName] 2>$null | Out-String)
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($manifestText)) {
        Write-Warning 'WSL rootfs runtime manifest is unreadable or empty.'
        return $false
    }

    $manifest = [System.Collections.Generic.Dictionary[string,string]]::new([System.StringComparer]::Ordinal)
    foreach ($rawLine in ($manifestText -split '\r?\n')) {
        $line = $rawLine.Trim()
        if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('#')) { continue }
        $parts = $line -split '=', 2
        if ($parts.Count -ne 2 -or [string]::IsNullOrWhiteSpace($parts[0]) -or [string]::IsNullOrWhiteSpace($parts[1])) {
            Write-Warning "Invalid WSL rootfs manifest line: $rawLine"
            return $false
        }
        if ($manifest.ContainsKey($parts[0])) {
            Write-Warning "Duplicate WSL rootfs manifest key: $($parts[0])"
            return $false
        }
        $manifest.Add($parts[0], $parts[1])
    }

    if (-not $manifest.ContainsKey('schema') -or $manifest['schema'] -ne '1') {
        Write-Warning "Unsupported WSL rootfs manifest schema: '$($manifest['schema'])'"
        return $false
    }
    if (-not $manifest.ContainsKey('marker') -or $manifest['marker'] -ne $rootfsMarker) {
        Write-Warning "WSL rootfs manifest marker mismatch: '$($manifest['marker'])'"
        return $false
    }

    $requiredPathKeys = @('ca', 'xvfb', 'xvfb_run', 'xdpyinfo', 'xkbcomp', 'xkb_data', 'x11vnc', 'websockify', 'python3', 'browser', 'novnc')
    foreach ($key in $requiredPathKeys) {
        if (-not $manifest.ContainsKey($key)) {
            Write-Warning "WSL rootfs manifest is missing key: $key"
            return $false
        }
        $rawPath = $manifest[$key]
        if (-not $rawPath.StartsWith('/')) {
            Write-Warning "WSL rootfs manifest path is not absolute: $key=$rawPath"
            return $false
        }
        $normalizedPath = ConvertTo-NormalizedRootfsTarEntry $rawPath
        if (-not $entryMap.ContainsKey($normalizedPath)) {
            Write-Warning "WSL rootfs manifest path is missing from archive: $key=$rawPath"
            return $false
        }
    }

    $caMember = $entryMap[(ConvertTo-NormalizedRootfsTarEntry $manifest['ca'])]
    $caText = (& tar -xOf $Path $caMember 2>$null | Out-String)
    if ($LASTEXITCODE -ne 0 -or $caText.Length -lt 65536 -or ([regex]::Matches($caText, '-----BEGIN CERTIFICATE-----')).Count -lt 50) {
        Write-Warning 'WSL rootfs CA certificate bundle is missing or incomplete.'
        return $false
    }

    return $true
}

$scriptDir = Get-ScriptDir
if ([string]::IsNullOrWhiteSpace($OutputTar)) {
    $OutputTar = Join-Path $scriptDir 'windows-wsl2-rootfs.tar'
}
$OutputTar = [System.IO.Path]::GetFullPath($OutputTar)

if ((Test-Path -LiteralPath $OutputTar) -and -not $Force) {
    if (Test-RootfsTar -Path $OutputTar) {
        Write-Host "Using existing WSL rootfs: $OutputTar"
        exit 0
    }
    Write-Warning "Existing WSL rootfs is stale or invalid, rebuilding: $OutputTar"
    Remove-Item -Force -LiteralPath $OutputTar
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw 'docker not found. Install Docker Desktop or provide a prebuilt windows-wsl2-rootfs.tar.'
}
if (-not (Get-Command tar -ErrorAction SilentlyContinue)) {
    throw 'tar not found. A BSD/GNU tar executable is required to validate the rootfs archive.'
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputTar) | Out-Null

$images = [System.Collections.Generic.List[string]]::new()
$images.Add($BaseImage)
if ($BaseImage -eq 'ubuntu:24.04') {
    $images.Add('public.ecr.aws/docker/library/ubuntu:24.04')
    $images.Add('mcr.microsoft.com/devcontainers/base:ubuntu-24.04')
}

$prepareScript = Join-Path $scriptDir 'prepare_windows_wsl_rootfs.sh'
if (-not (Test-Path -LiteralPath $prepareScript -PathType Leaf)) {
    throw "required WSL rootfs preparation script is missing: $prepareScript"
}
$prepareCommand = 'sed -i "s/\r$//" /tmp/orcastudio-prepare-rootfs.sh; exec /bin/sh /tmp/orcastudio-prepare-rootfs.sh "$1"'


foreach ($image in $images) {
    $containerName = 'bambu-studio-wsl-rootfs-' + [guid]::NewGuid().ToString('N')
    try {
        Write-Host "Preparing WSL rootfs from image: $image"
        Invoke-WithRetry -Label "docker pull $image" -Action {
            & docker pull --platform linux/amd64 $image | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "docker pull failed for $image" }
        }
        Invoke-WithRetry -Label "docker create $image" -Action {
            & docker rm -f $containerName 2>$null | Out-Null
            & docker create --platform linux/amd64 --user 0:0 --name $containerName $image /bin/sh -lc 'trap : TERM INT; sleep infinity & wait' | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "docker create failed for $image" }
        }
        Invoke-WithRetry -Label "docker start $image" -Action {
            & docker start $containerName | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "docker start failed for $image" }
        }
        Invoke-WithRetry -Label 'copy WSL rootfs preparation script' -Action {
            & docker cp $prepareScript "${containerName}:/tmp/orcastudio-prepare-rootfs.sh" | Out-Host
            if ($LASTEXITCODE -ne 0) { throw 'copying the WSL rootfs preparation script failed' }
        }
        Invoke-WithRetry -Label 'prepare Linux auth runtime dependencies' -Action {
            & docker exec -e DEBIAN_FRONTEND=noninteractive $containerName /bin/sh -lc $prepareCommand sh $rootfsMarker | Out-Host
            if ($LASTEXITCODE -ne 0) { throw 'Linux auth runtime preparation failed' }
        }
        Invoke-WithRetry -Label "docker stop $image" -Action {
            & docker stop $containerName | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "docker stop failed for $containerName" }
        }

        if (Test-Path -LiteralPath $OutputTar) { Remove-Item -Force -LiteralPath $OutputTar }
        Invoke-WithRetry -Label "docker export $image" -Action {
            & docker export $containerName -o $OutputTar
            if ($LASTEXITCODE -ne 0) { throw "docker export failed for $image" }
        }

        if (Test-RootfsTar -Path $OutputTar) {
            Write-Host 'WSL rootfs created:'
            Write-Host "  $OutputTar"
            Write-Host "  image: $image"
            exit 0
        }

        Write-Warning "Created rootfs is invalid or incomplete: $OutputTar"
        Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $OutputTar
    }
    catch {
        Write-Warning "Failed with image $image: $($_.Exception.Message)"
        Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $OutputTar
    }
    finally {
        & docker rm -f $containerName 2>$null | Out-Null
    }
}

throw 'failed to create WSL rootfs from all configured images'
