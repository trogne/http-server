$ErrorActionPreference = "Stop"
$projectDirectory = Split-Path -Parent $PSScriptRoot
Push-Location $projectDirectory

try {
    & docker info *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "Docker Desktop's Linux engine is unavailable. Start or restart Docker Desktop, then retry."
    }

    & docker build --target test -t pthread-http-test .
    if ($LASTEXITCODE -ne 0) {
        throw "Docker image build failed with exit code $LASTEXITCODE."
    }

    & docker run --rm pthread-http-test
    if ($LASTEXITCODE -ne 0) {
        throw "Docker tests failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
