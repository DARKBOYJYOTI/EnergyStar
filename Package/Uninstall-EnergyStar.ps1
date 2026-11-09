$ErrorActionPreference = 'Stop'
$destination = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'EnergyStar'
$schtasks = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::System)) 'schtasks.exe'

& $schtasks /Query /TN '\EnergyStar' *> $null
if ($LASTEXITCODE -eq 0) {
    $taskProcess = Start-Process -FilePath $schtasks -ArgumentList '/Delete /TN "\EnergyStar" /F' -Verb RunAs -Wait -PassThru
    if ($taskProcess.ExitCode -ne 0) {
        throw "Could not remove EnergyStar startup task (exit code $($taskProcess.ExitCode))."
    }
}
else {
    Write-Host 'No EnergyStar startup task was registered.'
}

if (Test-Path $destination) {
    $manifestPath = Join-Path $destination 'EnergyStar.install-manifest.json'
    if (Test-Path $manifestPath) {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $root = [System.IO.Path]::GetFullPath($destination).TrimEnd('\') + '\'
        foreach ($relativePath in $manifest) {
            $filePath = [System.IO.Path]::GetFullPath((Join-Path $destination $relativePath))
            if ($filePath.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $filePath -PathType Leaf)) {
                Remove-Item -LiteralPath $filePath -Force
            }
        }
        Remove-Item -LiteralPath $manifestPath -Force
        Write-Host "Removed EnergyStar program files from $destination"
    }
    else {
        Write-Warning 'Install manifest missing; kept install folder to protect user files.'
    }
}

Write-Host 'EnergyStar.log and EnergyStar.xml are preserved. Per-user settings remain under LocalAppData.'
