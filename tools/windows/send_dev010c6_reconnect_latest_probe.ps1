param(
    [Parameter(Mandatory=$true)]
    [ValidateSet("Status","SendGreen","SendRed","SendBlue","SendWhite","RunBurst")]
    [string]$Action,

    [string]$SourceIp = "10.200.200.2",
    [string]$DestinationIp = "10.200.200.1",
    [int]$Physical = 43
)

$ErrorActionPreference = "Stop"
$ProbeName = "DMXWB DEV010C6 Reconnect Latest Probe"
$ProbeVersion = "1.1"

function New-ArtDmxPacket {
    param([byte[]]$Rgbw)

    $packet = New-Object byte[] 22
    $id = [System.Text.Encoding]::ASCII.GetBytes("Art-Net")
    [Array]::Copy($id, 0, $packet, 0, 7)
    $packet[7] = 0
    $packet[8] = 0x00
    $packet[9] = 0x50
    $packet[10] = 0x00
    $packet[11] = 0x0e
    $packet[12] = 0
    $packet[13] = [byte]$Physical
    $packet[14] = 0
    $packet[15] = 0
    $packet[16] = 0
    $packet[17] = 4
    [Array]::Copy($Rgbw, 0, $packet, 18, 4)
    return $packet
}

function New-BoundUdpClient {
    $sourceAddress = [System.Net.IPAddress]::Parse($SourceIp)
    $localEndpoint = New-Object System.Net.IPEndPoint($sourceAddress, 0)
    return New-Object System.Net.Sockets.UdpClient($localEndpoint)
}

function Send-Packet {
    param(
        [System.Net.Sockets.UdpClient]$Client,
        [byte[]]$Packet
    )
    $destinationAddress = [System.Net.IPAddress]::Parse($DestinationIp)
    $remoteEndpoint = New-Object System.Net.IPEndPoint($destinationAddress, 6454)
    [void]$Client.Send($Packet, $Packet.Length, $remoteEndpoint)
}

function Send-ColorStream {
    param(
        [System.Net.Sockets.UdpClient]$Client,
        [byte[]]$Packet,
        [string]$Name
    )

    $deadline = [DateTimeOffset]::UtcNow.AddMilliseconds(1200)
    $count = 0
    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        Send-Packet -Client $Client -Packet $Packet
        $count++
        Start-Sleep -Milliseconds 25
    }
    Write-Output "color_stream=$Name"
    Write-Output "color_stream_packets=$count"
}

if ($Action -eq "Status") {
    Write-Output "probe_name=$ProbeName"
    Write-Output "probe_version=$ProbeVersion"
    Write-Output "powershell_version=$($PSVersionTable.PSVersion.ToString())"
    Write-Output "source_ip=$SourceIp"
    Write-Output "destination_ip=$DestinationIp"
    Write-Output "physical=$Physical"
    exit 0
}

$client = New-BoundUdpClient
try {
    $green = New-ArtDmxPacket -Rgbw ([byte[]](0,255,0,0))
    $red = New-ArtDmxPacket -Rgbw ([byte[]](255,0,0,0))
    $blue = New-ArtDmxPacket -Rgbw ([byte[]](0,0,255,0))
    $white = New-ArtDmxPacket -Rgbw ([byte[]](0,0,0,255))

    if ($Action -eq "SendGreen") {
        Send-ColorStream -Client $client -Packet $green -Name "GREEN"
        exit 0
    }
    if ($Action -eq "SendRed") {
        Send-ColorStream -Client $client -Packet $red -Name "RED"
        exit 0
    }
    if ($Action -eq "SendBlue") {
        Send-ColorStream -Client $client -Packet $blue -Name "BLUE"
        exit 0
    }
    if ($Action -eq "SendWhite") {
        Send-ColorStream -Client $client -Packet $white -Name "WHITE"
        exit 0
    }

    if ($Action -eq "RunBurst") {
        [Console]::Out.WriteLine("phase=BURST_START")
        [Console]::Out.Flush()

        $burstPackets = @($red, $green, $white, $blue)
        $burstStart = [DateTimeOffset]::UtcNow
        $burstCount = 4096
        for ($i = 0; $i -lt $burstCount; $i++) {
            Send-Packet -Client $client -Packet $burstPackets[$i % 4]
        }
        $burstDurationMs = [int](([DateTimeOffset]::UtcNow - $burstStart).TotalMilliseconds)

        [Console]::Out.WriteLine("phase=FINAL_BLUE_GUARD_START")
        [Console]::Out.Flush()

        for ($i = 0; $i -lt 120; $i++) {
            Send-Packet -Client $client -Packet $blue
            Start-Sleep -Milliseconds 5
        }

        [Console]::Out.WriteLine("phase=FINAL_BLUE_STABLE")
        [Console]::Out.Flush()

        Write-Output "burst_packets_sent=$burstCount"
        Write-Output "burst_duration_ms=$burstDurationMs"
        exit 0
    }

    throw "Unsupported action: $Action"
}
finally {
    $client.Dispose()
}
