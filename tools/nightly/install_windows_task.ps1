<#
.SYNOPSIS
Registers the Windows Task Scheduler entry for the HydrogenVM nightly corpus run.

.DESCRIPTION
Runs daily at 03:17 (with WakeToRun enabled) via `wsl.exe -e bash -lc ...` over both
Lawson's Desktop IPA and APK folders, passing `--publish` (which uses `gh auth` inside WSL;
no GitHub tokens are stored in the repository or task arguments).

.EXAMPLE
powershell -ExecutionPolicy Bypass -File tools\nightly\install_windows_task.ps1
#>
[CmdletBinding()]
param(
    [string]$TaskName = "HydrogenVM Nightly",
    [string]$Time = "03:17",
    [string]$Distro = "",
    [string]$RepoPath = "",
    [string]$IpaDir = "",
    [string]$ApkDir = "",
    [switch]$NoPublish
)

$ErrorActionPreference = "Stop"

function Convert-ToWslPath([string]$WinPath) {
    $args = @()
    if ($Distro) { $args += @("-d", $Distro) }
    $args += @("-e", "wslpath", "-a", $WinPath)
    $out = & wsl.exe @args
    if ($LASTEXITCODE -ne 0 -or -not $out) {
        throw "Failed to convert Windows path to WSL path: $WinPath"
    }
    return ($out | Select-Object -First 1).Trim()
}

if (-not $RepoPath) {
    $winRepo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
    $RepoPath = Convert-ToWslPath $winRepo
}
if (-not $IpaDir) {
    $winIpa = Join-Path $env:USERPROFILE "Desktop\IPA"
    if (-not (Test-Path $winIpa)) {
        $fallback = Join-Path $env:USERPROFILE "Desktop\test_ipa"
        if (Test-Path $fallback) { $winIpa = $fallback }
    }
    $IpaDir = Convert-ToWslPath $winIpa
}
if (-not $ApkDir) {
    $winApk = Join-Path $env:USERPROFILE "Desktop\APK"
    $ApkDir = Convert-ToWslPath $winApk
}

$publishFlag = if ($NoPublish) { "" } else { " --publish" }
$bashCmd = "cd '$RepoPath' && bash tools/nightly/run_nightly.sh '$IpaDir' '$ApkDir'$publishFlag"
$wslArgs = if ($Distro) { "-d `"$Distro`" -e bash -lc `"$bashCmd`"" } else { "-e bash -lc `"$bashCmd`"" }

$action = New-ScheduledTaskAction -Execute "wsl.exe" -Argument $wslArgs
$trigger = New-ScheduledTaskTrigger -Daily -At $Time
$settings = New-ScheduledTaskSettingsSet `
    -WakeToRun `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null

Write-Host "Registered scheduled task '$TaskName' (daily at $Time, WakeToRun enabled)."
Write-Host "Command: wsl.exe $wslArgs"
Write-Host "Note: '--publish' uses 'gh auth' inside WSL; run 'wsl.exe -e gh auth status' to verify login."
