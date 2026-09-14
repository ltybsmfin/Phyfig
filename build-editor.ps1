<#
.SYNOPSIS
    Single shared build entry for the physics_editor target.
.DESCRIPTION
    Configures the checked-in vs2022 CMake preset and then incrementally builds
    the physics_editor target with the release (default) or debug build preset.
    Debug and Release are two configurations of the same sources, so every entry
    point (run-editor.ps1, run-editor-mcp.ps1, the VS Code editor task) calls this
    file instead of repeating cmake commands.

    A non-zero cmake exit code stops this script with an error, so a caller can
    never continue into launching a stale executable. This script never ends a
    process: when the build fails because physics_editor.exe is still running and
    therefore locked by the linker, the error says that the editor window must be
    saved and closed first.

    The only object written to the success stream is the absolute path of the
    built executable, so callers can do:

        $exe = & "$PSScriptRoot/build-editor.ps1" -DebugBuild:$DebugBuild | Select-Object -Last 1

    All progress goes to the host (Write-Host) by default, or to stderr with
    -LogToStandardError, which is what the MCP launcher needs: MCP owns stdout.
.PARAMETER DebugBuild
    Build the Debug configuration (debug preset) instead of Release.
.PARAMETER LogToStandardError
    Write progress to the standard error stream instead of the host.
#>
[CmdletBinding()]
param(
    [switch]$DebugBuild,
    [switch]$LogToStandardError
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$configuration = if ($DebugBuild) { 'Debug' } else { 'Release' }
$preset = if ($DebugBuild) { 'debug' } else { 'release' }
$executable = Join-Path (Join-Path (Join-Path $root 'build/vs2022/bin/editor') $configuration) 'physics_editor.exe'

function Write-BuildLog {
    param([string]$Message)
    if ($LogToStandardError) {
        try { [Console]::Error.WriteLine($Message) } catch { }
    } else {
        Write-Host $Message
    }
}

function Get-LockedExecutableHint {
    # Diagnostic only. The script deliberately does not stop any process: the
    # user has to save the open project and close the editor window, because a
    # running executable cannot be replaced by the linker.
    $running = @()
    try { $running = @(Get-Process -Name 'physics_editor' -ErrorAction SilentlyContinue) } catch { $running = @() }
    if ($running.Count -gt 0) {
        return ("$($running.Count) running physics_editor process(es) were detected. Save the open project(s) and close " +
                'the editor window(s), then run the command again. ')
    }
    return ('If an editor window is still open, save the project and close the window, then run the command again. ')
}

function Invoke-CMakeStep {
    param(
        [Parameter(Mandatory = $true)][string]$Step,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    Write-BuildLog "== cmake $Step"
    Write-BuildLog "   cmake $($Arguments -join ' ')"
    $previousErrorAction = $ErrorActionPreference
    $hadNativePreference = $false
    $previousNativePreference = $null
    # PSNativeCommandUseErrorActionPreference exists from PowerShell 7.3 on; it
    # must not turn a native command's stderr into a terminating error here.
    $nativePreference = Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue
    if ($null -ne $nativePreference) {
        $hadNativePreference = $true
        $previousNativePreference = $nativePreference.Value
    }
    $exitCode = 0
    $output = @()
    try {
        # Windows PowerShell 5.1 turns a native command's stderr into error
        # records; with ErrorActionPreference=Stop a warning on a successful
        # build would abort the script. Capture both streams and trust
        # $LASTEXITCODE, which is the actual contract with cmake.
        $ErrorActionPreference = 'Continue'
        $PSNativeCommandUseErrorActionPreference = $false
        $output = & cmake @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } catch {
        throw "Cannot run cmake ($($_.Exception.Message)). Install CMake 3.24+ and make sure it is on PATH."
    } finally {
        $ErrorActionPreference = $previousErrorAction
        if ($hadNativePreference) {
            $PSNativeCommandUseErrorActionPreference = $previousNativePreference
        } else {
            Remove-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue
        }
    }
    foreach ($line in @($output)) { Write-BuildLog ("   " + [string]$line) }
    if ($exitCode -ne 0) {
        throw ("physics_editor build failed: cmake $Step exited with code $exitCode. " +
               (Get-LockedExecutableHint) +
               'This script never ends a process and never falls back to an older executable.')
    }
    Write-BuildLog "== cmake $Step finished (exit code 0)"
}

# The repository root is the working directory for both preset resolution and
# the generated build tree.
Push-Location $root
try {
    Invoke-CMakeStep -Step 'configure (--preset vs2022)' -Arguments @('--preset', 'vs2022')
    Invoke-CMakeStep -Step "build (--preset $preset --target physics_editor)" -Arguments @('--build', '--preset', $preset, '--target', 'physics_editor')
} finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $executable)) {
    throw "cmake reported success but $executable does not exist. Check the output directory in CMakePresets.json / CMakeLists.txt."
}
Write-BuildLog "== $configuration executable: $executable"

# The single object on the success stream: callers use it as the executable path.
Write-Output $executable
