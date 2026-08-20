# Builds the native Win32 DesktopBrightness.exe with LLVM-MinGW.
#
# -Os          optimize for size (this is a tray utility, not a hot loop)
# -mwindows    GUI subsystem, no console window
# -municode    wWinMain entry point
# -s           strip symbols
# --gc-sections + -ffunction-sections/-fdata-sections drop unreferenced code

$ErrorActionPreference = 'Stop'

$mingwRoot = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\llvm-mingw-20260616-ucrt-x86_64\bin"
$gcc = Join-Path $mingwRoot 'gcc.exe'

if (-not (Test-Path $gcc)) {
    # Fall back to whatever gcc is on PATH
    $onPath = Get-Command gcc -ErrorAction SilentlyContinue
    if ($onPath) { $gcc = $onPath.Source } else { throw "gcc not found. Install LLVM-MinGW or put gcc on PATH." }
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

Write-Host "Compiling $srcFile ..."
& $gcc @compilerArgs

if ($LASTEXITCODE -eq 0) {
    $size = [math]::Round((Get-Item $outFile).Length / 1KB, 1)
    Write-Host "SUCCESS: $outFile ($size KB)"
} else {
    Write-Host "ERROR: compilation failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}
