param(
    [string]$Distro = "Ubuntu"
)

$ErrorActionPreference = "Stop"
$projectDirectory = Split-Path -Parent $PSScriptRoot
$portableWindowsPath = $projectDirectory.Replace("\", "/")
$linuxDirectory = (& wsl.exe -d $Distro -- wslpath -a $portableWindowsPath).Trim()
if ($LASTEXITCODE -ne 0 -or -not $linuxDirectory) {
    throw "Could not translate the project path for WSL distro '$Distro'."
}

& wsl.exe -d $Distro -- make -C $linuxDirectory clean test
if ($LASTEXITCODE -ne 0) {
    throw "WSL build or tests failed with exit code $LASTEXITCODE."
}
