param([Parameter(Mandatory=$true)][string]$Port)
$ErrorActionPreference = 'Stop'
$voicePython = Join-Path (Split-Path $PSScriptRoot -Parent) '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $voicePython)) {
    throw 'Extract firmware-install inside the project folder and run setup.ps1 there first.'
}
$manifestPath = Join-Path $PSScriptRoot 'SHA256.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$images = @('bootloader.bin','partition-table.bin','home_voice_satellite.bin','srmodels.bin')
foreach ($name in $images) {
    $imagePath = Join-Path $PSScriptRoot $name
    if (-not (Test-Path -LiteralPath $imagePath)) { throw "Missing firmware image: $name" }
    $expected = $manifest.PSObject.Properties[$name].Value
    if (-not $expected -or (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash -ne $expected) {
        throw "Firmware checksum mismatch: $name"
    }
}
Write-Host 'Image hashes verified. Target: Waveshare ESP32-S3-AUDIO-Board, 16 MB flash / 8 MB octal PSRAM.'
Write-Host 'This flashes firmware only. It does not provision eFuses or erase saved settings.'
& $voicePython -m pip install 'esptool==5.4.0'
if ($LASTEXITCODE) { throw 'Could not install the flashing utility.' }
Push-Location $PSScriptRoot
try {
    & $voicePython -m esptool --chip esp32s3 --port $Port --baud 460800 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 home_voice_satellite.bin 0x410000 srmodels.bin
    if ($LASTEXITCODE) { throw 'Flashing failed. Check the port/cable and the troubleshooting guide.' }
} finally { Pop-Location }
Write-Host 'Firmware installed. Continue with encrypted-storage provisioning and browser setup.'
