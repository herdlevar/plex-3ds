param(
    [string]$Subnet = ""
)

if (-not $Subnet) {
    $ipObj = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.InterfaceAlias -notlike "*Loopback*" -and $_.IPAddress -notlike "169.254*" } | Select-Object -First 1).IPAddress
    if ($ipObj) {
        $parts = $ipObj.Split('.')
        $Subnet = "$($parts[0]).$($parts[1]).$($parts[2])."
    } else {
        $Subnet = "192.168.1."
    }
}
if (-not $Subnet.EndsWith('.')) { $Subnet += '.' }

$base = $Subnet
Write-Host "Sweeping ${base}0/24 for 3DS..." -ForegroundColor Cyan

# 1. Quick ping sweep to populate ARP cache
1..254 | ForEach-Object -Parallel {
    $ip = $using:base + $_
    $ping = New-Object System.Net.NetworkInformation.Ping
    try {
        $ping.Send($ip, 100) | Out-Null
    } catch {}
} -ThrottleLimit 64

# 2. Get all ARP entries
$arpPattern = "${base}*"
$arpEntries = Get-NetNeighbor -AddressFamily IPv4 | Where-Object { $_.IPAddress -like $arpPattern -and $_.State -ne "Unreachable" }

# Nintendo known MAC prefixes
$nintendoPrefixes = @("00-09-BF", "00-16-56", "00-17-AB", "00-19-1D", "00-19-FD", "00-1A-E9", "00-1B-7A", "00-1B-EA", "00-1C-BE", "00-1D-BC", "00-1E-35", "00-1E-A9", "00-1F-32", "00-1F-C5", "00-21-47", "00-21-BD", "00-22-4C", "00-22-AA", "00-22-D7", "00-23-31", "00-23-CC", "00-24-1E", "00-24-44", "00-24-F3", "00-25-A0", "00-26-59", "00-27-09", "04-03-D6", "08-A2-24", "08-F0-43", "08-F1-DA", "0C-FE-45", "18-2A-7B", "2C-10-C1", "34-AF-2C", "40-D2-8A", "40-F4-07", "58-2F-40", "58-BD-A3", "60-6B-FF", "78-A2-A0", "7C-BB-8A", "8C-56-C5", "94-01-C2", "94-58-CB", "98-B6-E9", "A4-38-CC", "A4-5C-27", "A4-C0-E1", "B8-78-26", "B8-AE-6E", "CC-9E-00", "D4-F0-57", "D8-6B-F7", "DC-68-EB", "E0-0C-7F", "E0-E7-51", "E8-4E-CE")

Write-Host "`nScanning devices for Nintendo MAC OUI or active 3DS services..."
$found = @()

foreach ($entry in $arpEntries) {
    $ip = $entry.IPAddress
    $mac = $entry.LinkLayerAddress.ToUpper()
    $prefix = $mac.Substring(0, [Math]::Min(8, $mac.Length))
    
    $isNintendo = $nintendoPrefixes -contains $prefix
    
    # Check port 5000 (FTPD) and 17491 (NetLoader)
    $hasPort5000 = $false
    $hasPort17491 = $false
    
    foreach ($p in @(5000, 17491)) {
        $tcp = New-Object System.Net.Sockets.TcpClient
        $ar = $tcp.BeginConnect($ip, $p, $null, $null)
        if ($ar.AsyncWaitHandle.WaitOne(80, $false)) {
            try {
                $tcp.EndConnect($ar)
                if ($p -eq 5000) { $hasPort5000 = $true }
                if ($p -eq 17491) { $hasPort17491 = $true }
            } catch {}
        }
        $tcp.Close()
    }
    
    if ($isNintendo -or $hasPort5000 -or $hasPort17491) {
        $found += [PSCustomObject]@{
            IP = $ip
            MAC = $mac
            IsNintendoMAC = $isNintendo
            FTPD = $hasPort5000
            NetLoader = $hasPort17491
        }
    }
}

if ($found.Count -eq 0) {
    Write-Host "No definitive 3DS signature detected via MAC/Port yet. Listing all active IPs:"
    $arpEntries | Select-Object IPAddress, LinkLayerAddress | Format-Table -AutoSize
} else {
    Write-Host "=== Found Potential 3DS Device(s) ===" -ForegroundColor Green
    $found | Format-Table -AutoSize
}
