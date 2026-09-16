$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
if (-not (Test-Path -LiteralPath '.venv\Scripts\python.exe')) {
    $voicePython = Get-Command python -ErrorAction SilentlyContinue
    $voiceBundled = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
    if ($voicePython) { & $voicePython.Source -m venv .venv }
    elseif (Test-Path -LiteralPath $voiceBundled) { & $voiceBundled -m venv .venv }
    else { throw 'Install Python 3.11 or newer, then run this script again.' }
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the Python environment.' }
}
& '.\.venv\Scripts\python.exe' -m pip install -c requirements-lock.txt -e .
if ($LASTEXITCODE -ne 0) { throw 'Dependency installation failed.' }
Write-Host 'Ready. Follow docs/getting-started.md to flash and configure your Waveshare speaker.'
