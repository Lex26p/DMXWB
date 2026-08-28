param(
    [Parameter(Mandatory=$true)]
    [ValidateSet("Status","GreenBurst","RunTimedRedRelease")]
    [string]$Action,

    [string]$SourceIp = "10.200.200.2",
    [string]$DestinationIp = "10.200.200.1",
    [int]$Physical = 42
)

$ErrorActionPreference = "Stop"

$ProbeName = "DMXWB DEV010C5 ArtSync Probe"
$ProbeVersion = "1.1"

function New-ArtDmxPacket {
    param([byte[]]$Rgbw)

    $packet = New-Object byte[] 22
    $id = [System.Text.Encoding]::ASCII.GetBytes("Art-Net")
    [Array]::Copy($id, 0, $packet, 0, 7)
    $packet[7] = 0

    # OpDmx = 0x5000, little-endian on wire.
    $packet[8] = 0x00
    $packet[9] = 0x50

    # ProtVer = 14, big-endian.
    $packet[10] = 0x00
    $packet[11] = 0x0e

    # Sequence 0 disables sequence ordering for the deterministic probe.
    $packet[12] = 0
    $packet[13] = [byte]$Physical

    # Port-Address 0.
    $packet[14] = 0
    $packet[15] = 0

    # Length = 4, big-endian.
    $packet[16] = 0
    $packet[17] = 4

    [Array]::Copy($Rgbw, 0, $packet, 18, 4)
    return $packet
}

function New-ArtSyncPacket {
    $packet = New-Object byte[] 14
    $id = [System.Text.Encoding]::ASCII.GetBytes("Art-Net")
    [Array]::Copy($id, 0, $packet, 0, 7)
    $packet[7] = 0

    # OpSync = 0x5200, little-endian on wire.
    $packet[8] = 0x00
    $packet[9] = 0x52

    # ProtVer = 14.
    $packet[10] = 0x00
    $packet[11] = 0x0e
    $packet[12] = 0
    $packet[13] = 0
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
    if ($Action -eq "GreenBurst") {
        $green = New-ArtDmxPacket -Rgbw ([byte[]](0,255,0,0))

        for ($i = 0; $i -lt 25; $i++) {
            Send-Packet -Client $client -Packet $green
            Start-Sleep -Milliseconds 40
        }

        Write-Output "green_burst_sent=25"
        exit 0
    }

    if ($Action -eq "RunTimedRedRelease") {
        # Re-establish this deterministic probe as ACTIVE with GREEN.
        $green = New-ArtDmxPacket -Rgbw ([byte[]](0,255,0,0))
        for ($i = 0; $i -lt 8; $i++) {
            Send-Packet -Client $client -Packet $green
            Start-Sleep -Milliseconds 30
        }

        $sync = New-ArtSyncPacket

        [Console]::Out.WriteLine("phase=FIRST_SYNC")
        [Console]::Out.Flush()
        Send-Packet -Client $client -Packet $sync
        $firstSyncAt = [DateTimeOffset]::UtcNow
        Start-Sleep -Milliseconds 100

        # Continuously stage RED for about 2.5 s. Progress is printed live so
        # the operator can see that the fixture must remain GREEN throughout
        # the entire staging phase.
        $red = New-ArtDmxPacket -Rgbw ([byte[]](255,0,0,0))
        $redPackets = 0
        $nextMarkerMs = 500
        $stageTargetMs = 2500

        [Console]::Out.WriteLine("phase=STAGING_RED_START")
        [Console]::Out.Flush()

        while ($true) {
            $elapsedMs = [int](([DateTimeOffset]::UtcNow - $firstSyncAt).TotalMilliseconds)
            if ($elapsedMs -ge $stageTargetMs) {
                break
            }

            Send-Packet -Client $client -Packet $red
            $redPackets++

            if ($elapsedMs -ge $nextMarkerMs) {
                [Console]::Out.WriteLine("staging_elapsed_ms=$elapsedMs")
                [Console]::Out.Flush()
                $nextMarkerMs += 500
            }

            Start-Sleep -Milliseconds 40
        }

        $secondSyncAt = [DateTimeOffset]::UtcNow
        $stagingMs = [int](($secondSyncAt - $firstSyncAt).TotalMilliseconds)

        [Console]::Out.WriteLine("phase=SECOND_SYNC_NOW")
        [Console]::Out.Flush()
        Send-Packet -Client $client -Packet $sync

        # Keep RED arriving briefly after release so the resulting RED output
        # is easy to observe.
        for ($i = 0; $i -lt 25; $i++) {
            Send-Packet -Client $client -Packet $red
            Start-Sleep -Milliseconds 40
        }

        [Console]::Out.WriteLine("phase=RED_RELEASE_COMPLETE")
        [Console]::Out.WriteLine("red_staging_packets=$redPackets")
        [Console]::Out.WriteLine("staging_window_ms=$stagingMs")
        [Console]::Out.Flush()
        exit 0
    }

    throw "Unsupported action: $Action"
}
finally {
    $client.Dispose()
}
