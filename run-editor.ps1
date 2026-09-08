param([switch]$DebugBuild, [string]$Project)
$ErrorActionPreference = 'Stop'
$configuration = if ($DebugBuild) { 'Debug' } else { 'Release' }
$executable = Join-Path $PSScriptRoot "build/vs2022/bin/editor/$configuration/physics_editor.exe"
if (-not (Test-Path -LiteralPath $executable)) {
    throw "Please build physics_editor ($configuration) with the checked-in CMake preset first."
}
Push-Location $PSScriptRoot
try { if ($Project) { & $executable --validation --project $Project } else { & $executable --validation } } finally { Pop-Location }
