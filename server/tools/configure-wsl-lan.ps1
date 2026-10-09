param(
    [string]$Distro = "Ubuntu",
    [string]$ListenAddress = "192.168.137.1",
    [int]$Port = 8000,
    [string]$RemoteAddress = "192.168.137.0/24"
)

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an Administrator PowerShell window."
}
if ($Port -lt 1 -or $Port -gt 65535) { throw "Port is outside the valid TCP range." }

$localAddress = Get-NetIPAddress -AddressFamily IPv4 -IPAddress $ListenAddress -ErrorAction SilentlyContinue
if (-not $localAddress) {
    throw "Windows does not currently own $ListenAddress. Set -ListenAddress to the Windows LAN IPv4 from ipconfig."
}

$wslOutput = (& wsl.exe -d $Distro -- hostname -I 2>$null) -join " "
if ($LASTEXITCODE -ne 0) { throw "Could not query WSL distro '$Distro'. Start it and retry." }
$match = [regex]::Match($wslOutput, '\b(?:\d{1,3}\.){3}\d{1,3}\b')
if (-not $match.Success) { throw "Could not find the WSL IPv4 address in: $wslOutput" }
$wslAddress = $match.Value

Write-Host "Forwarding $ListenAddress`:$Port -> $wslAddress`:$Port for $RemoteAddress"
& netsh.exe interface portproxy delete v4tov4 listenaddress=$ListenAddress listenport=$Port 2>$null | Out-Null
& netsh.exe interface portproxy add v4tov4 listenaddress=$ListenAddress listenport=$Port connectaddress=$wslAddress connectport=$Port
if ($LASTEXITCODE -ne 0) { throw "Could not add the Windows port-forward rule." }

$ruleName = "Dispense Telemetry WSL $Port"
Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Action Allow `
    -Protocol TCP -LocalAddress $ListenAddress -LocalPort $Port `
    -RemoteAddress $RemoteAddress -Profile Any | Out-Null

Write-Host "LAN forwarding is configured. Re-run this script after WSL restarts because its IP can change."
