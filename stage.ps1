# stage.ps1 — collect build outputs for the installer.
param([string]$Config = "RelWithDebInfo")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$staging = Join-Path $PSScriptRoot "staging"
New-Item -ItemType Directory -Force -Path $staging | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $staging "data") | Out-Null

Copy-Item (Join-Path $root "build\$Config\obs-dji-uvc.dll") $staging -Force
Copy-Item (Join-Path $root "data\*") (Join-Path $staging "data") -Recurse -Force

# libusb DLL if the build produced a shared one
$usb = Get-ChildItem -Path (Join-Path $root "build") -Recurse -Filter "libusb-1.0.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($usb) { Copy-Item $usb.FullName $staging -Force }

Write-Host "Staged to $staging"
