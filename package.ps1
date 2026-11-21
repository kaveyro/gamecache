<#
.SYNOPSIS
  Builds the release ZIP for one game profile into dist\.
.EXAMPLE
  .\package.ps1 -GameProfile control-resonant -Game 'Control Resonant' -Exe CONTROLResonant.exe
#>
param(
    [Parameter(Mandatory)] [string] $GameProfile,
    [Parameter(Mandatory)] [string] $Game,
    [Parameter(Mandatory)] [string] $Exe,
    [string] $RepoUrl = 'https://github.com/kaveyro/gamecache'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$iniSource = Join-Path $root "profiles\$GameProfile.ini"
if (-not (Test-Path $iniSource)) { throw "Profile not found: $iniSource" }
$proxy = (Select-String -LiteralPath $iniSource -Pattern '^;\s*proxy\s*=\s*(\S+)').Matches[0].Groups[1].Value
$dll = Join-Path $root "build\$proxy.dll"
if (-not (Test-Path $dll)) { throw "$dll is missing - run: build.cmd $proxy" }
$version = (Get-Item $dll).VersionInfo.ProductVersion

$stage = Join-Path $root "dist\stage-$GameProfile"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $dll (Join-Path $stage "$proxy.dll")
Copy-Item $iniSource (Join-Path $stage 'gamecache.ini')
(Get-Content (Join-Path $root 'packaging\INSTALL.txt') -Raw) `
    -replace '\{VERSION\}', $version -replace '\{GAME\}', $Game -replace '\{PROXY\}', $proxy `
    -replace '\{EXE\}', $Exe -replace '\{REPO_URL\}', $RepoUrl |
    Set-Content (Join-Path $stage 'INSTALL.txt') -Encoding utf8
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')

$zip = Join-Path $root "dist\gamecache-$version-$GameProfile.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item $stage -Recurse -Force
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path $zip -Leaf)" | Set-Content "$zip.sha256" -Encoding ascii
"packaged: $zip"
"sha256:   $hash"
