param(
    [string]$PackageDir = "",
    [string]$ComponentDir = "",
    [string]$ComponentCacheDir = "",
    [string]$DistroName = "",
    [string]$InstallDir = "",
    [switch]$ReplaceExisting,
    [switch]$SkipCopyToComponentDir
)

$ErrorActionPreference = 'Stop'
trap {
    Write-Error ("install_runtime.ps1 failed: {0}" -f $_.Exception.Message)
    exit 1
}
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

function Convert-FileToLf([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or !(Test-Path $Path)) {
        return
    }

    $content = [System.IO.File]::ReadAllText($Path)
    $content = $content.Replace("`r`n", "`n").Replace("`r", "`n")
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $content, $utf8NoBom)
}

function Copy-IfExists([string]$Source, [string]$Destination) {
    if (Test-Path $Source) {
        $srcFull = [System.IO.Path]::GetFullPath($Source)
        $dstFull = [System.IO.Path]::GetFullPath($Destination)
        if ($srcFull -ieq $dstFull) {
            return
        }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
        Copy-Item -Force $Source $Destination
    }
}

function Sync-Directory([string]$SourceDir, [string]$DestinationDir) {
    if (!(Test-Path $SourceDir)) {
        return
    }
    if (Test-Path $DestinationDir) {
        Remove-Item -Recurse -Force $DestinationDir
    }
    New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null
    Copy-Item -Recurse -Force (Join-Path $SourceDir '*') $DestinationDir
}

function Resolve-DistroName([string]$Dir, [string]$Current) {
    if (-not [string]::IsNullOrWhiteSpace($Current)) {
        return $Current
    }

    $distroFile = Join-Path $Dir 'slicer_linux_runtime_wsl_distro.txt'
    if (Test-Path $distroFile) {
        $value = (Get-Content $distroFile -Raw).Trim()
        if (-not [string]::IsNullOrWhiteSpace($value)) {
            return $value
        }
    }

    if ($env:SLICER_LINUX_RUNTIME_WSL_DISTRO) {
        return $env:SLICER_LINUX_RUNTIME_WSL_DISTRO.Trim()
    }

    return 'BambuStudio-LinuxRuntime'
}

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
}

function Get-RootFsHashMarkerPath([string]$Dir) {
    return (Join-Path $Dir 'slicer-linux-runtime-rootfs-sha256.txt')
}

function Write-RootFsHashMarker([string]$Dir, [string]$Hash) {
    if ([string]::IsNullOrWhiteSpace($Dir) -or [string]::IsNullOrWhiteSpace($Hash)) {
        return
    }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    Set-Content -Path (Get-RootFsHashMarkerPath $Dir) -Value ($Hash.Trim().ToLowerInvariant()) -NoNewline
}

function Read-RootFsHashMarker([string]$Dir) {
    $path = Get-RootFsHashMarkerPath $Dir
    if (!(Test-Path $path)) {
        return ''
    }
    return ((Get-Content $path -Raw).Trim().ToLowerInvariant())
}


function Resolve-ComponentCacheDir([string]$Dir, [string]$Current) {
    if (-not [string]::IsNullOrWhiteSpace($Current)) {
        return [System.IO.Path]::GetFullPath($Current.Trim())
    }

    if ($env:SLICER_LINUX_RUNTIME_WINDOWS_COMPONENT_CACHE_DIR) {
        return [System.IO.Path]::GetFullPath($env:SLICER_LINUX_RUNTIME_WINDOWS_COMPONENT_CACHE_DIR.Trim())
    }

    $subdirFile = Join-Path $Dir 'slicer_linux_runtime_component_dir.txt'
    if (Test-Path $subdirFile) {
        $subdir = (Get-Content $subdirFile -Raw).Trim()
        if (-not [string]::IsNullOrWhiteSpace($subdir)) {
            if (-not $env:APPDATA) { throw 'APPDATA is not available' }
            return [System.IO.Path]::GetFullPath((Join-Path $env:APPDATA $subdir))
        }
    }

    if (-not $env:APPDATA) { throw 'APPDATA is not available' }
    return [System.IO.Path]::GetFullPath((Join-Path $env:APPDATA 'BambuStudio_OrcaSlicer\ota\plugins'))
}

