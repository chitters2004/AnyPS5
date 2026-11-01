param(
    [Parameter(Mandatory = $true)][string]$Title,
    [Parameter(Mandatory = $true)][string]$Libs,
    [Parameter(Mandatory = $true)][ValidateSet("main", "off", "shadow", "gpu")][string]$Mode,
    [string]$Exe = "app.exe",
    [int]$TraceStart = 120,
    [int]$TraceLength = 30,
    [string]$Out = (Join-Path $PSScriptRoot "logs"),
    [string]$Tag = ""
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$exePath = Join-Path $Title $Exe
if (-not (Test-Path $exePath)) { throw "no $exePath" }
$titleLibs = Join-Path $Title "libs"
if (-not (Test-Path $titleLibs)) { throw "no $titleLibs (the converted title's libs folder)" }
if (-not (Test-Path (Join-Path $Libs "libSceAgcDriver.prx"))) { throw "no libSceAgcDriver.prx in $Libs (pass <tree>\build\core\libs\libs)" }

Write-Host "deploying $Libs -> $titleLibs"
Get-ChildItem -Path $Libs -File | Where-Object { $_.Extension -in ".prx", ".dll" } | Copy-Item -Destination $titleLibs -Force

$switches = @("APS5_LABEL_GPU_COMPARE", "APS5_LABEL_COMPARE_SHADOW", "APS5_PROFILE_DRAW", "APS5_TRACE_FRAMES", "APS5_TRACE_SECONDS", "APS5_TRACE_DIR")
foreach ($variable in $switches) { Remove-Item -Path "Env:$variable" -ErrorAction SilentlyContinue }
$env:APS5_TRACE_SECONDS = "$($TraceStart):$($TraceLength)"
$env:APS5_TRACE_DIR = $Out
switch ($Mode) {
    "shadow" { $env:APS5_LABEL_COMPARE_SHADOW = "1" }
    "gpu" { $env:APS5_LABEL_GPU_COMPARE = "1" }
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$name = if ($Tag -ne "") { "gta-$Mode-$Tag-$stamp" } else { "gta-$Mode-$stamp" }
$log = Join-Path $Out "$name.log"
$outLog = Join-Path $Out "$name.stdout.log"
Write-Host "running $exePath ($Mode); trace window $($TraceStart)s +$($TraceLength)s; stderr -> $log"
Write-Host "go to the same scene as the other runs before the window opens, keep still, then quit the game"
try {
    $process = Start-Process -FilePath $exePath -WorkingDirectory $Title -RedirectStandardError $log -RedirectStandardOutput $outLog -PassThru -Wait
    Write-Host "exit code $($process.ExitCode)"
} finally {
    foreach ($variable in $switches) { Remove-Item -Path "Env:$variable" -ErrorAction SilentlyContinue }
}
