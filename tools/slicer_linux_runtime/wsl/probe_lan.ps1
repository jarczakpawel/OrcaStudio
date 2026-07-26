param(
    [Parameter(Mandatory = $true)][string]$PrinterIp,
    [string]$PackageDir = "",
    [string]$Distro = ""
)

$ErrorActionPreference = 'Stop'

if ($PrinterIp -notmatch '^[0-9]{1,3}(\.[0-9]{1,3}){3}$') {
    throw 'PrinterIp must be an IPv4 address'
}

$octets = $PrinterIp.Split('.')
foreach ($octet in $octets) {
    $value = [int]$octet
    if ($value -lt 0 -or $value -gt 255) {
        throw 'PrinterIp must be an IPv4 address'
    }
}

if ([string]::IsNullOrWhiteSpace($Distro)) {
    if (-not [string]::IsNullOrWhiteSpace($env:SLICER_LINUX_RUNTIME_WSL_DISTRO)) {
        $Distro = $env:SLICER_LINUX_RUNTIME_WSL_DISTRO
    } elseif (-not [string]::IsNullOrWhiteSpace($PackageDir)) {
        $distroFile = Join-Path $PackageDir 'slicer_linux_runtime_wsl_distro.txt'
        if (Test-Path $distroFile) {
            $Distro = (Get-Content -LiteralPath $distroFile -TotalCount 1).Trim()
        }
    }
}

if ([string]::IsNullOrWhiteSpace($Distro)) {
    $Distro = 'slicer-linux-runtime'
}

$escapedIp = $PrinterIp.Replace("'", "'\\''")
$script = @'
set -eu
IP='__PRINTER_IP__'
printf 'printer_ip=%s\n' "$IP"
printf 'routes:\n'
ip -4 route || true
printf 'interfaces:\n'
ip -4 -o addr show || true
for port in 8883 990 6000; do
    if timeout 3 bash -lc ": </dev/tcp/$IP/$port" >/dev/null 2>&1; then
        printf 'tcp_%s=ok\n' "$port"
    else
        printf 'tcp_%s=fail\n' "$port"
    fi
done
'@
$script = $script.Replace('__PRINTER_IP__', $escapedIp)

& wsl.exe -d $Distro -- sh -lc $script
exit $LASTEXITCODE
