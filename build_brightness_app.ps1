# Builds the native Win32 DesktopBrightness.exe with LLVM-MinGW (or any MinGW gcc).
#
# -Os          optimize for size (this is a tray utility, not a hot loop)
# -mwindows    GUI subsystem, no console window
# -municode    wWinMain entry point
# -s           strip symbols
# --gc-sections + -ffunction-sections/-fdata-sections drop unreferenced code

$ErrorActionPreference = 'Stop'

# Prefer an LLVM-MinGW installed by winget (any version), then any gcc on PATH.
$gcc = $null
$wingetPkgs = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
if (Test-Path $wingetPkgs) {
    $gcc = Get-ChildItem -Path $wingetPkgs -Filter 'gcc.exe' -Recurse -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -match 'llvm-mingw' } |
           Sort-Object FullName -Descending |
           Select-Object -First 1 -ExpandProperty FullName
}
if (-not $gcc) {
    $onPath = Get-Command gcc -ErrorAction SilentlyContinue
    if ($onPath) { $gcc = $onPath.Source } else { throw "gcc not found. Install LLVM-MinGW (winget install MartinStorsjo.LLVM-MinGW.UCRT) or put gcc on PATH." }
}

$srcFile = Join-Path $PSScriptRoot 'DesktopBrightness.c'
$outFile = Join-Path $PSScriptRoot 'DesktopBrightness.exe'

$compilerArgs = @(
    '-Os'
    '-mwindows'
    '-municode'
    '-ffunction-sections'
    '-fdata-sections'
    '-Wall'
    '-Wextra'
    $srcFile
    '-o', $outFile
    '-Wl,--gc-sections'
    '-s'
    '-ldxva2'
    '-luser32'
    '-lgdi32'
    '-lshell32'
    '-ladvapi32'
)

Write-Host "Compiling $srcFile with $gcc ..."
& $gcc @compilerArgs

if ($LASTEXITCODE -eq 0) {
    $size = [math]::Round((Get-Item $outFile).Length / 1KB, 1)
    Write-Host "SUCCESS: $outFile ($size KB)"
} else {
    Write-Host "ERROR: compilation failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}
