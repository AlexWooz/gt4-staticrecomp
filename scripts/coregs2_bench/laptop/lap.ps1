# TASK-134: the owner's recorded lap (George V Paris, Spec II) headless on the notebook, interleaved, for an A/B of candidates. Started
# over SSH; each candidate is a folder bench\t134\<tag> with gt4vulkan-spec2.exe and its DLLs. Nothing in user\ or data\ is written:
# the card is a copy, the logs go to bench\t134\logs\<run>\log.txt (the layout cpu_summary.py reads).
# usage: lap.ps1 -Tags a,b [-Rounds 2] [-Scale 1] [-Performance] [-Prefix ""] [-Last 8032] [-Sampler <tag>]
param(
    [Parameter(Mandatory = $true)][string[]]$Tags,
    [int]$Rounds = 2,
    [int]$Scale = 1,
    [switch]$Performance,
    [string]$Prefix = '',
    [int]$Last = 8032,
    [string]$Sampler = ''
)
$ErrorActionPreference = 'Stop'
$Tags = @($Tags | ForEach-Object { $_ -split ',' })
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$elf = Join-Path $root 'data\games\spec2\prepared\SCUS_974.36'
$iso = Get-Content (Join-Path $PSScriptRoot 'spec2-iso.txt') -Raw
$iso = $iso.Trim()
$recording = Join-Path $PSScriptRoot 'george-volta2.txt'
$card = Join-Path $PSScriptRoot 'card-base.ps2'
foreach ($path in @($elf, $iso, $recording, $card)) { if (!(Test-Path -LiteralPath $path)) { throw "missing: $path" } }
if (Get-Process gt4, gt4-spec2, gt4vulkan-spec2, GT4Recomp -ErrorAction SilentlyContinue) { throw 'a game or the launcher is running' }

$environment = @{
    GT4_RENDERER = 'hw'; GT4_HW_CORE = '2'; GT4_FORCE_480P = '1'; GT4_FORCE_WIDESCREEN = '1'; GT4_DISPLAY_ASPECT = '16:9'
    GT4_HW_SCALE = "$Scale"; GT4_INPUT_REPLAY = $recording; GT4_MTVU_REPORT_EVERY = '600'; GT4_HW_PROFILE_EVERY = '600'
}
if ($Performance) { $environment['GT4_CORE2_PERFORMANCE'] = '1' }
foreach ($key in $environment.Keys) { Set-Item "env:$key" $environment[$key] }

# A player's session reads its pipelines from the launcher's shader cache; without one every run compiled CoreGS 2.0's
# specialised pipelines during the lap (2026-11-03). Each candidate gets its own cache, filled by --prepare-shaders and one
# unmeasured lap the first time (a candidate folder replaced with a new build needs its cache folder removed).
function Get-ShaderCache([string]$tag) { Join-Path $PSScriptRoot "shader-cache\$tag" }
foreach ($tag in $Tags) {
    $exe = Join-Path $PSScriptRoot "$tag\gt4vulkan-spec2.exe"
    $env:GT4_HW_SHADER_CACHE = Get-ShaderCache $tag
    if (Test-Path (Join-Path $env:GT4_HW_SHADER_CACHE 'variants.bin')) { continue }
    New-Item -ItemType Directory -Force $env:GT4_HW_SHADER_CACHE | Out-Null
    Start-Process -FilePath $exe -ArgumentList "--prepare-shaders" -NoNewWindow -Wait -RedirectStandardOutput (Join-Path $env:TEMP 'gt4-prep.out') -RedirectStandardError (Join-Path $env:TEMP 'gt4-prep.err')
    $warmCard = Join-Path $env:TEMP 'gt4-warm.ps2'
    Copy-Item -LiteralPath $card $warmCard -Force
    $arguments = @($elf, '--ntsc', '--disc', $iso, '--frames', "$Last", '--memcard', $warmCard)
    Start-Process -FilePath $exe -ArgumentList ($arguments | ForEach-Object { '"' + $_ + '"' }) -NoNewWindow -Wait `
        -RedirectStandardOutput (Join-Path $env:TEMP 'gt4-warm.out') -RedirectStandardError (Join-Path $env:TEMP 'gt4-warm.err') | Out-Null
    Remove-Item $warmCard -Force
    Write-Output "warmed the shader cache of $tag"
}
foreach ($round in 1..$Rounds) {
    foreach ($tag in $Tags) {
        $run = "$Prefix$tag$round"
        $out = Join-Path $PSScriptRoot "logs\$run"
        New-Item -ItemType Directory -Force $out | Out-Null
        $runCard = Join-Path $out 'card.ps2'
        Copy-Item -LiteralPath $card $runCard -Force
        $exe = Join-Path $PSScriptRoot "$tag\gt4vulkan-spec2.exe"
        $env:GT4_HW_SHADER_CACHE = Get-ShaderCache $tag
        if ($Sampler -eq $tag -and $round -eq 1) {
            $env:GT4_CPU_SAMPLER = Join-Path $out 'sampler.bin'; $env:GT4_CPU_SAMPLER_FROM = '2400'; $env:GT4_CPU_SAMPLER_TO = '6000'
        } else {
            Remove-Item env:GT4_CPU_SAMPLER -ErrorAction SilentlyContinue
        }
        $arguments = @($elf, '--ntsc', '--disc', $iso, '--frames', "$Last", '--memcard', $runCard)
        $process = Start-Process -FilePath $exe -ArgumentList ($arguments | ForEach-Object { '"' + $_ + '"' }) -NoNewWindow -PassThru -Wait `
            -RedirectStandardOutput (Join-Path $out 'log.txt') -RedirectStandardError (Join-Path $out 'err.txt')
        Get-Content (Join-Path $out 'err.txt') | Add-Content (Join-Path $out 'log.txt')
        Remove-Item (Join-Path $out 'err.txt'), $runCard -Force
        Write-Output "finished $run exit $($process.ExitCode)"
    }
}
