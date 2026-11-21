<#
.SYNOPSIS
  Installs or removes gamecache in a game folder (for people building from source).
  Release ZIP users just copy the two files from the ZIP into the game folder.
.EXAMPLE
  .\install.ps1 -GameDir 'C:\Games\CONTROL Resonant' -GameProfile control-resonant
.EXAMPLE
  .\install.ps1 -GameDir 'D:\Games\SomeGame' -Patterns '*.pak' -Proxy xinput1_4
.EXAMPLE
  .\install.ps1 -GameDir 'C:\Games\CONTROL Resonant' -Uninstall
#>
param(
    [Parameter(Mandatory)] [string] $GameDir,
    [string] $GameProfile,
    [string] $Patterns = '',
    [string] $Proxy = 'xinput1_4',
    [switch] $Uninstall
)
$ErrorActionPreference = 'Stop'

function Test-IsGamecache([string] $path) {
    (Get-Item -LiteralPath $path).VersionInfo.ProductName -eq 'gamecache'
}

if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) { throw "Game folder not found: $GameDir" }
$ini = Join-Path $GameDir 'gamecache.ini'
$log = Join-Path $GameDir 'gamecache.log'

if ($Uninstall) {
    $found = Get-ChildItem -LiteralPath $GameDir -Filter '*.dll' -File | Where-Object { Test-IsGamecache $_.FullName }
    foreach ($f in @($found.FullName) + $ini + $log) {
        if ($f -and (Test-Path -LiteralPath $f)) { Remove-Item -LiteralPath $f; "removed: $f" }
    }
    return
}

$iniSource = $null
if ($GameProfile) {
    $iniSource = Join-Path $PSScriptRoot "profiles\$GameProfile.ini"
    if (-not (Test-Path $iniSource)) { throw "Profile not found: $iniSource" }
    $proxyLine = Select-String -LiteralPath $iniSource -Pattern '^;\s*proxy\s*=\s*(\S+)' | Select-Object -First 1
    if ($proxyLine) { $Proxy = $proxyLine.Matches[0].Groups[1].Value }
}

$src = Join-Path $PSScriptRoot "build\$Proxy.dll"
if (-not (Test-Path $src)) { throw "$src is missing - run: build.cmd $Proxy" }
$dll = Join-Path $GameDir "$Proxy.dll"
if ((Test-Path -LiteralPath $dll) -and -not (Test-IsGamecache $dll)) {
    throw "$dll already exists and is not gamecache - aborting so nothing of the game is overwritten."
}

Copy-Item -LiteralPath $src -Destination $dll -Force
"installed: $dll"
if (Test-Path -LiteralPath $ini) {
    "kept existing config: $ini"
} elseif ($iniSource) {
    Copy-Item -LiteralPath $iniSource -Destination $ini
    "config: $ini (profile $GameProfile)"
} else {
    "[gamecache]`r`nenabled=1`r`nlog=1`r`npatterns=$Patterns`r`n" | Set-Content -LiteralPath $ini -Encoding ascii -NoNewline
    "config: $ini"
}
Get-Content -LiteralPath $ini
