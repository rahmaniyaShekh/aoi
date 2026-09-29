# AOI installer - share this PC's audio over the internet.
#
# The repository is private, so the release is fetched with the GitHub CLI
# (logged in to an account that has access). One line, in PowerShell:
#
#   gh release download -R rahmaniyaShekh/aoi -p install.ps1 -O - | Out-String | iex
#
# It downloads aoi.exe from the latest release, installs it for this user
# (%LOCALAPPDATA%\Programs\AOI, on PATH, no admin rights) and starts sharing.
# Afterwards: aoi start / aoi status / aoi stop.
$ErrorActionPreference = 'Stop'
$repo = 'rahmaniyaShekh/aoi'
$dir = Join-Path $env:LOCALAPPDATA 'Programs\AOI'

function Fail($msg) { Write-Host "  $msg" -ForegroundColor Red; return }

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
  Fail 'GitHub CLI (gh) is needed:  winget install --id GitHub.cli   then: gh auth login'
  return
}
gh auth status *> $null
if ($LASTEXITCODE -ne 0) { Fail 'Log in to GitHub first:  gh auth login'; return }

$tmp = Join-Path $env:TEMP ("aoi-setup-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
Write-Host ''
Write-Host '  Downloading AOI from the latest release...' -ForegroundColor Cyan
gh release download -R $repo -p aoi.exe -D $tmp --clobber
if ($LASTEXITCODE -ne 0) { Fail "Could not download from $repo (no access to the repository?)"; return }
$exe = Join-Path $tmp 'aoi.exe'
Unblock-File -Path $exe -ErrorAction SilentlyContinue

# Installs (copies itself, PATH, Apps & features entry) and starts sharing.
& $exe install
$code = $LASTEXITCODE
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue

# Make `aoi` work in this window too, not only in new ones.
if (($env:Path -split ';') -notcontains $dir) { $env:Path = "$env:Path;$dir" }
if ($code -ne 0) { Fail 'Install did not finish. See the message above.' }
