param(
    [Parameter(Mandatory = $true)]
    [string]$Target,

    [Parameter(Mandatory = $true)]
    [int]$PortAddress,

    [Parameter(Mandatory = $true)]
    [int]$Red,

    [Parameter(Mandatory = $true)]
    [int]$Green,

    [Parameter(Mandatory = $true)]
    [int]$Blue,

    [int]$White = 0,
    [int]$Count = 40,
    [int]$IntervalMs = 25
)

$ErrorActionPreference = "Stop"

if ($PortAddress -lt 0 -or $PortAddress -gt 32767) {
    throw "PortAddress must be in range 0..32767"
}

foreach ($entry in @{
    Red = $Red
    Green = $Green
    Blue = $Blue
    White = $White
}.GetEnumerator()) {
    if ($entry.Value -lt 0 -or $entry.Value -gt 255) {
        throw "$($entry.Key) must be in range 0..255"
    }
}

if ($Count -lt 1 -or $Count -gt 255) {
    throw "Count must be in range 1..255"
}

if ($IntervalMs -lt 1 -or $IntervalMs -gt 1000) {
    throw "IntervalMs must be in range 1..1000"
}

$endpoint = New-Object System.Net.IPEndPoint(
    [System.Net.IPAddress]::Parse($Target),
    6454
)
$udp = New-Object System.Net.Sockets.UdpClient(
    [System.Net.Sockets.AddressFamily]::InterNetwork
)

try {
    for ($index = 1; $index -le $Count; $index++) {
        $packet = New-Object byte[] 22

        $id = [System.Text.Encoding]::ASCII.GetBytes("Art-Net")
        [Array]::Copy($id, 0, $packet, 0, $id.Length)
        $packet[7] = 0

        # OpDmx = 0x5000, Art-Net opcodes are little-endian.
        $packet[8] = 0x00
        $packet[9] = 0x50

        # ProtVer = 14, big-endian.
        $packet[10] = 0
        $packet[11] = 14

        # Non-zero rolling sequence; Physical=0.
        $packet[12] = [byte]$index
        $packet[13] = 0

        # 15-bit Port-Address: SubUni low byte + Net high 7 bits.
        $packet[14] = [byte]($PortAddress -band 0xff)
        $packet[15] = [byte](($PortAddress -shr 8) -band 0x7f)

        # Four RGBW data bytes. ArtDmx Length is big-endian and even.
        $packet[16] = 0
        $packet[17] = 4
        $packet[18] = [byte]$Red
        $packet[19] = [byte]$Green
        $packet[20] = [byte]$Blue
        $packet[21] = [byte]$White

        [void]$udp.Send($packet, $packet.Length, $endpoint)
        Start-Sleep -Milliseconds $IntervalMs
    }
}
finally {
    $udp.Dispose()
}

Write-Host (
    "DEV011F4 ArtDmx sent: target={0} port_address={1} rgbw={2},{3},{4},{5} packets={6}" -f `
        $Target, $PortAddress, $Red, $Green, $Blue, $White, $Count
)
