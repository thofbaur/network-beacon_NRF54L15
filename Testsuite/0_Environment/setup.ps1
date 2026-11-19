# Windows: create the test environment in Testsuite/0_Environment/.venv
# and install the git pre-commit hook (regression check before every commit).
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
python -m venv .venv
.\.venv\Scripts\python -m pip install --upgrade pip
.\.venv\Scripts\python -m pip install -r requirements.txt

$hooks = $null
try { $hooks = git rev-parse --git-path hooks 2>$null; if ($LASTEXITCODE -ne 0) { $hooks = $null } } catch {}
if ($hooks) {
    $hooks = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($PSScriptRoot, $hooks))
    $target = Join-Path $hooks "pre-commit"
    if ((Test-Path $target) -and ((Get-FileHash $target).Hash -ne (Get-FileHash pre-commit).Hash)) {
        Copy-Item $target "$target.bak" -Force
        Write-Host "Existing pre-commit hook saved as $target.bak"
    }
    New-Item -ItemType Directory -Force $hooks | Out-Null
    Copy-Item pre-commit $target -Force
    Write-Host "Installed pre-commit hook: $target"
} else {
    Write-Host "Not a git checkout: pre-commit hook not installed."
}
Write-Host "Done. From Testsuite/ run: .\0_Environment\.venv\Scripts\python -m pytest"
