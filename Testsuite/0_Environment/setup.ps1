# Windows: create the test environment in Testsuite/0_Environment/.venv
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
python -m venv .venv
.\.venv\Scripts\python -m pip install --upgrade pip
.\.venv\Scripts\python -m pip install -r requirements.txt
Write-Host "Done. From Testsuite/ run: .\0_Environment\.venv\Scripts\python -m pytest"
