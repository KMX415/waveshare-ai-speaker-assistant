param([switch]$Flash, [string]$Port = 'COM14')
$ErrorActionPreference = 'Stop'
$voiceCache = Join-Path $env:USERPROFILE '.cache\home-voice'
$env:IDF_PATH = Join-Path $voiceCache 'esp-idf'
$env:IDF_TOOLS_PATH = Join-Path $voiceCache 'idf-tools'
$voicePython = Join-Path $env:IDF_TOOLS_PATH 'python_env\idf5.5_py3.12_env\Scripts\python.exe'
$exports = & $voicePython (Join-Path $env:IDF_PATH 'tools\idf_tools.py') export --format key-value
if ($LASTEXITCODE) { throw 'ESP-IDF environment export failed' }
foreach ($entry in $exports) {
    if ($entry -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') {
        $value = $Matches[2].Replace('%PATH%', $env:PATH).Replace('$PATH', $env:PATH)
        [Environment]::SetEnvironmentVariable($Matches[1], $value, 'Process')
    }
}
$env:IDF_PYTHON_ENV_PATH = Split-Path (Split-Path $voicePython)
# ESP-IDF requires a path without spaces. Source remains in the workspace.
$stage = Join-Path $voiceCache 'firmware'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CMakeLists.txt'), (Join-Path $PSScriptRoot 'sdkconfig.defaults') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'partitions.csv') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'main') -Destination $stage -Recurse -Force
if (Test-Path (Join-Path $PSScriptRoot 'dependencies.lock')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'dependencies.lock') -Destination $stage -Force
}
Push-Location $stage
try {
    # Apply partition changes to an existing development build as well as a fresh one.
    if (Test-Path 'sdkconfig') {
        $configText = Get-Content 'sdkconfig' -Raw
        $configText = $configText.Replace('CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y', '# CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE is not set')
        $configText = $configText.Replace('# CONFIG_PARTITION_TABLE_CUSTOM is not set', 'CONFIG_PARTITION_TABLE_CUSTOM=y')
        $configText = $configText.Replace('CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y', '# CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC is not set')
        $configText = $configText.Replace('CONFIG_ESP_CONSOLE_SECONDARY_NONE=y', '# CONFIG_ESP_CONSOLE_SECONDARY_NONE is not set')
        foreach ($setting in @('ESP_WS_CLIENT_SEPARATE_TX_LOCK', 'NVS_ENCRYPTION', 'NVS_SEC_KEY_PROTECT_USING_HMAC', 'MBEDTLS_EXTERNAL_MEM_ALLOC', 'ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG', 'SR_WN_WN9_JARVIS_TTS', 'SR_WN_WN9_COMPUTER_TTS')) {
            $configText = $configText -replace "(?m)^# CONFIG_$setting is not set\r?$", "CONFIG_$setting=y"
            if ($configText -notmatch "(?m)^CONFIG_$setting=y") { $configText += "`nCONFIG_$setting=y`n" }
        }
        $configText = $configText -replace '(?m)^CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=.*$', 'CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=5'
        $configText = $configText -replace '(?m)^CONFIG_ESP_MAIN_TASK_STACK_SIZE=.*$', 'CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192'
        $configText = $configText -replace '(?m)^CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=.*$', 'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024'
        $configText = $configText -replace '(?m)^CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=.*$', 'CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536'
        Set-Content -LiteralPath 'sdkconfig' -Value $configText -Encoding utf8
    }
    & $voicePython (Join-Path $env:IDF_PATH 'tools\idf.py') build
    if ($LASTEXITCODE) { throw 'Firmware build failed' }
    if ($Flash) {
        & $voicePython (Join-Path $env:IDF_PATH 'tools\idf.py') -p $Port flash
        if ($LASTEXITCODE) { throw 'Firmware flash failed' }
    }
} finally { Pop-Location }
