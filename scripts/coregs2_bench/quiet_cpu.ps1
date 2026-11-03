# TASK-134: the whole process's CPU and wall time of the recorded lap with no report switched on, as in a player's session: the
# clock reads and scopes that the reports need are off, so no report can show their gain. Interleaved over candidates.
# usage: quiet_cpu.ps1 -Exes a=<exe>,b=<exe> [-Rounds 2] [-Scale 1] [-Last 8032] [-Assets C:\Antigravity\gt4-staticrecomp] [-Out <folder>]
param(
    [Parameter(Mandatory = $true)][string[]]$Exes,
    [int]$Rounds = 2,
    [int]$Scale = 1,
    [int]$Last = 8032,
    [string]$Assets = 'C:\Antigravity\gt4-staticrecomp',
    [string]$Iso = '',
    [string]$Elf = '',
    [string]$Card = '',
    [string]$Recording = '',
    [string]$Out = "$env:TEMP\gt4-quiet"
)
$ErrorActionPreference = 'Stop'
if (!$Iso) { $Iso = "$Assets\Gran Turismo 4 Spec II v1.10 (PTBR).iso" }
if (!$Elf) { $Elf = "$Assets\private\slice272-spec2-elf\SCUS_974.36" }
if (!$Card) { $Card = "$Assets\private\slice110-menu-pilot\name-control4\card.ps2" }
if (!$Recording) { $Recording = "$Assets\private\coregs2-bench\inputs\george-volta2.txt" }
New-Item -ItemType Directory -Force $Out | Out-Null
$pairs = @($Exes | ForEach-Object { $_ -split ',' })
$env:GT4_RENDERER = 'hw'; $env:GT4_HW_CORE = '2'; $env:GT4_FORCE_480P = '1'; $env:GT4_FORCE_WIDESCREEN = '1'; $env:GT4_DISPLAY_ASPECT = '16:9'
$env:GT4_HW_SCALE = "$Scale"; $env:GT4_INPUT_REPLAY = $Recording
# A player's session reads its pipelines from the launcher's shader cache; without one every run compiled CoreGS 2.0's
# specialised pipelines during the lap (2026-11-03). Each executable gets its own cache, filled by --prepare-shaders and one
# unmeasured lap the first time.
function Get-ShaderCache([string]$exe) { Join-Path $Out ('shader-cache\' + (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.Substring(0, 16).ToLower()) }
function Start-Lap([string]$exe, [string]$tag) {
    $runCard = Join-Path $Out "$tag.ps2"
    Copy-Item -LiteralPath $Card $runCard -Force
    $arguments = @($Elf, '--ntsc', '--disc', $Iso, '--frames', "$Last", '--memcard', $runCard) | ForEach-Object { '"' + $_ + '"' }
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $Out "$tag.out.txt") -RedirectStandardError (Join-Path $Out "$tag.err.txt")
    $null = $process.Handle  # keeps the handle, so the times can be read after the exit
    return $process
}
foreach ($pair in $pairs) {
    $tag, $exe = $pair.Split('=', 2)
    $env:GT4_HW_SHADER_CACHE = Get-ShaderCache $exe
    if (!(Test-Path (Join-Path $env:GT4_HW_SHADER_CACHE 'variants.bin'))) {
        New-Item -ItemType Directory -Force $env:GT4_HW_SHADER_CACHE | Out-Null
        Start-Process -FilePath $exe -ArgumentList "--prepare-shaders" -NoNewWindow -Wait -RedirectStandardOutput (Join-Path $Out 'prep.out') -RedirectStandardError (Join-Path $Out 'prep.err')
        $warm = Start-Lap $exe "$tag-warm"
        $warm.WaitForExit()
        Remove-Item (Join-Path $Out "$tag-warm.ps2") -Force
    }
}
$totals = @{}
foreach ($round in 1..$Rounds) {
    foreach ($pair in $pairs) {
        $tag, $exe = $pair.Split('=', 2)
        $env:GT4_HW_SHADER_CACHE = Get-ShaderCache $exe
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $process = Start-Lap $exe $tag
        $process.WaitForExit()
        $watch.Stop()
        $cpu = $process.TotalProcessorTime.TotalMilliseconds / $Last
        $wall = $watch.Elapsed.TotalMilliseconds / $Last
        $compiled = Select-String -LiteralPath (Join-Path $Out "$tag.err.txt") -Pattern 'compile_ms=[0-9.]*' -AllMatches | Select-Object -Last 1
        $compiled = if ($compiled) { $compiled.Matches[-1].Value } else { 'compile_ms=?' }
        Write-Output ('{0} round {1}: process CPU {2:N3} ms/VBlank, wall {3:N3} ms/VBlank, exit {4}, {5}' -f $tag, $round, $cpu, $wall, $process.ExitCode, $compiled)
        Remove-Item (Join-Path $Out "$tag.ps2") -Force
    }
}
