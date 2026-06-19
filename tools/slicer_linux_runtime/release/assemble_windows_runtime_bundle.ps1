param(
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [Parameter(Mandatory = $true)][string]$RootFs,
    [Parameter(Mandatory = $true)][string]$LinuxHostBinary,
    [string]$RuntimeDir = "",
    [string]$RuntimeDll = "",
    [string]$DistroName = "BambuStudio-LinuxRuntime"
)

$ErrorActionPreference = 'Stop'

function Get-ScriptDir {
    if ($PSScriptRoot) {
        return $PSScriptRoot
    }
    if ($PSCommandPath) {
        return (Split-Path -Parent $PSCommandPath)
    }
    if ($MyInvocation -and $MyInvocation.MyCommand -and $MyInvocation.MyCommand.Path) {
        return (Split-Path -Parent $MyInvocation.MyCommand.Path)
    }
    return (Get-Location).Path
}

$scriptDir = Get-ScriptDir
$toolsRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptDir '..'))
$wslRoot = Join-Path $toolsRoot 'wsl'

if (-not (Test-Path $RootFs)) { throw "RootFs not found: $RootFs" }
if (-not (Test-Path $LinuxHostBinary)) { throw "LinuxHostBinary not found: $LinuxHostBinary" }
if ($RuntimeDir -and -not (Test-Path $RuntimeDir)) { throw "RuntimeDir not found: $RuntimeDir" }
if ($RuntimeDll -and -not (Test-Path $RuntimeDll)) { throw "RuntimeDll not found: $RuntimeDll" }

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

Copy-Item -Force (Join-Path $wslRoot 'install_runtime.ps1') (Join-Path $OutputDir 'install_runtime.ps1')
Copy-Item -Force (Join-Path $wslRoot 'install_runtime.cmd') (Join-Path $OutputDir 'install_runtime.cmd')
Copy-Item -Force (Join-Path $wslRoot 'verify_runtime.ps1') (Join-Path $OutputDir 'verify_runtime.ps1')
Copy-Item -Force (Join-Path $wslRoot 'slicer_linux_runtime_wsl_run_host.sh') (Join-Path $OutputDir 'slicer_linux_runtime_wsl_run_host.sh')
Copy-Item -Force (Join-Path $wslRoot 'slicer_linux_runtime_wsl_distro.txt') (Join-Path $OutputDir 'slicer_linux_runtime_wsl_distro.txt')
Copy-Item -Force (Join-Path $wslRoot 'slicer_linux_runtime_component_dir.txt') (Join-Path $OutputDir 'slicer_linux_runtime_component_dir.txt')

Set-Content -Path (Join-Path $OutputDir 'slicer_linux_runtime_wsl_distro.txt') -Value ($DistroName + [Environment]::NewLine) -NoNewline:$false

Copy-Item -Force $RootFs (Join-Path $OutputDir 'windows-wsl2-rootfs.tar')
Copy-Item -Force $LinuxHostBinary (Join-Path $OutputDir 'slicer_linux_runtime_host')

if ($RuntimeDll) {
    Copy-Item -Force $RuntimeDll (Join-Path $OutputDir 'slicer_linux_runtime.dll')
}

if ($RuntimeDir) {
    Get-ChildItem -Path $RuntimeDir -File -ErrorAction Stop | ForEach-Object {
        Copy-Item -Force $_.FullName (Join-Path $OutputDir $_.Name)
    }
}

Write-Host 'Bundle created:'
Write-Host $OutputDir
