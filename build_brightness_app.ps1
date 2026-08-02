$cscPath = "C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
$srcFile = "H:\projects\Brightness Adjuster\DesktopBrightnessHotkeys.cs"
$outFile = "H:\projects\Brightness Adjuster\DesktopBrightness.exe"

$refs = @(
    "System.dll",
    "System.Windows.Forms.dll",
    "System.Drawing.dll"
)

$refArgs = ($refs | ForEach-Object { "/r:`"$($_)`"" }) -join " "

$cmd = "& `"$cscPath`" /target:winexe /out:`"$outFile`" /optimize+ $refArgs `"$srcFile`""
Write-Host "Compiling $srcFile..."

Invoke-Expression $cmd
if ($LASTEXITCODE -eq 0) {
    Write-Host "SUCCESS: DesktopBrightness compiled to $outFile!"
} else {
    Write-Host "ERROR: Compilation failed with exit code $LASTEXITCODE"
}
