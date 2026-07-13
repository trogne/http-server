$ErrorActionPreference = "Stop"

$projectDirectory = Split-Path -Parent $PSScriptRoot
Push-Location $projectDirectory

try {
    $dockerPath = (Get-Command docker -ErrorAction Stop).Source

    Write-Host "Checking Docker Desktop..."

    $serverOperatingSystem = & $dockerPath info --format '{{.OSType}}' 2>&1

    if ($LASTEXITCODE -ne 0) {
        throw "Docker Desktop's engine is unavailable. $serverOperatingSystem"
    }

    $serverOperatingSystem = "$serverOperatingSystem".Trim()

    if ($serverOperatingSystem -ne "linux") {
        throw "Docker is running, but its active engine is '$serverOperatingSystem' instead of 'linux'."
    }

    Write-Host "Docker Desktop's Linux engine is running."

    & $dockerPath build --target test -t pthread-http-test .

    if ($LASTEXITCODE -ne 0) {
        throw "Docker image build failed with exit code $LASTEXITCODE."
    }

    & $dockerPath run --rm pthread-http-test

    if ($LASTEXITCODE -ne 0) {
        throw "Docker tests failed with exit code $LASTEXITCODE."
    }

    Write-Host "Docker tests completed successfully."
}
finally {
    Pop-Location
}
