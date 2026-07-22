# Download WebView2 Fixed Version Runtime for self-contained browser engine (~150 MB)
# After running this, KiCad Forge will have a built-in browser with no system dependency.
#
# Quick start:  .\scripts\setup_webview2.ps1

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
$runtimeDir = Join-Path $projectRoot "webview2_runtime"

if (Test-Path (Join-Path $runtimeDir "msedgewebview2.exe")) {
    Write-Host "[OK] WebView2 runtime already bundled at webview2_runtime/"
    exit 0
}

Write-Host "=== KiCad Forge — WebView2 Runtime Setup ==="
Write-Host "Downloading self-contained browser engine (~150 MB, one-time)"
Write-Host ""

$tempDir = Join-Path $env:TEMP "kf_wv2_setup"
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null

# Microsoft's Evergreen bootstrapper — downloads the full runtime
$bootstrapperUrl = "https://go.microsoft.com/fwlink/p/?LinkId=2124703"
$setup = Join-Path $tempDir "MicrosoftEdgeWebview2Setup.exe"

Write-Host "[1/2] Downloading WebView2 Runtime installer..."
Invoke-WebRequest -Uri $bootstrapperUrl -OutFile $setup

Write-Host "[2/2] Installing WebView2 Runtime (silent)..."
$proc = Start-Process -FilePath $setup -ArgumentList "/install", "/silent" -Wait -PassThru -NoNewWindow

if ($proc.ExitCode -ne 0) {
    Write-Host "ERROR: Installer exited with code $($proc.ExitCode)"
    Write-Host "Manual download: https://developer.microsoft.com/microsoft-edge/webview2/"
    Remove-Item -Recurse -Force $tempDir -ErrorAction SilentlyContinue
    exit 1
}

# After Evergreen install, find the runtime and copy it
$edgePath = "${env:ProgramFiles(x86)}\Microsoft\EdgeWebView\Application"
if (Test-Path $edgePath) {
    $versions = Get-ChildItem $edgePath -Directory | Sort-Object Name -Descending
    if ($versions.Count -gt 0) {
        $latest = $versions[0].FullName
        Write-Host "Copying runtime from: $latest"
        # Copy the entire runtime folder (EBWebView contains the engine)
        Copy-Item -Path "$latest\*" -Destination $runtimeDir -Recurse -Force
    }
}

# Clean up
Remove-Item -Recurse -Force $tempDir -ErrorAction SilentlyContinue

if (Test-Path (Join-Path $runtimeDir "msedgewebview2.exe")) {
    $size = (Get-ChildItem -Path $runtimeDir -Recurse | Measure-Object -Property Length -Sum).Sum / 1MB
    Write-Host ""
    Write-Host "SUCCESS! WebView2 runtime bundled (~$([int]$size) MB)"
    Write-Host "KiCad Forge will now use the built-in browser engine."
    Write-Host "You can delete webview2_runtime/ to go back to system browser mode."
} else {
    Write-Host ""
    Write-Host "NOTE: Evergreen Runtime installed system-wide."
    Write-Host "To bundle with the app, download the Fixed Version from:"
    Write-Host "  https://developer.microsoft.com/microsoft-edge/webview2/"
    Write-Host "  (Choose 'Fixed Version', extract to webview2_runtime/)"
}
