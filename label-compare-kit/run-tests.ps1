param(
    [Parameter(Mandatory = $true)][string]$Branch,
    [Parameter(Mandatory = $true)][string]$Main,
    [string]$BuildDir = "build",
    [switch]$Wide,
    [string]$Out = (Join-Path $PSScriptRoot "logs"),
    [int]$Jobs = 4
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$kit = $PSScriptRoot
$targetsFile = if ($Wide) { "wide-targets.txt" } else { "focused-targets.txt" }
$testsFile = if ($Wide) { "wide-tests.txt" } else { "focused-tests.txt" }
$targets = Get-Content (Join-Path $kit $targetsFile) | Where-Object { $_ -ne "" }
$tests = (Get-Content (Join-Path $kit $testsFile) -Raw).Trim()
$branchOnly = @("agc_label_compare_tests")
$validation = @{
    "ANYPS5_REQUIRE_VULKAN" = "1"
    "ANYPS5_NO_SHADER_CACHE" = "1"
    "VK_INSTANCE_LAYERS" = "VK_LAYER_KHRONOS_validation"
}

function Build-Tests([string]$tree, [string[]]$names) {
    $build = Join-Path $tree $BuildDir
    Write-Host "building $($names.Count) test targets in $build"
    & cmake --build $build --parallel $Jobs --target @names
    if ($LASTEXITCODE -ne 0) { throw "build failed in $build" }
}

function Run-Tests([string]$tree, [string]$log, [hashtable]$extra) {
    $build = Join-Path $tree $BuildDir
    $all = $validation.Clone()
    foreach ($key in $extra.Keys) { $all[$key] = $extra[$key] }
    foreach ($key in $all.Keys) { Set-Item -Path "Env:$key" -Value $all[$key] }
    try {
        Write-Host "ctest in $build -> $log"
        & ctest --test-dir $build -R $tests -V --timeout 600 *> $log
    } finally {
        foreach ($key in $all.Keys) { Remove-Item -Path "Env:$key" -ErrorAction SilentlyContinue }
    }
    $passed = Select-String -Path $log -Pattern "tests passed, " | Select-Object -Last 1
    Write-Host "  $($passed.Line)"
}

Build-Tests $Main ($targets | Where-Object { $branchOnly -notcontains $_ })
Build-Tests $Branch $targets
$suffix = if ($Wide) { "wide" } else { "focused" }
$mainLog = Join-Path $Out "tests-$suffix-main.log"
$offLog = Join-Path $Out "tests-$suffix-branch-off.log"
$onLog = Join-Path $Out "tests-$suffix-branch-on.log"
Run-Tests $Main $mainLog @{}
Run-Tests $Branch $offLog @{}
Run-Tests $Branch $onLog @{ "APS5_LABEL_GPU_COMPARE" = "1" }
& py -3 (Join-Path $kit "summarize.py") tests $mainLog $offLog $onLog
exit $LASTEXITCODE