function Normalize-ComponentCacheDir([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $Path
    }
    $full = [System.IO.Path]::GetFullPath($Path)
    $pluginsChild = Join-Path $full 'plugins'
    if ((Split-Path -Leaf $full) -ieq 'ota' -and (Test-Path $pluginsChild)) {
        return [System.IO.Path]::GetFullPath($pluginsChild)
    }
    if ((Test-Path $pluginsChild) -and !(Test-Path (Join-Path $full 'libbambu_networking.so')) -and !(Test-Path (Join-Path $full 'libBambuSource.so'))) {
        return [System.IO.Path]::GetFullPath($pluginsChild)
    }
    return $full
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

function Invoke-NativeCapture([string]$FilePath, [string[]]$ArgumentList) {
    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()
    try {
        $proc = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList -Wait -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden
        $stdoutText = if (Test-Path $stdoutPath) { Normalize-NativeText (Read-TextAuto $stdoutPath) } else { '' }
        $stderrText = if (Test-Path $stderrPath) { Normalize-NativeText (Read-TextAuto $stderrPath) } else { '' }
        $combined = (($stdoutText + "`n" + $stderrText).Trim())
        return @{
            ExitCode = $proc.ExitCode
            StdOut = $stdoutText
            StdErr = $stderrText
            Combined = $combined
        }
    } finally {
        Remove-Item -Force -ErrorAction SilentlyContinue $stdoutPath, $stderrPath
    }
}


function Assert-NativeOk($Result, [string]$Action) {
    if ($Result.ExitCode -eq 0) {
        return
    }
    $text = $Result.Combined
    if ([string]::IsNullOrWhiteSpace($text)) {
        $text = 'no details'
    }
    throw ("{0} failed with exit code {1}: {2}" -f $Action, $Result.ExitCode, $text)
}

function Invoke-Wsl([string[]]$ArgumentList, [string]$Action) {
    $result = Invoke-NativeCapture $script:wsl @ArgumentList
    Assert-NativeOk $result $Action
    return $result
}

function Remove-StaleInstallDir([string]$Dir) {
    if ([string]::IsNullOrWhiteSpace($Dir)) {
        return
    }
    if (Test-Path $Dir) {
        Remove-Item -Recurse -Force $Dir
    }
}

function Test-WslDistroExists([string]$WslPath, [string]$Name, [ref]$Reason) {
    $list = Invoke-NativeCapture $WslPath @('--list', '--quiet')
    if ($list.ExitCode -ne 0) {
        $text = $list.Combined
        if ([string]::IsNullOrWhiteSpace($text)) {
            throw 'Failed to query WSL distributions'
        }
        throw ("Failed to query WSL distributions: {0}" -f $text)
    }

    $exists = $false
    foreach ($line in ($list.StdOut -split "`n")) {
        $item = $line.Trim()
        if ([string]::IsNullOrWhiteSpace($item)) {
            continue
        }
        if ($item -ieq $Name) {
            $exists = $true
            break
        }
    }

    if (-not $exists) {
        $Reason.Value = "WSL distro '$Name' is not installed"
        return $false
    }

    $probe = Invoke-NativeCapture $WslPath @('-d', $Name, '--user', 'root', '--', 'sh', '-lc', 'true')
    if ($probe.ExitCode -eq 0) {
        $Reason.Value = ''
        return $true
    }

    $text = $probe.Combined
    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "Failed to start WSL distro '$Name'"
    }

    throw ("Failed to start WSL distro '{0}': {1}" -f $Name, $text)
}

$scriptDir = Get-ScriptDir
$defaultPackageDir = $scriptDir
if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    $PackageDir = $defaultPackageDir
}
$PackageDir = [System.IO.Path]::GetFullPath($PackageDir)

$DistroName = Resolve-DistroName $PackageDir $DistroName
$ComponentCacheDir = Normalize-ComponentCacheDir (Resolve-ComponentCacheDir $PackageDir $ComponentCacheDir)

if ([string]::IsNullOrWhiteSpace($ComponentDir)) {
    if (-not $env:APPDATA) {
        throw 'APPDATA is not available'
    }
    $ComponentDir = Join-Path $env:APPDATA 'BambuStudio_OrcaSlicer\plugins'
}
$ComponentDir = [System.IO.Path]::GetFullPath($ComponentDir)

