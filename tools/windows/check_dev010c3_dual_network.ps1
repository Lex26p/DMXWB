param(
    [string]$PrimarySourceIp = "10.200.200.2",
    [string]$PrimaryDestinationIp = "10.200.200.1",
    [string]$SecondarySourceIp = "192.168.42.160",
    [string]$SecondaryDestinationIp = "192.168.42.1"
)

$ErrorActionPreference = "Stop"

function Get-SingleIpv4 {
    param([string]$Address)

    $items = @(
        Get-NetIPAddress -AddressFamily IPv4 -IPAddress $Address -ErrorAction SilentlyContinue |
            Where-Object { $_.AddressState -ne "Duplicate" }
    )
    if ($items.Count -lt 1) {
        throw "IPv4 address $Address is not assigned on Windows."
    }
    return $items[0]
}

function Get-SelectedSourceIp {
    param([string]$Destination)

    $client = New-Object System.Net.Sockets.UdpClient([System.Net.Sockets.AddressFamily]::InterNetwork)
    try {
        $client.Connect($Destination, 6454)
        $endpoint = [System.Net.IPEndPoint]$client.Client.LocalEndPoint
        return $endpoint.Address.ToString()
    }
    finally {
        $client.Dispose()
    }
}

$primary = Get-SingleIpv4 -Address $PrimarySourceIp
$secondary = Get-SingleIpv4 -Address $SecondarySourceIp

if ($primary.InterfaceIndex -eq $secondary.InterfaceIndex) {
    throw "Primary and secondary IPv4 addresses are on the same Windows interface index $($primary.InterfaceIndex)."
}

$primarySelected = Get-SelectedSourceIp -Destination $PrimaryDestinationIp
$secondarySelected = Get-SelectedSourceIp -Destination $SecondaryDestinationIp

if ($primarySelected -ne $PrimarySourceIp) {
    throw "Windows route to $PrimaryDestinationIp selects $primarySelected, expected $PrimarySourceIp."
}
if ($secondarySelected -ne $SecondarySourceIp) {
    throw "Windows route to $SecondaryDestinationIp selects $secondarySelected, expected $SecondarySourceIp."
}

$primaryPing = Test-Connection -ComputerName $PrimaryDestinationIp -Count 2 -Quiet -ErrorAction SilentlyContinue
$secondaryPing = Test-Connection -ComputerName $SecondaryDestinationIp -Count 2 -Quiet -ErrorAction SilentlyContinue

if (-not $primaryPing) {
    throw "Primary WB address $PrimaryDestinationIp does not answer ping."
}
if (-not $secondaryPing) {
    throw "Wi-Fi WB address $SecondaryDestinationIp does not answer ping."
}

Write-Output "checker_name=DMXWB DEV010C3 Dual Network Check"
Write-Output "checker_version=1.0"
Write-Output "primary_source_ip=$PrimarySourceIp"
Write-Output "primary_destination_ip=$PrimaryDestinationIp"
Write-Output "primary_interface_alias=$($primary.InterfaceAlias)"
Write-Output "primary_interface_index=$($primary.InterfaceIndex)"
Write-Output "primary_route_selected_source=$primarySelected"
Write-Output "primary_ping=PASS"
Write-Output "secondary_source_ip=$SecondarySourceIp"
Write-Output "secondary_destination_ip=$SecondaryDestinationIp"
Write-Output "secondary_interface_alias=$($secondary.InterfaceAlias)"
Write-Output "secondary_interface_index=$($secondary.InterfaceIndex)"
Write-Output "secondary_route_selected_source=$secondarySelected"
Write-Output "secondary_ping=PASS"
Write-Output "windows_interfaces_distinct=PASS"
Write-Output "dual_network_check=PASS"
