# Build Win32 dinput8.dll proxy into this folder (and optionally the game dir).
param(
    [string]$OutDir = $PSScriptRoot,
    [string]$GameDir = "D:\Games\PSUClementine_22072025"
)

$ErrorActionPreference = "Stop"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msvc = if( Test-Path $vswhere ) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if( -not $msvc ) { $msvc = "D:\VSBuildTools" }
$vcvars = Join-Path $msvc "VC\Auxiliary\Build\vcvars32.bat"
if( -not ( Test-Path $vcvars ) ) { throw "vcvars32.bat not found" }

$src = Join-Path $PSScriptRoot "dinput8_proxy.cpp"
$def = Join-Path $PSScriptRoot "dinput8_proxy.def"
$out = Join-Path $OutDir "dinput8.dll"
$cmd = @"
call `"$vcvars`" >nul
cl /nologo /O2 /LD /EHsc /DWIN32 /DUNICODE /D_UNICODE `"$src`" /link /DEF:`"$def`" /OUT:`"$out`" dinput8.lib dxguid.lib user32.lib
"@
$bat = Join-Path $env:TEMP "build_dinput8_proxy.bat"
Set-Content -Path $bat -Value $cmd -Encoding ASCII
# vcvars prints a harmless "vswhere.exe is not recognized" on stderr; only cl's exit code counts.
$eap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& cmd /c $bat 2>&1 | ForEach-Object { "$_" }
$ErrorActionPreference = $eap
if( $LASTEXITCODE -ne 0 ) { throw "cl failed: $LASTEXITCODE" }

if( $GameDir -and ( Test-Path $GameDir ) ) {
    Copy-Item $out ( Join-Path $GameDir "dinput8.dll" ) -Force
    Write-Host "Deployed to $GameDir\dinput8.dll"
}
Write-Host "Built $out"