if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    if (-not $env:LOCALAPPDATA) {
        throw 'LOCALAPPDATA is not available'
    }
    $InstallDir = Join-Path $env:LOCALAPPDATA $DistroName
}
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)

$script:wsl = Join-Path $env:WINDIR 'System32\wsl.exe'
if (!(Test-Path $script:wsl)) {
    throw 'wsl.exe not found'
}
$wsl = $script:wsl

if (-not $SkipCopyToComponentDir) {
    New-Item -ItemType Directory -Force -Path $ComponentDir | Out-Null

    $fileNames = @(
        'slicer_linux_runtime.dll',
        'slicer_linux_runtime_host',
        'slicer_linux_runtime_host_abi1',
        'slicer_linux_runtime_host_abi0',
        'slicer_linux_runtime_wsl_distro.txt',
        'slicer_linux_runtime_component_dir.txt',
        'slicer_linux_runtime_wsl_run_host.sh',
        'slicer_linux_runtime_wsl_run_host.sh',
        'install_runtime.ps1',
        'install_runtime.cmd',
        'verify_runtime.ps1',
        'windows-wsl2-rootfs.tar',
        'README_runtime_runtime.txt',
        'assemble_windows_runtime_bundle.ps1',
        'linux_component_manifest.json',
        'libbambu_networking.so',
        'libBambuSource.so',
        'liblive555.so',
        'libagora_rtc_sdk.so',
        'libagora-fdkaac.so',
        'ca-certificates.crt',
        'slicer_base64.cer'
    )

    foreach ($name in $fileNames) {
        Copy-IfExists (Join-Path $PackageDir $name) (Join-Path $ComponentDir $name)
    }

    Get-ChildItem -Path $PackageDir -File -ErrorAction SilentlyContinue | ForEach-Object {
        $name = $_.Name
        if ($name -match '^lib.+\.so(\..+)?$') {
            Copy-IfExists $_.FullName (Join-Path $ComponentDir $name)
        }
    }

    $legacyRuntimeDir = Join-Path $ComponentDir 'slicer_linux_runtime_host.runtime'
    if (Test-Path $legacyRuntimeDir) {
        Remove-Item -Recurse -Force $legacyRuntimeDir
    }
    $PackageDir = $ComponentDir

    Write-Host "Runtime package dir: $PackageDir"
    Write-Host "Component dir: $ComponentDir"
    Write-Host "Component cache dir: $ComponentCacheDir"
    Write-Host "WSL distro: $DistroName"
}

$requiredFiles = @(
    'slicer_linux_runtime.dll',
    'slicer_linux_runtime_host',
    'slicer_linux_runtime_host_abi1',
    'slicer_linux_runtime_host_abi0',
    'slicer_linux_runtime_wsl_distro.txt',
    'install_runtime.ps1',
    'verify_runtime.ps1',
    'windows-wsl2-rootfs.tar',
    'ca-certificates.crt',
    'slicer_base64.cer'
)

foreach ($name in $requiredFiles) {
    $path = Join-Path $PackageDir $name
    if (!(Test-Path $path)) {
        throw "Missing package file: $name"
    }
}

$bootstrapPath = Join-Path $PackageDir 'slicer_linux_runtime_wsl_run_host.sh'
if (!(Test-Path $bootstrapPath)) { $bootstrapPath = Join-Path $PackageDir 'slicer_linux_runtime_wsl_run_host.sh' }
if (!(Test-Path $bootstrapPath)) {
    throw 'Missing package file: slicer_linux_runtime_wsl_run_host.sh'
}

$statusResult = Invoke-NativeCapture $wsl @('--status')
if ($statusResult.ExitCode -ne 0) {
    $statusText = $statusResult.Combined
    if ([string]::IsNullOrWhiteSpace($statusText)) {
        $statusText = 'no details'
    }
    throw ("WSL is not ready. Enable Microsoft-Windows-Subsystem-Linux and VirtualMachinePlatform, reboot, then try again. Details: {0}" -f $statusText)
}

Convert-FileToLf $bootstrapPath

