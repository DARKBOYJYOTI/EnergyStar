#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'
$destination = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'EnergyStar'
$payload = Join-Path $PSScriptRoot 'publish'

if (-not (Test-Path (Join-Path $payload 'EnergyStar.UI.exe'))) {
    throw "Published EnergyStar files were not found in '$payload'."
}

New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item -Path (Join-Path $payload '*') -Destination $destination -Recurse -Force
$installedFiles = Get-ChildItem -Path $payload -File -Recurse | ForEach-Object {
    [System.IO.Path]::GetRelativePath($payload, $_.FullName)
}
$installedFiles | ConvertTo-Json | Set-Content -Path (Join-Path $destination 'EnergyStar.install-manifest.json') -Encoding UTF8
Write-Host "EnergyStar installed to $destination"
Write-Host 'Launch EnergyStar.UI.exe. Startup registration is available in the app.'
