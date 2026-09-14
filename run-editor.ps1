<#
.SYNOPSIS
    Incrementally build physics_editor and launch it.
.DESCRIPTION
    Calls the shared build entry build-editor.ps1 first, so the window is never
    started from a stale executable and there is no way to skip compiling. A
    failed build stops the script: it prints what to do (save and close an open
    editor window) and never falls back to an older physics_editor.exe. The
    script does not end any process.

    The program always runs from the repository root and its exit code is
    returned to the caller.
.PARAMETER DebugBuild
    Build and run the Debug configuration instead of Release.
.PARAMETER Project
    Optional .physproj path forwarded as --project. Relative paths resolve
    against the repository root, which is the working directory.
#>
[CmdletBinding()]
param(
    [switch]$DebugBuild,
    [string]$Project
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$buildEntry = Join-Path $root 'build-editor.ps1'
if (-not (Test-Path -LiteralPath $buildEntry)) {
    try { [Console]::Error.WriteLine("Missing shared build entry: $buildEntry") } catch { }
    exit 2
}

# Build first: a failed build must stop here instead of launching an old binary.
# The build entry already printed the cmake log and the "save and close the editor
# window" hint; report its message once, without a PowerShell error dump.
try {
    $executable = & $buildEntry -DebugBuild:$DebugBuild | Select-Object -Last 1
} catch {
    try { [Console]::Error.WriteLine("physics_editor was not started: $($_.Exception.Message)") } catch { }
    exit 1
}
if ([string]::IsNullOrWhiteSpace($executable) -or -not (Test-Path -LiteralPath $executable)) {
    try { [Console]::Error.WriteLine('physics_editor was not started: the build step did not return an executable path.') } catch { }
    exit 1
}

function ConvertTo-ArgumentString {
    # CommandLineToArgvW-compatible quoting, so a project path with spaces or
    # quotes survives as one argument.
    param([string]$Value)
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

$programArguments = @('--validation')
if (-not [string]::IsNullOrWhiteSpace($Project)) { $programArguments += @('--project', $Project) }

Push-Location $root
try {
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo.FileName = $executable
    $process.StartInfo.Arguments = (($programArguments | ForEach-Object { ConvertTo-ArgumentString $_ }) -join ' ')
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.WorkingDirectory = $root
    # WaitForExit() instead of the call operator: it waits for a GUI program
    # unconditionally and returns the real exit code to this script.
    try {
        [void]$process.Start()
    } catch {
        try { [Console]::Error.WriteLine("Cannot start physics_editor: $($_.Exception.Message)") } catch { }
        exit 1
    }
    $process.WaitForExit()
    $exitCode = $process.ExitCode
} finally {
    Pop-Location
}

exit $exitCode
