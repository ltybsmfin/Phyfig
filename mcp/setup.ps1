param([switch]$RegisterCodex)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$environmentPath = Join-Path $projectRoot 'build/mcp-venv'
if (-not (Test-Path -LiteralPath (Join-Path $environmentPath 'Scripts/python.exe'))) {
    python -m venv $environmentPath
    if ($LASTEXITCODE -ne 0) { throw 'Cannot create Python environment' }
}
$pythonPath = Join-Path $environmentPath 'Scripts/python.exe'
& $pythonPath -m pip install -r (Join-Path $PSScriptRoot 'requirements-lock.txt')
if ($LASTEXITCODE -ne 0) { throw 'Cannot install MCP SDK' }
if ($RegisterCodex) {
    $executable = Join-Path $projectRoot 'build/vs2022/bin/editor/Release/physics_editor.exe'
    if (-not (Test-Path -LiteralPath $executable)) { throw 'Build the Release physics_editor target before registering' }
    codex mcp add physics-editor -- $pythonPath (Join-Path $PSScriptRoot 'server.py') --workspace $projectRoot --start-editor $executable
    if ($LASTEXITCODE -ne 0) { throw 'Codex MCP registration failed' }
}
