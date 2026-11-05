param(
    [string]$BuildDir = 'build',
    [string]$Output = 'dist',
    [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$BuildRoot = [IO.Path]::GetFullPath((Join-Path $ProjectRoot $BuildDir))
$OutputRoot = [IO.Path]::GetFullPath((Join-Path $ProjectRoot $Output))
if (-not $OutputRoot.StartsWith($ProjectRoot + [IO.Path]::DirectorySeparatorChar)) {
    throw 'Output must be a dedicated directory inside EdgeEcho'
}
$BinRoot = if (Test-Path (Join-Path $BuildRoot "$Configuration/edgeecho.exe")) { Join-Path $BuildRoot $Configuration } else { $BuildRoot }
$Cli = Join-Path $BinRoot 'edgeecho_cli.exe'
$VersionText = & $Cli --version
if ($LASTEXITCODE -ne 0) { throw 'CLI version check failed' }
$Version = [regex]::Match(($VersionText -join ' '), '\d+\.\d+\.\d+').Value
if (-not $Version) { throw 'CLI did not report its version' }
$Expected = (& python -c "import sys;sys.path.insert(0,sys.argv[1]);from pathlib import Path;from validation_support import fingerprints,source_id;print(source_id(fingerprints(Path(sys.argv[2]))))" (Join-Path $ProjectRoot 'tools') $ProjectRoot).Trim()
if ($LASTEXITCODE -ne 0 -or -not ($Expected -match '^source-sha256:[0-9a-f]{64}$')) { throw 'Cannot compute current source identity' }
if (-not (($VersionText -join ' ') -match [regex]::Escape($Expected))) { throw 'CLI is stale for the current source' }
$GuiVersion = & (Join-Path $BinRoot 'edgeecho.exe') --version
if ($LASTEXITCODE -ne 0 -or -not (($GuiVersion -join ' ') -match [regex]::Escape($Expected))) { throw 'GUI and CLI do not share the current source build' }

$Name = "EdgeEcho-$Version-windows-x86_64"
$Stage = Join-Path $OutputRoot $Name
if (Test-Path $Stage) {
    if (-not (Test-Path (Join-Path $Stage '.edgeecho-package'))) { throw 'Refusing to replace an unrelated directory' }
    Remove-Item $Stage -Recurse -Force
}
New-Item $Stage -ItemType Directory -Force | Out-Null
New-Item (Join-Path $Stage 'bin') -ItemType Directory -Force | Out-Null
New-Item (Join-Path $Stage 'share/doc') -ItemType Directory -Force | Out-Null
New-Item (Join-Path $Stage 'share/licenses') -ItemType Directory -Force | Out-Null
Set-Content (Join-Path $Stage '.edgeecho-package') 'EdgeEcho runtime bundle'
Copy-Item (Join-Path $BinRoot 'edgeecho.exe') (Join-Path $Stage 'bin')
Copy-Item $Cli (Join-Path $Stage 'bin')
$Deploy = (Get-Command windeployqt.exe -ErrorAction Stop).Source
& $Deploy --release --compiler-runtime --no-translations --dir (Join-Path $Stage 'bin') (Join-Path $Stage 'bin/edgeecho.exe')
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }
# Do not permit a developer installation path to survive in Qt lookup settings.
Set-Content (Join-Path $Stage 'bin/qt.conf') "[Paths]`nPrefix=.`nPlugins=.`nLibraries=."
Copy-Item (Join-Path $ProjectRoot 'README.md') (Join-Path $Stage 'share/doc')
Copy-Item (Join-Path $ProjectRoot 'THIRD_PARTY_NOTICES.md') (Join-Path $Stage 'share/doc')
Copy-Item (Join-Path $ProjectRoot 'docs') (Join-Path $Stage 'share/doc/docs') -Recurse
Copy-Item (Join-Path $ProjectRoot 'third_party/licenses') (Join-Path $Stage 'share/licenses/edgeecho') -Recurse
$QtRoot = Split-Path (Split-Path $Deploy -Parent) -Parent
if (Test-Path (Join-Path $QtRoot 'LICENSES')) {
    Copy-Item (Join-Path $QtRoot 'LICENSES') (Join-Path $Stage 'share/licenses/Qt') -Recurse
}
@{
    version = ($VersionText -join ' ')
    semantic_version = $Version
    build_id = $Expected
    platform = 'Windows x86_64'
    packaging = 'native windeployqt runtime and compiler-runtime deployment'
    qt_source = 'https://download.qt.io/archive/qt/'
    scope = 'Native execution must be validated separately; this manifest alone is not a test result.'
} | ConvertTo-Json | Set-Content (Join-Path $Stage 'PACKAGE.json') -Encoding utf8
$Manifest = Get-ChildItem $Stage -File -Recurse | Sort-Object FullName | ForEach-Object {
    $Relative = $_.FullName.Substring($Stage.Length + 1).Replace('\','/')
    "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower())  $Relative"
}
$Manifest | Set-Content (Join-Path $Stage 'SHA256SUMS') -Encoding ascii
$Zip = Join-Path $OutputRoot "$Name.zip"
if (Test-Path $Zip) { Remove-Item $Zip -Force }
Compress-Archive -Path $Stage -DestinationPath $Zip
Write-Output $Zip
