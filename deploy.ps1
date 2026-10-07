param(
    [string]$SdDrive = "",
    [string]$3dsIp = ""
)

$ErrorActionPreference = 'Stop'
$projectDir = $PSScriptRoot
$unixDir = ($projectDir -replace '\\', '/' -replace '^[A-Za-z]:', { param($m) '/' + $m.Value[0].ToString().ToLower() })

Write-Host "=== 1. Building Plex3DS ===" -ForegroundColor Cyan
& "C:\devkitPro\msys2\usr\bin\bash.exe" -lc "export DEVKITPRO=/c/devkitPro; export DEVKITARM=/c/devkitPro/devkitARM; export PATH=/usr/bin:/c/devkitPro/devkitARM/bin:/c/devkitPro/tools/bin:`$PATH; cd '$unixDir'; make"

if (!(Test-Path "$projectDir\Plex3DS.3dsx")) {
    Write-Error "Build failed: Plex3DS.3dsx not found."
    return
}
Write-Host "[+] Build successful: Plex3DS.3dsx created!" -ForegroundColor Green

# Deploy to SD Card if drive detected or provided
if ($SdDrive -ne "") {
    $sd = $SdDrive.TrimEnd('\')
    if (Test-Path $sd) {
        Write-Host "`n=== 2. Deploying to SD Card ($sd) ===" -ForegroundColor Cyan
        $appDir = "$sd\3ds\Plex3DS"
        $configDir = "$sd\3ds\plex-3ds"

        if (!(Test-Path $appDir)) { New-Item -ItemType Directory -Path $appDir -Force | Out-Null }
        if (!(Test-Path $configDir)) { New-Item -ItemType Directory -Path $configDir -Force | Out-Null }

        Copy-Item "$projectDir\Plex3DS.3dsx" "$appDir\Plex3DS.3dsx" -Force
        Copy-Item "$projectDir\Plex3DS.smdh" "$appDir\Plex3DS.smdh" -Force
        if (Test-Path "$projectDir\config.json") {
            Copy-Item "$projectDir\config.json" "$configDir\config.json" -Force
        }
        Write-Host "[+] Copied to $appDir\Plex3DS.3dsx and config staged!" -ForegroundColor Green
    }
}

# Deploy over Wi-Fi via 3dslink (NetLoader) if IP provided
if ($3dsIp -ne "") {
    Write-Host "`n=== 3. Sending via NetLoader (3dslink) to $3dsIp ===" -ForegroundColor Cyan
    & "C:\devkitPro\tools\bin\3dslink.exe" -a $3dsIp "$projectDir\Plex3DS.3dsx"
}
