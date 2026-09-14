<#
.SYNOPSIS
    Build physics_editor and start the Python MCP server on stdio.
.DESCRIPTION
    One command that replaces the copy-pasted python command line: it calls the
    shared build entry build-editor.ps1 first, then runs

        build/mcp-venv/Scripts/python.exe mcp/server.py ^
            --workspace <repo root> --editor-transport stdio --start-editor <exe>

    Stdout is the MCP protocol stream and nothing else. Build progress and all
    diagnostics are written to stderr, so the script can be registered directly as
    an MCP server command. The editor is started by mcp/server.py over the stdio
    transport: it is its own visible window and never attaches to an existing
    named pipe.

    Standard input, standard output and standard error of the Python process are
    inherited, not redirected or captured: the host's MCP pipes reach the server
    unchanged.

    Register it with a host as (note -NoProfile and -File: a profile script that
    writes to stdout would corrupt the protocol stream):

        powershell -NoProfile -ExecutionPolicy Bypass -File <repo>\run-editor-mcp.ps1

.PARAMETER DebugBuild
    Build and run the Debug configuration instead of Release.
#>
[CmdletBinding()]
param(
    [switch]$DebugBuild
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$buildEntry = Join-Path $root 'build-editor.ps1'
$server = Join-Path $root 'mcp/server.py'
$python = Join-Path (Join-Path $root 'build/mcp-venv') 'Scripts/python.exe'

# Diagnostic output must never reach stdout: MCP owns it. Console.Error writes
# straight to the process stderr handle and is wrapped because a closed stderr
# must not break the server.
function Write-McpDiagnostic {
    param([string]$Message)
    try { [Console]::Error.WriteLine("[run-editor-mcp] $Message") } catch { }
}

# Chinese paths and messages: prefer UTF-8 for this process' text streams.
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }

if (-not (Test-Path -LiteralPath $python)) {
    Write-McpDiagnostic "MCP Python environment not found: $python"
    Write-McpDiagnostic 'Create it once with: powershell -NoProfile -ExecutionPolicy Bypass -File .\mcp\setup.ps1'
    Write-McpDiagnostic '(setup.ps1 creates build/mcp-venv and installs mcp/requirements-lock.txt)'
    exit 2
}
if (-not (Test-Path -LiteralPath $server)) {
    Write-McpDiagnostic "MCP server script not found: $server"
    exit 2
}
if (-not (Test-Path -LiteralPath $buildEntry)) {
    Write-McpDiagnostic "Missing shared build entry: $buildEntry"
    exit 2
}

function ConvertTo-ArgumentString {
    # CommandLineToArgvW-compatible quoting, so a workspace path with spaces
    # survives as one argument (Start-Process would need the same, and on
    # Windows PowerShell 5.1 "Start-Process -Wait -PassThru" is unreliable).
    param([string]$Value)
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

# Same build entry as run-editor.ps1. -LogToStandardError routes every line to
# stderr, so a build failure is reported without touching the protocol stream.
try {
    $executable = & $buildEntry -DebugBuild:$DebugBuild -LogToStandardError | Select-Object -Last 1
} catch {
    Write-McpDiagnostic "physics_editor build failed, the MCP server was not started: $($_.Exception.Message)"
    exit 1
}
if ([string]::IsNullOrWhiteSpace($executable) -or -not (Test-Path -LiteralPath $executable)) {
    Write-McpDiagnostic "The build step did not return a physics_editor executable path."
    exit 1
}

$serverArguments = @(
    '--workspace', $root,
    '--editor-transport', 'stdio',
    '--start-editor', $executable
)
Write-McpDiagnostic "editor: $executable"
Write-McpDiagnostic "python: $python"
Write-McpDiagnostic 'transport: stdio (the server starts its own visible editor window; no named pipe is attached)'

$process = New-Object System.Diagnostics.Process
$process.StartInfo.FileName = $python
$argumentList = @($server) + $serverArguments
$process.StartInfo.Arguments = (($argumentList | ForEach-Object { ConvertTo-ArgumentString $_ }) -join ' ')
$process.StartInfo.UseShellExecute = $false
# Keep inherited standard handles. CreateNoWindow can discard them on Windows
# and leave the MCP client waiting forever for its initialize response.
$process.StartInfo.WorkingDirectory = $root
try {
    # No redirection: the child inherits this process' stdin/stdout/stderr, so
    # PowerShell never decodes, re-encodes or buffers the MCP protocol bytes.
    [void]$process.Start()
} catch {
    Write-McpDiagnostic "Cannot start the MCP server: $($_.Exception.Message)"
    exit 2
}
$process.WaitForExit()
exit $process.ExitCode
