# Generates the app's tile and splash PNGs from the app's mark (tools/glyph.ps1).
param([string]$OutDir = "$PSScriptRoot\..\src\app\Assets")

. "$PSScriptRoot\glyph.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$Ink = [System.Drawing.Color]::FromArgb(255, 235, 238, 245)
$Accent = [System.Drawing.Color]::FromArgb(255, 90, 170, 255)

function New-Image([string]$name, [int]$w, [int]$h, [double]$glyphScale, [bool]$transparent) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    if ($transparent) { $g.Clear([System.Drawing.Color]::Transparent) }
    else { $g.Clear([System.Drawing.Color]::FromArgb(255, 16, 18, 24)) }
    Add-AppGlyph $g ($w / 2.0) ($h / 2.0) ([Math]::Min($w, $h) * $glyphScale) $Ink $Accent
    $g.Dispose()
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

New-Image "Square44x44Logo.png" 44 44 0.86 $true
New-Image "Square44x44Logo.targetsize-44_altform-unplated.png" 44 44 0.86 $true
New-Image "Square150x150Logo.png" 150 150 0.5 $false
New-Image "Wide310x150Logo.png" 310 150 0.5 $false
New-Image "StoreLogo.png" 50 50 0.8 $false
New-Image "SplashScreen.png" 620 300 0.45 $false
Get-ChildItem $OutDir | Select-Object Name, Length
