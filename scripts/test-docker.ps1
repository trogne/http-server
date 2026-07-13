$ErrorActionPreference = "Stop"
$projectDirectory = Split-Path -Parent $PSScriptRoot
Push-Location $projectDirectory

try {
    $dockerPath = (Get-Command docker -ErrorAction Stop).Source
    $standardOutput = New-TemporaryFile
    $standardError = New-TemporaryFile
    try {
        $healthCheck = Start-Process -FilePath $dockerPath -ArgumentList "info" -PassThru `
            -NoNewWindow -RedirectStandardOutput $standardOutput -RedirectStandardError $standardError
        if (-not $healthCheck.WaitForExit(15000)) {
            $healthCheck.Kill()
            $healthCheck.WaitForExit()
            throw "Docker Desktop's Linux engine did not respond within 15 seconds. Restart or update Docker Desktop, then retry."
        }
        if ($healthCheck.ExitCode -ne 0) {
            $details = (Get-Content -Raw $standardError).Trim()
            throw "Docker Desktop's Linux engine is unavailable. $details"
        }
    }
    finally {
        Remove-Item -LiteralPath $standardOutput, $standardError -Force -ErrorAction SilentlyContinue
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
