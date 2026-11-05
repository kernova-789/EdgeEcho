param([switch]$CoreOnly)
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$BuildPath = Join-Path $ProjectRoot 'build'
$Gui = if ($CoreOnly) { 'OFF' } else { 'ON' }
cmake -S $ProjectRoot -B $BuildPath "-DEDGEECHO_BUILD_GUI=$Gui"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $BuildPath --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --test-dir $BuildPath -C Release --output-on-failure
exit $LASTEXITCODE
