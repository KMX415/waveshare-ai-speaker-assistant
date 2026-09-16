$ErrorActionPreference = 'Stop'
$voicePython = Join-Path $PSScriptRoot '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $voicePython)) { throw 'Run .\setup.ps1 first.' }
& $voicePython -m home_voice @args
exit $LASTEXITCODE