$rootFsTar = Join-Path $PackageDir 'windows-wsl2-rootfs.tar'
$currentRootFsHash = Get-FileSha256 $rootFsTar
$storedRootFsHash = Read-RootFsHashMarker $InstallDir

$distroReason = ''
$alreadyInstalled = Test-WslDistroExists $wsl $DistroName ([ref]$distroReason)
if ($alreadyInstalled) {
    if (-not $ReplaceExisting) {
        if ([string]::IsNullOrWhiteSpace($storedRootFsHash) -or $storedRootFsHash -ne $currentRootFsHash) {
            Write-Host "WSL rootfs changed or marker missing - reinstalling distro $DistroName"
            $ReplaceExisting = $true
        }
    }

    if ($ReplaceExisting) {
        Invoke-NativeCapture $wsl @('--terminate', $DistroName) | Out-Null
        $unregisterResult = Invoke-NativeCapture $wsl @('--unregister', $DistroName)
        Assert-NativeOk $unregisterResult "wsl --unregister $DistroName"
        $alreadyInstalled = $false
    }
}

if (-not $alreadyInstalled) {
    Remove-StaleInstallDir $InstallDir
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

    Write-Host "Importing WSL runtime rootfs..."
    $importResult = Invoke-NativeCapture $wsl @('--import', $DistroName, $InstallDir, $rootFsTar, '--version', '2')
    Assert-NativeOk $importResult "wsl --import $DistroName"
    if (-not [string]::IsNullOrWhiteSpace($importResult.Combined)) {
        Write-Host $importResult.Combined
    }

    $wslConf = @'
[automount]
enabled=true
root=/mnt/
mountFsTab=false

[interop]
enabled=true
appendWindowsPath=false
'@

    $setupCmd = @"
cat > /etc/wsl.conf <<'WSL_EOF'
$wslConf
WSL_EOF
mkdir -p /root/.slicer-linux-runtime
"@

    Write-Host "Initializing WSL runtime distro..."
    $initResult = Invoke-NativeCapture $wsl @('-d', $DistroName, '--user', 'root', '--', 'sh', '-lc', $setupCmd)
    Assert-NativeOk $initResult "initialize WSL distro $DistroName"
    if (-not [string]::IsNullOrWhiteSpace($initResult.Combined)) {
        Write-Host $initResult.Combined
    }

    $terminateResult = Invoke-NativeCapture $wsl @('--terminate', $DistroName)
    Assert-NativeOk $terminateResult "wsl --terminate $DistroName"

    Write-RootFsHashMarker $InstallDir $currentRootFsHash
} elseif ($storedRootFsHash -ne $currentRootFsHash) {
    Write-RootFsHashMarker $InstallDir $currentRootFsHash
}

$verifyArgs = @(
    '-NoProfile',
    '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $PackageDir 'verify_runtime.ps1'),
    '-PackageDir', $PackageDir,
    '-DistroName', $DistroName,
    '-ComponentCacheDir', $ComponentCacheDir,
    '-AllowMissingComponent',
    '-SkipProbe'
)

$verifyShell = $null
$pwshCmd = Get-Command pwsh -ErrorAction SilentlyContinue
if ($pwshCmd) {
    $verifyShell = $pwshCmd.Source
} else {
    $powershellCmd = Get-Command powershell -ErrorAction SilentlyContinue
    if ($powershellCmd) {
        $verifyShell = $powershellCmd.Source
    }
}
if ([string]::IsNullOrWhiteSpace($verifyShell)) {
    throw 'No PowerShell host found to run verify_runtime.ps1'
}

$verifyResult = Invoke-NativeCapture $verifyShell $verifyArgs
if (-not [string]::IsNullOrWhiteSpace($verifyResult.StdOut)) {
    Write-Host $verifyResult.StdOut
}
if (-not [string]::IsNullOrWhiteSpace($verifyResult.StdErr)) {
    Write-Host $verifyResult.StdErr
}
Assert-NativeOk $verifyResult 'verify_runtime.ps1'

Write-Host ''
Write-Host "WSL runtime installed to: $PackageDir"
Write-Host "WSL distro: $DistroName"
Write-Host "WSL install dir: $InstallDir"
Write-Host 'Now start OrcaSlicer.'
